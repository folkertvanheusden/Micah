#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <functional>
#include <jansson.h>
#include <libgen.h>
#include <map>
#include <netdb.h>
#include <numeric>
#include <omp.h>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>
#include <boost/mpi.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "libchess/Position.h"
#ifndef __ANDROID__
#include "libchess/Tuner.h"
#endif
#include "Fathom/src/tbprobe.h"
#include "tt.h"
#include "search.h"
#include "utils.h"
#include "eval_par.h"
#include "eval.h"
#include "psq.h"

namespace mpi = boost::mpi;
constexpr int mpi_tag = 9;

bool tune_program(std::string tune_file)
{
	std::ifstream fh(tune_file);

	if (fh.good() == false)
		return false;

	std::string line;
	while(std::getline(fh, line)) {
		if (line.at(0) == '#')
			continue;

		size_t is = line.find('=');

		std::string key = line.substr(0, is);
		int value = atoi(line.substr(is + 1).c_str());

		printf("Applying value %d to key '%s'\n", value, key.c_str());

		default_parameters.set_eval(key, value);
	}

	return true;
}

#ifndef __ANDROID__
void tune(std::string file)
{
	auto normalized_results = libchess::NormalizedResult<libchess::Position>::parse_epd(file, [](const std::string& fen) { return *libchess::Position::from_fen(fen); });

	printf("%zu EPDs loaded\n", normalized_results.size());

	std::vector<libchess::TunableParameter> tunable_parameters = default_parameters.get_tunable_parameters();
	printf("%zu parameters\n", tunable_parameters.size());

	libchess::Tuner<libchess::Position> tuner{normalized_results, tunable_parameters,
		[](libchess::Position& pos, const std::vector<libchess::TunableParameter> & params) {
			eval_par cur;

			for(auto & e : params)
				cur.set_eval(e.name(), e.value());

			meta_t meta;
			std::atomic_bool ef(false);
			end_indicator_t ei;
			ei.flag = false;
			meta.ei = &ei;
			meta.node_count = 0;
			meta.max_depth = 14;
			meta.tti = nullptr;
			meta.bco_1st_move = meta.bco_total = 0;
			memset(meta.hbt, 0x00, sizeof(meta.hbt)); // not changed in qs (but used by movesort!)

			libchess::Move rcm;
			libchess::Move killers[2];

			int score = qs(pos, -32767, 32767, &meta, 0, &rcm, cur);

			if (pos.side_to_move() != libchess::constants::WHITE)
				score = -score;

			return score;
		}};


	uint64_t start_ts = get_ts_ms();
	double start_error = tuner.error();
	tuner.tune();
	double end_error = tuner.error();
	uint64_t end_ts = get_ts_ms();

	time_t start = start_ts / 1000;
	char *str = ctime(&start), *lf = strchr(str, '\n');

	if (lf)
		*lf = 0x00;

	printf("# error: %.18f (%f%%), took: %fs, %s\n", end_error, sqrt(end_error) * 100.0, (end_ts - start_ts) / 1000.0, str);

	auto parameters = tuner.tunable_parameters();
	for(auto parameter : parameters)
		printf("%s=%d\n", parameter.name().c_str(), parameter.value());
	printf("#---\n");
}
#endif

libchess::Position *new_pos()
{
	return new libchess::Position(libchess::constants::STARTPOS_FEN);
}

void cluster_node(mpi::communicator & world, tt *const tti, const int n_threads)
{
	std::vector<ponder_pars *> *pp = nullptr;

	dolog("node %d out of %d", world.rank(), world.size());

	for(;;) {
		std::string data;
		world.recv(0, mpi_tag, data);

		if (pp)
			stop_ponder(pp);

		std::vector<uint64_t> history;

		tti->inc_age();

		dolog("Received request: %s", data.c_str());

		json_error_t error { 0 };
		json_t *json_in = json_loads(data.c_str(), 0, &error);

		libchess::Position pos = libchess::Position::from_fen(json_string_value(json_object_get(json_in, "position"))).value();
		// 90% thinktime to compensate for network etc
		int think_time = json_integer_value(json_object_get(json_in, "think_time")) * 0.9;
		int depth = json_integer_value(json_object_get(json_in, "depth"));
		int cluster_idx = json_integer_value(json_object_get(json_in, "idx"));

		result_t r = lazy_smp_search(cluster_idx, tti, n_threads, pos, think_time, depth);

		json_decref(json_in);

		// send result
		json_t *json_out = json_object();
		json_object_set(json_out, "position", json_string(pos.fen().c_str()));
		json_object_set(json_out, "move", json_string(r.m.to_str().c_str()));
		json_object_set(json_out, "depth", json_integer(r.depth));
		json_object_set(json_out, "score", json_integer(r.score));

		const char *json = json_dumps(json_out, JSON_COMPACT);
		std::string json_str = json;
		free((void *)json);

		json_decref(json_out);

		world.send(0, mpi_tag, json_str);

		pp = ponder(tti, pos, n_threads);
	}
}

void cluster_send_requests(mpi::communicator & world, const std::vector<std::string> *const nodes, const libchess::Position & p, const int think_time, const int depth)
{
	if (nodes == nullptr)
		return;

	dolog("send request to %zu nodes", nodes->size());

	std::string pos = p.fen();

	json_t *obj = json_object();
	json_object_set(obj, "position", json_string(pos.c_str()));
	json_object_set(obj, "think_time", json_integer(think_time));
	json_object_set(obj, "depth", json_integer(depth));

	const char *json = json_dumps(obj, JSON_COMPACT);
	std::string json_str = json;
	free((void *)json);
	json_decref(obj);

	broadcast(world, json_str, mpi_tag);
}

void cluster_receive_results(mpi::communicator & world, const libchess::Position & p, std::vector<result_t> *const results)
{
	std::string compare_pos = p.fen();

	for(int i=0; i<world.size() - 1; i++) {
		std::string buffer;
		world.recv(mpi::any_source, mpi_tag, buffer);

		json_error_t error { 0 };
		json_t *json_in = json_loads(buffer.c_str(), 0, &error);

		std::string recv_pos = json_string_value(json_object_get(json_in, "position"));

		if (recv_pos == compare_pos) {
			std::string move = json_string_value(json_object_get(json_in, "move"));
			int depth = json_integer_value(json_object_get(json_in, "depth"));
			int score = json_integer_value(json_object_get(json_in, "score"));

			result_t r;
			r.m     = libchess::Move::from(move).value();
			r.depth = depth;
			r.score = score;

			results->push_back(r);
		}
		else {
			fprintf(stderr, "Unexpected FEN\n");
		}

		json_decref(json_in);
	}
}

void help()
{
	printf("-H x   size of tt in MB\n");
	printf("-p     ponder\n");
	printf("-c x   number of threads\n");
	printf("-t x   tune using fen-file x\n");
	printf("-T x   while playing, tune program with file x (generated using -t)\n");
	printf("-l x   use log file x\n");
	printf("-x x   use log file tag x\n");
	printf("-s x   path to Syzygy files\n");
	printf("-n x   nodes\n");
	printf("-N x   is a node (listening on (UDP) port x)\n");
	printf("-L     also calculate with local CPU\n");
}

int main(int argc, char** argv)
{
	std::string syzygy_files;
	std::string tune_file = "tune.dat", tune_in;
	bool go_ponder = false;
	int hash_size = 256, n_threads = 1;
	std::vector<std::string> *nodes = nullptr;
	bool is_cluster_node = false;
	bool include_local = false;
	int c = -1;

	while((c = getopt(argc, argv, "s:l:c:H:pt:T:x:Lh")) != -1) {
		switch(c) {
			case 'L':
				include_local = true;
				break;

			case 's':
				syzygy_files = optarg;
				break;

			case 'l':
				set_logfile(optarg);
				break;

			case 'x':
				set_logfile_tag(optarg);
				break;

			case 'c':
				n_threads = atoi(optarg);
				break;

			case 'H':
				hash_size = atoi(optarg);
				break;

			case 'p':
				go_ponder = true;
				break;

			case 't':
				tune_in = optarg;
				break;

			case 'T':
				tune_file = optarg;
				break;

			case 'h':
				help();
				return 0;

			default:
				help();
				return 1;
		}
	}

#ifndef __ANDROID__
	omp_set_num_threads(n_threads);
#endif

	if (!tune_file.empty()) {
		if (tune_program(tune_file))
			dolog("load of tune file from %s succeeded", tune_file.c_str());
		else {
			std::string temp = std::string(dirname(argv[0])) + "/" + tune_file;

			if (tune_program(temp)) {
				tune_file = temp;
				dolog("load of tune file from %s succeeded", tune_file.c_str());
			}
			else {
				temp = "/usr/share/doc/micah-" VERSION "/" + tune_file;

				if (tune_program(temp)) {
					tune_file = temp;
					dolog("load of tune file from %s succeeded", tune_file.c_str());
				}
			}
		}
	}

#ifndef __ANDROID__
	if (!tune_in.empty()) {
		tune(tune_in);
		return 0;
	}
#endif

	if (!syzygy_files.empty()) {
		tb_init(syzygy_files.c_str());

		dolog("%d men syzygy", TB_LARGEST);
	}

	tt tti(hash_size * 1024ll * 1024ll);
	libchess::Position *p = new_pos();

	std::vector<ponder_pars *> *pp = nullptr;
	uint64_t pp_start_ts = 0;
	libchess::Move pp_last_move;

	mpi::environment env;
	mpi::communicator world;

	if (world.rank()) {
		cluster_node(world, &tti, n_threads);
		return 0;
	}

	for(;;) {
		char buffer[65536];
		if (!fgets(buffer, sizeof buffer, stdin))
			break;

		char *lf = strchr(buffer, '\n');
		if (lf)
			*lf = 0x00;

		dolog("> %s", buffer);

		std::vector<std::string> *parts = split(buffer, " ");

		if (parts->size() == 0) {
			delete parts;
			continue;
		}

		if (parts->at(0) == "uci") {
			printf("id name Micah\n");
			printf("id author Folkert van Heusden\n");
			printf("option name Threads type spin default %d min 1 max 4096\n", n_threads);
			printf("option name SyzygyPath type string default %s\n", syzygy_files.c_str());
			printf("option name Hash type spin default %d min 17 max 1048576\n", hash_size);
			printf("uciok\n");
		}
		else if (parts->at(0) == "setoption" && parts->size() >= 5) {
			if (parts->at(2) == "SyzygyPath") {
				if (!syzygy_files.empty())
					tb_free();

				syzygy_files = parts->at(4);

				tb_init(syzygy_files.c_str());

				printf("# %d men syzygy\n", TB_LARGEST);
			}
			else if (parts->at(2) == "Threads") {
				n_threads = atoi(parts->at(4).c_str());
			}
			else if (parts->at(2) == "Hash") {
				hash_size = atoi(parts->at(4).c_str()) * 1024ll * 1024ll;

				tti.resize(hash_size);
			}
		}
		else if (parts->at(0) == "ucinewgame") {
			if (pp) {
				stop_ponder(pp);
				pp = nullptr;
				pp_start_ts = 0;
			}

			delete p;
			p = new_pos();
		}
		else if (parts->at(0) == "position") {
			bool moves = false;

			for(size_t i=1; i<parts->size();) {
				if (parts->at(i) == "fen") {
					std::string fen;
					for(size_t f = i + 1; f<std::min(i + 7, parts->size()); f++)
						fen += parts->at(f) + " ";

					delete p;
					p = new libchess::Position(fen);

					i += 7;
				}
				else if (parts->at(i) == "startpos") {
					delete p;
					p = new_pos();
					i++;
				}
				else if (parts->at(i) == "moves") {
					moves = true;
					i++;
				}
				else if (moves) {
					while(i < parts->size() && parts -> at(i).size() < 4)
						i++;

					libchess::Move m = *libchess::Move::from(parts -> at(i));

					libchess::MoveList move_list = p->pseudo_legal_move_list();
					for(const libchess::Move move : move_list) {
						if (move.from_square() == m.from_square() &&
								move.to_square() == m.to_square() &&
								move.promotion_piece_type() == m.promotion_piece_type()) {
							m = move;
							break;
						}
					}

					if (i == parts->size() - 2)
						pp_last_move = m;

					p->make_move(m);

					i++;
				}
				else {
				}
			}
		}
		else if (parts->at(0) == "play" && parts->size() == 2) {
			int think_time = atoi(parts -> at(1).c_str());

			for(;;) {
				result_t r = lazy_smp_search(0, &tti, n_threads, *p, think_time, -1);

				printf("%s | %s\n", p->fen().c_str(), move_to_str(r.m).c_str());

				p->make_move(r.m);
			}
		}
		else if (parts->at(0) == "go") {
			int depth = -1;
			int moves_to_go = 40 - p->fullmoves();
			int w_time = 0, b_time = 0, w_inc = 0, b_inc = 0;
			bool timeSet = false;

			for(size_t i=1; i<parts->size(); i++) {
				if (parts->at(i) == "depth")
					depth = atoi(parts->at(++i).c_str());
				else if (parts->at(i) == "movetime") {
					w_time = b_time = atoi(parts->at(++i).c_str());
					timeSet = true;
				}
				else if (parts->at(i) == "wtime")
					w_time = atoi(parts->at(++i).c_str());
				else if (parts->at(i) == "btime")
					b_time = atoi(parts->at(++i).c_str());
				else if (parts->at(i) == "winc")
					w_inc = atoi(parts->at(++i).c_str());
				else if (parts->at(i) == "binc")
					b_inc = atoi(parts->at(++i).c_str());
				else if (parts->at(i) == "movestogo")
					moves_to_go = atoi(parts->at(++i).c_str());
			}

			int pp_think_sub = 0;

			if (pp) {
				result_t r = stop_ponder(pp);

				if (r.m != pp_last_move && r.m.value()) {
					pp_think_sub = get_ts_ms() - pp_start_ts;

					dolog("ponder hit %s/%d/%d: %dms", move_to_str(r.m).c_str(), r.depth, r.score, pp_think_sub);
				}

				pp = nullptr;
				pp_start_ts = 0;
			}

			int think_time = 0;
			if (timeSet)
				think_time = p->side_to_move() == libchess::constants::WHITE ? w_time : b_time;
			else {
				int cur_n_moves = moves_to_go <= 0 ? 40 : moves_to_go;

				int timeInc = p->side_to_move() == libchess::constants::WHITE ? w_inc : b_inc;

				int ms = p->side_to_move() == libchess::constants::WHITE ? w_time : b_time;
				think_time = (ms + (cur_n_moves - 1) * timeInc) / double(cur_n_moves + 7);

				int limit_duration_min = ms / 15;
				if (think_time > limit_duration_min)
					think_time = limit_duration_min;

				dolog("wb-time %d, think_time %d", ms, think_time);
			}

			if (pp_think_sub) {
				if (pp_think_sub > think_time)
					think_time -= pp_think_sub;
				else
					think_time = 1;
			}

			std::vector<uint64_t> history;

			tti.inc_age();

			cluster_send_requests(world, nodes, *p, think_time, depth);
			
			std::vector<result_t> results;

			if (include_local) {
				result_t r = lazy_smp_search(0, &tti, n_threads, *p, think_time, depth);
				dolog("local result: %s (depth %d, score %d)", r.m.to_str().c_str(), r.depth, r.score);
				results.push_back(r);

//				cluster_receive_results(world, nodes, *p, think_time * 0.1, &results);
			}
			else {
//				cluster_receive_results(world, nodes, *p, think_time, &results);
			}

			// find result with best values (depth & score)
			result_t final_r { { }, -1, -32767 };

			size_t chosen = 0;
			for(size_t i=0; i<results.size(); i++) {
				if (results.at(i).depth >= final_r.depth && results.at(i).score >= final_r.score) {
					final_r.depth = results.at(i).depth;
					final_r.score = results.at(i).score;
					final_r.m     = results.at(i).m;
					chosen = i;
				}
			}

			printf("bestmove %s\n", move_to_str(final_r.m).c_str());
			dolog("bestmove %s (idx: %zu, depth: %d, score: %d)", move_to_str(final_r.m).c_str(), chosen, final_r.depth, final_r.score);

			if (go_ponder) {
				pp = ponder(&tti, *p, n_threads);
				pp_start_ts = get_ts_ms();
			}
		}
		/////
		else if (parts->at(0) == "sdiv" && parts->size() == 2) {
			int depth = atoi(parts->at(1).c_str());

			for(auto move : p->legal_move_list()) {
				tt ttc(hash_size * 1024ll * 1024ll);

				p->make_move(move);

				std::string fen = p->fen();

				result_t r = lazy_smp_search(0, &ttc, 1, *p, -1, depth);

				p->unmake_move();

				std::cout << "Move: " << move << ", score: " << r.score << ", selected move: " << r.m << ", fen: " << fen << std::endl;
			}
		}
		else if (parts->at(0) == "eval") {
			printf("eval: %d\n", eval(*p, default_parameters));
		}
		else if (parts->at(0) == "fen") {
			printf("fen: %s\n", p->fen().c_str());
		}
		else if (parts->at(0) == "syzygy") {
			std::optional<libchess::Move> m = probe_fathom(*p);

			if (m.has_value())
				std::cout << "Move: " << m.value() << std::endl;
			else
				std::cout << "-None-" << std::endl;
		}
		/////
		else if (parts->at(0) == "isready") {
			printf("readyok\n");
		}
		else if (parts->at(0) == "quit") {
			delete parts;
			break;
		}
		else {
			printf("Invalid command: %s\n", buffer);
		}

		delete parts;

		fflush(NULL);
	}

	if (pp)
		stop_ponder(pp);

	delete p;

	if (!syzygy_files.empty())
		tb_free();

	return 0;
}
