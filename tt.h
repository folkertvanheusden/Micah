#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#define __PRAGMA_PACKED__ __attribute__ ((__packed__))

#define BC_PORT 2318

typedef enum { NOTVALID = 0, EXACT = 1, LOWERBOUND = 2, UPPERBOUND = 3 } tt_entry_flag;

typedef struct __PRAGMA_PACKED__
{
        uint64_t hash;

        union u {
                struct {
                        int16_t score;
                        uint8_t flags : 2;
                        uint8_t age : 6;
                        uint8_t depth : 8;
                        uint32_t m;
                } _data;

                uint64_t data;
        } data_;
} tt_entry;

#define N_TE_PER_HASH_GROUP 8

typedef struct __PRAGMA_PACKED__
{
        tt_entry entries[N_TE_PER_HASH_GROUP];
} tt_hash_group;

class tt
{
private:
	std::thread *th { nullptr };
	std::thread *th2 { nullptr };
	std::atomic_bool stop_flag { false };

        std::mutex pkts_lock;
        std::condition_variable pkts_cv;
        std::queue<tt_entry> pkts;

	tt_hash_group *entries { nullptr };
	uint64_t n_entries { 0 };

	int age { 0 };

	void cluster_tx();
	void cluster_rx();

public:
	tt(size_t size_in_bytes);
	~tt();

	void inc_age();

	void resize(size_t size_in_bytes);

	std::optional<tt_entry> lookup(const uint64_t board_hash);
	void store(const uint64_t hash, const tt_entry_flag f, const int d, const int score, const libchess::Move & m);
};
