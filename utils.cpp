#include <cstdarg>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include "libchess/Position.h"
#include "tt.h"
#include "utils.h"

#ifdef __CYGWIN__
// from https://stackoverflow.com/questions/40159892/using-asprintf-on-windows
#ifndef _vscprintf
/* For some reason, MSVC fails to honour this #ifndef. */
/* Hence function renamed to _vscprintf_so(). */
int _vscprintf_so(const char * format, va_list pargs) {
    int retval;
    va_list argcopy;
    va_copy(argcopy, pargs);
    retval = vsnprintf(NULL, 0, format, argcopy);
    va_end(argcopy);
    return retval;}
#endif // _vscprintf

#ifndef vasprintf
int vasprintf(char **strp, const char *fmt, va_list ap) {
    int len = _vscprintf_so(fmt, ap);
    if (len == -1) return -1;
    char *str = (char *)malloc((size_t) len + 1);
    if (!str) return -1;
    int r = vsnprintf(str, len + 1, fmt, ap); /* "secure" version of vsprintf */
    if (r == -1) return free(str), -1;
    *strp = str;
    return r;}
#endif // vasprintf

#ifndef asprintf
int asprintf(char *strp[], const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;}
#endif // asprintf
#endif

std::string __attribute__((format (printf, 1, 2) )) myformat(const char *const fmt, ...)
{
	char *buffer = NULL;
	va_list ap;

	va_start(ap, fmt);
	int len = vasprintf(&buffer, fmt, ap);
	va_end(ap);

	std::string result(buffer, len);
	free(buffer);

	return result;
}

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(x) do {\
	 std::ostringstream oss;\
	 oss << x;\
	 __android_log_write(ANDROID_LOG_DEBUG, "Micah", oss.str().c_str());\
} while(false)
#else
#define LOG(x)
#endif

const char *logfile = nullptr, *logfile_tag = nullptr;

void set_logfile(const char *new_file)
{
	logfile = new_file;
}

void set_logfile_tag(const char *tag)
{
	logfile_tag = tag;
}

void __attribute__((format (printf, 1, 2) )) dolog(const char *fmt, ...)
{
	struct timeval tv;
	gettimeofday(&tv, nullptr);

        char *buffer = NULL;
        va_list ap;

        va_start(ap, fmt);
        int len = vasprintf(&buffer, fmt, ap);
        va_end(ap);

	LOG(buffer); // ANDROID

	FILE *fh = logfile ? fopen(logfile, "a+") : nullptr;
	if (fh) {
		struct tm *tm = localtime(&tv.tv_sec);

		char tsbuf[128];
		strftime(tsbuf, sizeof(tsbuf), "%Y:%m:%d %H:%M:%S", tm);

		fprintf(fh, "%05d] %s.%06ld %s %s\n", getpid(), tsbuf, tv.tv_usec, logfile_tag ? logfile_tag : "", buffer);
		fclose(fh);
	}

        free(buffer);
}

std::vector<std::string> * split(std::string in, std::string splitter)
{
	std::vector<std::string> *out = new std::vector<std::string>;
	size_t splitter_size = splitter.size();

	for(;;)
	{
		size_t pos = in.find(splitter);
		if (pos == std::string::npos)
			break;

		std::string before = in.substr(0, pos);
		out -> push_back(before);

		size_t bytes_left = in.size() - (pos + splitter_size);
		if (bytes_left == 0)
		{
			out -> push_back("");
			return out;
		}

		in = in.substr(pos + splitter_size);
	}

	if (in.size() > 0)
		out -> push_back(in);

	return out;
}

bool is_move_in_movelist(libchess::MoveList & move_list, libchess::Move & m)
{
	return std::any_of(move_list.begin(), move_list.end(), [m](const libchess::Move & move) {
			return move.from_square() == m.from_square() &&
			move.to_square() == m.to_square() &&
			move.promotion_piece_type() == m.promotion_piece_type();
			});
}

std::vector<pv_entry_t> get_pv_from_tt(tt *tti, libchess::Position & pos_in, libchess::Move & cur_move)
{
	libchess::Position work = pos_in;

	std::vector<pv_entry_t> out;
	out.push_back({ work.hash(), cur_move });

	work.make_move(cur_move);

	for(int i=0; i<500; i++) {
		std::optional<tt_entry> te = tti->lookup(work.hash());
		if (!te.has_value())
			break;

		cur_move = libchess::Move(te.value().data_._data.m);

		libchess::MoveList cur_moves = work.legal_move_list();
		if (!is_move_in_movelist(cur_moves, cur_move))
			break;

		out.push_back({ work.hash(), cur_move });
		work.make_move(cur_move);

		if (work.is_repeat(3))
			break;
	}

	return out;
}

std::string move_to_str(const libchess::Move & cur_move)
{
	std::ostringstream stream;
	stream << cur_move;
	return stream.str();
}

std::string pv_to_string(std::vector<pv_entry_t> & pv)
{
	std::string rc;
	bool first = true;

	for(pv_entry_t & cur : pv) {
		if (first)
			first = false;
		else
			rc += " ";

		rc += move_to_str(cur.move);
	}

	return rc;
}

libchess::Move find_pv_move_by_hash(std::vector<pv_entry_t> & pv, uint64_t hash)
{
	for(pv_entry_t cur : pv) {
		if (cur.hash == hash)
			return cur.move;
	}

	return libchess::Move();
}

uint64_t get_ts_ms()
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);

	return ts.tv_sec * 1000ll + ts.tv_nsec / (1000 * 1000);
}

uint64_t get_us()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return uint64_t(ts.tv_sec) * uint64_t(1000 * 1000) + ts.tv_nsec / 1000;
}
