#include <atomic>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "libchess/Position.h"
#include "Fathom/src/tbprobe.h"
#include "eval_par.h"
#include "eval.h"
#include "psq.h"
#include "tt.h"
#include "utils.h"
#include "search.h"
#include "utils.h"

#define WITH_LMR

class sort_movelist_compare
{
private:
        meta_t *const meta;
        libchess::Position *const p;
        const eval_par & pars;
        std::vector<libchess::Move> first_moves;
        std::optional<libchess::Square> previous_move_target;

public:
        sort_movelist_compare(meta_t *const meta, libchess::Position *const p, const eval_par & pars) : meta(meta), p(p), pars(pars) {
                if (p->previous_move())
                        previous_move_target = p->previous_move()->to_square();
        }

        void add_first_move(const libchess::Move move) {
                if (move.value())
                        first_moves.push_back(move);
        }

        int move_evaluater(const libchess::Move move) const {
                for(int i=0; i<first_moves.size(); i++) {
                        if (move == first_moves.at(i))
                                return INT_MAX - i;
                }

                auto piece_from = p->piece_on(move.from_square());

                int score = 0;

                if (p->is_promotion_move(move))
                        score += eval_piece(*move.promotion_piece_type(), pars) << 18;

                if (p->is_capture_move(move)) {
                        auto piece_to = p->piece_on(move.to_square());

                        // victim
                        score += (move.type() == libchess::Move::Type::ENPASSANT ? pars.tune_pawn.value() : eval_piece(piece_to->type(), pars)) << 18;

                        if (piece_from->type() != libchess::constants::KING)
                                score += (pars.tune_queen.value() - eval_piece(piece_from->type(), pars)) << 8;

//                      if (move.to_square() == previous_move_target)
//                              score += 100 << 8;
                }
                else {
                        score += meta -> hbt[p->side_to_move()][move.from_square()][move.to_square()] << 8;
                }

                score += -psq(move.from_square(), piece_from->color(), piece_from->type(), 0) + psq(move.to_square(), piece_from->color(), piece_from->type(), 0);

                return score;
        }
};

void sort_movelist(libchess::Position & pos, libchess::MoveList & move_list, sort_movelist_compare & smc)
{
	move_list.sort([&smc](const libchess::Move move) { return smc.move_evaluater(move); });
}

bool is_check(libchess::Position & pos)
{
	return pos.attackers_to(pos.piece_type_bb(libchess::constants::KING, !pos.side_to_move()).forward_bitscan(), pos.side_to_move());
}

libchess::MoveList gen_qs_moves(libchess::Position & pos)
{
	libchess::Color side = pos.side_to_move();

	if (pos.checkers_to(side))
		return pos.pseudo_legal_move_list();

	libchess::MoveList ml;
	pos.generate_promotions(ml, side);
	pos.generate_capture_moves(ml, side);

	return ml;
}

bool is_insufficient_material_draw(const libchess::Position & pos)
{
	if (pos.piece_type_bb(libchess::constants::PAWN, libchess::constants::WHITE).popcount() || 
		pos.piece_type_bb(libchess::constants::PAWN, libchess::constants::BLACK).popcount() ||
		pos.piece_type_bb(libchess::constants::ROOK, libchess::constants::WHITE).popcount() ||
		pos.piece_type_bb(libchess::constants::ROOK, libchess::constants::BLACK).popcount() ||
		pos.piece_type_bb(libchess::constants::QUEEN, libchess::constants::WHITE).popcount() ||
		pos.piece_type_bb(libchess::constants::QUEEN, libchess::constants::BLACK).popcount())
		return false;

	if (pos.piece_type_bb(libchess::constants::KNIGHT, libchess::constants::WHITE).popcount() || 
		pos.piece_type_bb(libchess::constants::KNIGHT, libchess::constants::BLACK).popcount() ||
		pos.piece_type_bb(libchess::constants::BISHOP, libchess::constants::WHITE).popcount() ||
		pos.piece_type_bb(libchess::constants::BISHOP, libchess::constants::BLACK).popcount())
		return false;

	return true;
}

int qs(libchess::Position & pos, int alpha, int beta, meta_t *meta, int qsdepth, libchess::Move *m, eval_par & pars)
{
	int best_score = -32767;

	meta->node_count++;

	if (pos.halfmoves() >= 100 || pos.is_repeat() || is_insufficient_material_draw(pos))
		return 0;

	bool in_check = pos.in_check();

	if (!in_check) {
		best_score = eval(pos, pars);

		if (best_score > alpha && best_score >= beta)
			return best_score;

		int BIG_DELTA = 975;
		if (pos.is_promotion_move(*pos.previous_move()))
			BIG_DELTA += 775;

		if (best_score < alpha - BIG_DELTA)
			return alpha;

		if (alpha < best_score)
			alpha = best_score;
	}

	auto move_list = gen_qs_moves(pos);
	int n_played = 0;

	sort_movelist_compare smc(meta, &pos, pars);
	sort_movelist(pos, move_list, smc);

	for(const auto move : move_list) {
		if (meta->ei->flag)
			break;

		if (!in_check && pos.is_capture_move(move)) {
			auto piece_to = pos.piece_on(move.to_square());
			int eval_target = move.type() == libchess::Move::Type::ENPASSANT ? pars.tune_pawn.value() : eval_piece(piece_to->type(), pars);

			auto piece_from = pos.piece_on(move.from_square());
			int eval_killer = eval_piece(piece_from->type(), pars);

			if (eval_killer > eval_target && pos.attackers_to(move.to_square(), piece_to->color()))
				continue;
		}

		libchess::Move curm{0};

		pos.make_move(move);

		if (pos.attackers_to(pos.piece_type_bb(libchess::constants::KING, !pos.side_to_move()).forward_bitscan(), pos.side_to_move())) {
			pos.unmake_move();
			continue;
		}

		n_played++;

		int score = -qs(pos, -beta, -alpha, meta, qsdepth + 1, &curm);

		pos.unmake_move();

		if (score > best_score) {
			best_score = score;
			*m = move;

			if (score > alpha) {
				alpha = score;

				if (score >= beta) {
					meta->bco_1st_move += n_played == 1;
					meta->bco_total++;
					break;
				}
			}
		}
	}

	if (n_played == 0) {
		if (in_check)
			best_score = -10000 + meta->max_depth + qsdepth;
		else if (best_score == -32767)
			best_score = eval(pos, pars);
	}

	return best_score;
}

int search(int cluster_idx, libchess::Position & pos, int depth, int alpha, int beta, bool is_null_move, meta_t *meta, libchess::Move *const m)
{
	if (depth == 0)
		return qs(pos, alpha, beta, meta, 0, m);

	meta->node_count++;

	const int start_alpha = alpha;

	bool is_root_position = meta->max_depth == depth;
	bool in_check = pos.in_check();

	if (!is_root_position && (pos.halfmoves() >= 100 || pos.is_repeat() || is_insufficient_material_draw(pos)))
		return 0;

	libchess::MoveList move_list;
	bool move_list_set = false;

	// TT //
	libchess::Move tt_move;
	uint64_t hash = pos.hash();
	std::optional<tt_entry> te = meta->tti->lookup(hash);

        if (te.has_value()) {
		tt_move = libchess::Move(te.value().data_._data.m);

		move_list = pos.legal_move_list();
		move_list_set = true;

		if (tt_move.value() && !is_move_in_movelist(move_list, tt_move))
			tt_move = libchess::Move();
		else if (te.value().data_._data.depth >= depth) {
			bool use = false;

			int csd = meta->max_depth - depth;
			int score = te.value().data_._data.score;
			int work_score = abs(score) > 9800 ? (score < 0 ? score + csd : score - csd) : score;

			if (te.value().data_._data.flags == EXACT)
				use = true;
			else if (te.value().data_._data.flags == LOWERBOUND && work_score >= beta)
				use = true;
			else if (te.value().data_._data.flags == UPPERBOUND && work_score <= alpha)
				use = true;

			if (use && (!is_root_position || tt_move.value())) {
				*m = tt_move;

				return work_score;
			}
		}
	}
	////////

	if (!is_root_position) {
		int staticeval = eval(pos, default_parameters);

		// static null pruning (reverse futility pruning)
		if (beta <= 9800) {
			if (depth == 1 && staticeval - default_parameters.tune_knight.value() > beta)
				return beta;

			if (depth == 2 && staticeval - default_parameters.tune_rook.value() > beta)
				return beta;

			if (depth == 3 && staticeval - default_parameters.tune_queen.value() > beta)
				depth--;
		}
	}

	int extension = in_check;

	// null move //
	int nm_reduce_depth = depth > 6 ? 4 : 3;
	if (depth >= nm_reduce_depth && !in_check && !is_root_position && !is_null_move) {
		pos.make_null_move();

		libchess::Move ignore;
		int nmscore = -search(cluster_idx, pos, depth - nm_reduce_depth, -beta, -beta + 1, true, meta, &ignore);

		pos.unmake_move();

                if (nmscore >= beta) {
			int verification = search(cluster_idx, pos, depth - nm_reduce_depth, beta - 1, beta, false, meta, &ignore);

			if (verification >= beta)
				return beta;
                }
	}
	///////////////

	// IID //
	libchess::Move iid_move;
	if (!is_null_move && tt_move.value() == 0 && depth >= 2) {
		if (abs(search(cluster_idx, pos, depth - 2, alpha, beta, is_null_move, meta, &iid_move)) > 9800)
			extension |= 1;
	}
	/////////

	if (!move_list_set)
		move_list = pos.legal_move_list();

	int best_score = -32767;
	libchess::Move best_move;

	sort_movelist_compare smc(meta, &pos, default_parameters);
	smc.add_first_move(tt_move);
	smc.add_first_move(iid_move);
	sort_movelist(pos, move_list, smc);

	int n_played = 0;

	const size_t lmr_start = !in_check && depth >= 2 ? 4 : 999;

	int nr = 0;
	for(const libchess::Move move : move_list) {
		if (meta->ei->flag)
			break;

		if (nr++ < cluster_idx && move_list.size() > cluster_idx)
			continue;

		pos.make_move(move);

		n_played++;

		int score = 0;

#ifdef WITH_LMR
		bool is_lmr = false;
		int new_depth = depth - 1;
		if (n_played >= lmr_start && !pos.is_capture_move(move) && !pos.is_promotion_move(move)) {
			is_lmr = true;

			if (n_played >= lmr_start + 2)
				new_depth = (depth - 1) * 2 / 3;
			else
				new_depth = depth - 2;
		}

		libchess::Move curm;

		bool check_after_move = pos.in_check();

		if (check_after_move)
			goto skip_lmr;

		score = -search(cluster_idx, pos, new_depth + extension, -beta, -alpha, is_null_move, meta, &curm);

		if (is_lmr && score > alpha) {
		skip_lmr:
			score = -search(cluster_idx, pos, depth - 1, -beta, -alpha, is_null_move, meta, &curm);
		}
#else
		score = -search(cluster_idx, pos, depth - 1 + extension, -beta, -alpha, is_null_move, meta, &curm);
#endif

		pos.unmake_move();

		if (score > best_score) {
			best_score = score;

			best_move = move;
			*m = move;

			if (score > alpha) {
				alpha = score;

				if (score >= beta) {
					meta->bco_1st_move += n_played == 1;
					meta->bco_total++;
					meta->bco_index += n_played;

					if (!pos.is_capture_move(move))
						meta -> hbt[pos.side_to_move()][move.from_square()][move.to_square()] += depth * depth;
					break;
				}
			}
		}
	}

	if (!n_played) {
		if (in_check)
			best_score = -10000 + (meta->max_depth - depth);
		else
			best_score = 0;
	}

	if (!meta->ei->flag) {
		tt_entry_flag flag = EXACT;

		if (best_score <= start_alpha)
			flag = UPPERBOUND;
		else if (best_score >= beta)
			flag = LOWERBOUND;

		meta->tti->store(hash, flag, depth, best_score, 
				best_score > start_alpha || tt_move.value() == 0 ? best_move : tt_move);
	}

	return best_score;
}

void timer(int think_time, end_indicator_t *const ei)
{
	int d = ei->flag;

	if (think_time > 0) {
		std::mutex m;
		std::unique_lock<std::mutex> lk(m);

		for(;;) {
			if (ei->cv.wait_for(lk, std::chrono::milliseconds(think_time)) == std::cv_status::timeout) {
				ei->flag = true;
				break;
			}

			if (ei->flag)
				break;
		}
	}
	else {
		ei->flag = true;
	}
}

libchess::Move pick_one(libchess::Position & pos)
{
	auto move_list = pos.legal_move_list();

	auto alt_list = move_list.values();

	return alt_list[rand() % move_list.size()];
}

bool time_management(int depth, std::chrono::time_point<std::chrono::system_clock> start_ts, int think_time, bool terminate_flag, bool is_thread)
{
	auto time_used_chrono = std::chrono::system_clock::now() - start_ts;
	uint64_t time_used_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_used_chrono).count();

	if (terminate_flag || (think_time > 0 && time_used_ms > think_time / 2 && is_thread == false)) {
		dolog("depth %d flag: %d think time: %d ms used: %ld ms", depth, terminate_flag, think_time, time_used_ms);
		return true;
	}

	return false;
}

void search_it(std::vector<struct ponder_pars *> *td, int me, tt *tti, const int think_time, const int max_depth)
{
	meta_t meta;
	meta.ei = &td->at(me)->ei;
	meta.node_count = 0;
	meta.bco_index = meta.bco_1st_move = meta.bco_total = 0;
	meta.tti = tti;
	memset(meta.hbt, 0x00, sizeof(meta.hbt));

	std::thread *t = nullptr;
	if (max_depth == -1)
		t = new std::thread(timer, think_time, meta.ei);
	
	auto start_ts = std::chrono::system_clock::now();

	int alpha = -32767, beta = 32767;
	bool selected_move = false;

	int add_alpha = 75, add_beta = 75;

	td->at(me)->depth = 1;

	for(;;) {
		meta.max_depth = td->at(me)->depth;

		if (time_management(td->at(me)->depth, start_ts, think_time, meta.ei->flag, me != 0))
			break;

		libchess::Move cur_move;
		int score = search(td->at(me)->cluster_idx, td->at(me)->pos, td->at(me)->depth, alpha, beta, false, &meta, &cur_move);

		if (meta.ei->flag) { // FIXME logging
			dolog("abort flag set");
			break;
		}

		if (score <= alpha) {
			beta = (alpha + beta) / 2;
			alpha = score - add_alpha;
			if (alpha < -10000)
				alpha = -10000;
			add_alpha += add_alpha / 15 + 1;
		}
		else if (score >= beta) {
			alpha = (alpha + beta) / 2;
			beta = score + add_beta;
			if (beta > 10000)
				beta = 10000;
			add_beta += add_beta / 15 + 1;
		}
		else {
			alpha = score - add_alpha;
			if (alpha < -10000)
				alpha = -10000;

			beta = score + add_beta;
			if (beta > 10000)
				beta = 10000;

			selected_move = true;
			td->at(me)->result.m = cur_move;
			td->at(me)->result.score = score;
			td->at(me)->result.depth = td->at(me)->depth;

			auto now_ts = std::chrono::system_clock::now();
			std::chrono::duration<double> diff_ts = now_ts - start_ts;

			if (diff_ts.count()) {
				auto pv = get_pv_from_tt(tti, td->at(me)->pos, cur_move);

				std::string moves = pv_to_string(pv);

				if (td->at(me)->is_ponder == false && me == 0) {
					printf("info depth %d score cp %d nodes %ld time %d nps %d pv %s\n", td->at(me)->depth, score, meta.node_count, int(diff_ts.count() * 1000), int(meta.node_count / diff_ts.count()), moves.c_str());
					dolog("info depth %d score cp %d nodes %ld time %d nps %d pv %s", td->at(me)->depth, score, meta.node_count, int(diff_ts.count() * 1000), int(meta.node_count / diff_ts.count()), moves.c_str());
					fflush(nullptr);
				}
			}

			if (td->size() <= 3 || me == 0)
				td->at(me)->depth++;
			else {
				for(;;) {
					td->at(me)->depth++;

					int dc = 0;
					for(int i=0; i<td->size(); i++)
						dc += td->at(me)->depth == td->at(i)->depth;

					if (dc < td->size() / 2)
						break;
				}
			}

			add_alpha = 75;
			add_beta = 75;

			if (max_depth > 0 && td->at(me)->depth > max_depth) {
				meta.ei->flag = true;
				meta.ei->cv.notify_one();
				break;
			}
		}
	}

	dolog("thread stops %d", me);

#ifndef __ANDROID__
	if (meta.bco_total && me == 0) {
		auto time_used_chrono = std::chrono::system_clock::now() - start_ts;
		uint64_t time_used_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_used_chrono).count();

		printf("info string beta cut-off after %f avg moves. # bco moves: %d, %% of total: %.2f%%, %f/s\n", meta.bco_index / double(meta.bco_total), meta.bco_total, meta.bco_total * 100.0 / meta.node_count, meta.bco_total * 1000.0 / time_used_ms);
	}
#endif

	if (t) {
		meta.ei->flag = true;
		meta.ei->cv.notify_one();

		t->join();

		delete t;
	}

	if (!selected_move)
		td->at(me)->result.m = pick_one(td->at(me)->pos);
}

result_t lazy_smp_search(int cluster_idx, tt *tti, int n_threads, libchess::Position & pos, int think_time, int max_depth)
{
	std::vector<ponder_pars *> td;

	for(int i=0; i<n_threads; i++)
		td.push_back(new ponder_pars(cluster_idx, i, pos, false));

	for(int i=0; i<n_threads; i++)
		td.at(i)->join_thread = new std::thread(search_it, &td, i, tti, think_time, max_depth);

	result_t r{ { }, -1, -32767 };

	std::optional<libchess::Move> syzygy_move = probe_fathom(pos);

	if (syzygy_move.has_value()) {
		dolog("SYZYGY HIT");
		for(auto & t : td) {
			t->ei.flag = true;
			t->ei.cv.notify_all();
		}
	}

	bool first = true;

	for(auto & t : td) {
		t->join_thread->join();

                if (first) {
                        first = false;
 
                        for(auto & t : td) {
                                t->ei.flag = true;
                                t->ei.cv.notify_all();
                        }
                }

		if (t->result.depth >= r.depth && t->result.score >= r.score) {
			r.depth = t->result.depth;
			r.score = t->result.score;
			r.m = t->result.m;
		}

		delete t->join_thread;
	}

	if (syzygy_move.has_value()) {
		dolog("SYZYGY HIT: %s (org: %s)", move_to_str(syzygy_move.value()).c_str(), move_to_str(r.m).c_str());
		r.m = syzygy_move.value();
		r.score = 0;
		r.depth = 0;
	}

	return r;
}

void ponder_search_it_wrapper(tt *tti, std::vector<struct ponder_pars *> *pp, int nr)
{
	search_it(pp, nr, tti, -1, 255);
}

std::vector<struct ponder_pars *> * ponder(tt *tti, libchess::Position & pos, int n_threads)
{
	std::vector<struct ponder_pars *> *vpp = new std::vector<struct ponder_pars *>;

	for(int i=0; i<n_threads; i++)
		vpp->push_back(new ponder_pars(0, i, pos, true));

	for(int i=0; i<n_threads; i++)
		vpp->at(i)->join_thread = new std::thread(ponder_search_it_wrapper, tti, vpp, i);

	return vpp;
}

result_t stop_ponder(std::vector<struct ponder_pars *> *vpp)
{
	result_t r { { }, -1, -32767 };

	for(auto & pp : *vpp){
		pp->ei.flag = true;
		pp->ei.cv.notify_all();
	}

	for(auto & pp : *vpp){
		pp->join_thread->join();
		delete pp->join_thread;

		if (pp->result.depth >= r.depth && pp->result.score >= r.score) {
			r.depth = pp->result.depth;
			r.score = pp->result.score;
			r.m = pp->result.m;
		}

		delete pp;
	}

	delete vpp;

	dolog("ponder move %s score %d depth %d", move_to_str(r.m).c_str(), r.score, r.depth);

	return r;
}
