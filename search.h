#include <condition_variable>
#include "eval_par.h"

typedef struct
{
	std::atomic_bool flag;
	std::condition_variable cv;
}
end_indicator_t;

typedef struct
{
	end_indicator_t *ei;
	long int node_count;
	int max_depth;

	tt *tti;

	long int bco_1st_move, bco_total;

	unsigned int hbt[2][64][64];
} meta_t;

typedef struct
{
	libchess::Move m;
	int depth, score;
} result_t;

int qs(libchess::Position & pos, int alpha, int beta, meta_t *meta, int qsdepth, libchess::Move *m, eval_par & pars = default_parameters);
int search(libchess::Position & pos, int depth, int alpha, int beta, libchess::Move *const m);
void search_it(std::vector<struct ponder_pars *> *td, int me, tt *tti, const int think_time, const int max_depth);
libchess::Move pick_one(libchess::Position & pos);

result_t lazy_smp_search(int cluster_idx, tt *tti, int n_threads, libchess::Position & pos, int think_time, int max_depth);

struct ponder_pars
{
	int cluster_idx, thread_nr, depth{ 1 };
	libchess::Position pos{ 0 };
	end_indicator_t ei { false };
	result_t result{ {}, -1, -32767 };
	std::thread *join_thread;
	const bool is_ponder;

	ponder_pars(const int node_nr, const int thread_nr, const libchess::Position & pos, const bool is_ponder) : cluster_idx(node_nr), thread_nr(thread_nr), pos(pos), is_ponder(is_ponder) {
	}
};
std::vector<struct ponder_pars *> * ponder(tt *tti, libchess::Position & pos, int n_threads);
result_t stop_ponder(std::vector<struct ponder_pars *> *pp);

std::optional<libchess::Move> probe_fathom(libchess::Position & lpos);
