#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "libchess/Position.h"
#include "tt.h"
#include "utils.h"

tt::tt(size_t size_in_bytes)
{
	resize(size_in_bytes);

	th = new std::thread(&tt::cluster_tx, this);
	th2 = new std::thread(&tt::cluster_rx, this);
}

tt::~tt()
{
	delete [] entries;
}

void tt::resize(size_t size_in_bytes)
{
	if (entries)
		delete [] entries;

	n_entries = size_in_bytes / sizeof(tt_hash_group);

	entries = new tt_hash_group[n_entries]();

	age = 0;
}

void tt::inc_age()
{
	age++;

	remote_counts[0] = remote_counts[1] = 0;
	n_store = rstore = rstore_full = n_lookup = 0;
	store_tt_per_flag[0] = store_tt_per_flag[1] = store_tt_per_flag[2] = store_tt_per_flag[3] = 0;
	lu_tt_per_flag[0] = lu_tt_per_flag[1] = lu_tt_per_flag[2] = lu_tt_per_flag[3] = 0;
}

std::optional<tt_entry> tt::lookup(const uint64_t hash)
{
	uint64_t index = hash % n_entries;

	tt_entry *const e = entries[index].entries;

	n_lookup++;

	for(int i=0; i<N_TE_PER_HASH_GROUP; i++) {
		tt_entry *const cur = &e[i];

		if ((cur -> hash ^ cur -> data_.data) == hash) {
			cur->data_._data.age = age;
			cur->hash = hash ^ cur->data_.data;

			remote_counts[cur->data_._data.is_remote]++;
			lu_tt_per_flag[cur->data_._data.flags]++;

			return *cur;
		}
	}

	return { };
}

void tt::store(const uint64_t hash, const tt_entry_flag f, const int d, const int score, const libchess::Move & m, const bool emit, const bool is_remote)
{
	unsigned long int index = hash % n_entries; // FIXME is biased at the bottom of the list

	tt_entry *const e = entries[index].entries;

	int useSubIndex = -1, minDepth = 999, mdi = -1;

	for(int i=0; i<N_TE_PER_HASH_GROUP; i++)
	{
		if ((e[i].hash ^ e[i].data_.data) == hash) {
			if (e[i].data_._data.depth > d) {
				e[i].data_._data.age = age;
				e[i].hash = hash ^ e[i].data_.data;
				return;
			}
			if (f!=EXACT && e[i].data_._data.depth==d) {
				e[i].data_._data.age = age;
				e[i].hash = hash ^ e[i].data_.data;
				return;
			}

			useSubIndex = i;

			break;
		}

		if (e[i].data_._data.age != age)
			useSubIndex = i;
		else if (e[i].data_._data.depth < minDepth) {
			minDepth = e[i].data_._data.depth;
			mdi = i;
		}
	}

	if (useSubIndex == -1)
		useSubIndex = mdi;

	tt_entry *const cur = &e[useSubIndex];
	tt_entry::u n;
	n._data.score = int16_t(score);
	n._data.depth = uint8_t(d);
	n._data.is_remote = is_remote;
	n._data.flags = f;
	n._data.age = age;
	n._data.m = m.value();

	cur -> hash = hash ^ n.data;
	cur -> data_.data = n.data;

	n_store++;

	store_tt_per_flag[f]++;

	if (f == EXACT && emit) {
		rstore++;

		tt_entry copy = *cur;
		copy.hash = hash;

		pkts_lock.lock();
		while (pkts.size() > MAX_BC_Q_SIZE) {
			pkts.pop();  // forget old stuff
			rstore_full++;
		}
		pkts.push(copy);
		pkts_lock.unlock();

		pkts_cv.notify_one();
	}
}

int create_bc_socket(const bool b)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	int enable = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof enable) == -1)
		dolog("setsockopt(SO_BROADCAST) failed: %s", strerror(errno));

	if (b) {
		struct sockaddr_in addr { 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(BC_PORT);

		if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1)
			dolog("bind() failed: %s", strerror(errno));
	}

	return fd;
}

void broadcast_tx(const int fd, const uint8_t *msg, const size_t len)
{
	struct sockaddr_in s { 0 };

	s.sin_family = AF_INET;
	s.sin_port = htons(BC_PORT);
	s.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	if (sendto(fd, msg, len, 0, (struct sockaddr *)&s, sizeof(struct sockaddr_in)) == -1)
		dolog("sendto() failed: %s", strerror(errno));
}

void tt::cluster_tx()
{
	int fd = create_bc_socket(false);

	for(;!stop_flag;) {
                std::unique_lock<std::mutex> lck(pkts_lock);

                using namespace std::chrono_literals;

                while(pkts.empty() && !stop_flag)
                        pkts_cv.wait_for(lck, 500ms);

                if (pkts.empty() || stop_flag)
                        continue;

                const tt_entry pkt = pkts.front();
                pkts.pop();

                lck.unlock();

		broadcast_tx(fd, (uint8_t *)&pkt, sizeof pkt);
	}

	close(fd);
}

void tt::cluster_rx()
{
	int fd = create_bc_socket(true);
	struct pollfd fds[] = { { fd, POLLIN, 0 } };

	for(;!stop_flag;) {
		if (poll(fds, 1, 500) == -1) {
			perror("poll");
			break;
		}

		if (fds[0].revents) {
			struct sockaddr addr { 0 };
			socklen_t a_len = sizeof addr;
			tt_entry pkt;

			if (recvfrom(fd, &pkt, sizeof pkt, 0, &addr, &a_len) == -1)
				perror("recvfrom");

			store(pkt.hash, tt_entry_flag(pkt.data_._data.flags), pkt.data_._data.depth, pkt.data_._data.score, libchess::Move(pkt.data_._data.m), false, true);
		}
	}

	close(fd);
}

json_t *tt::get_stats()
{
	json_t *obj = json_object();

	json_object_set(obj, "tt-lookups", json_integer(n_lookup));
	json_object_set(obj, "tt-lu-local", json_integer(remote_counts[0]));
	json_object_set(obj, "tt-lu-remote", json_integer(remote_counts[1]));
	json_object_set(obj, "tt-lu-exact", json_integer(lu_tt_per_flag[EXACT]));
	json_object_set(obj, "tt-lu-lb", json_integer(lu_tt_per_flag[LOWERBOUND]));
	json_object_set(obj, "tt-lu-ub", json_integer(lu_tt_per_flag[UPPERBOUND]));

	json_object_set(obj, "tt-store", json_integer(n_store));
	json_object_set(obj, "tt-rstore", json_integer(rstore));
	json_object_set(obj, "tt-rstore-full", json_integer(rstore_full));
	json_object_set(obj, "tt-store-exact", json_integer(store_tt_per_flag[EXACT]));
	json_object_set(obj, "tt-store-lb", json_integer(store_tt_per_flag[LOWERBOUND]));
	json_object_set(obj, "tt-store-ub", json_integer(store_tt_per_flag[UPPERBOUND]));

	return obj;
}
