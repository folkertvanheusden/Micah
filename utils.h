#pragma once

#include "tt.h"

void set_logfile(const char *new_file);
void set_logfile_tag(const char *tag);
void __attribute__((format (printf, 1, 2) )) dolog(const char *fmt, ...);

std::string myformat(const char *const fmt, ...);
std::vector<std::string> * split(std::string in, std::string splitter);
bool is_move_in_movelist(libchess::MoveList & move_list, libchess::Move & m);

typedef struct {
	uint64_t hash;
	libchess::Move move;
} pv_entry_t;

std::vector<pv_entry_t> get_pv_from_tt(tt *tti, libchess::Position & pos, libchess::Move & cur_move);
std::string pv_to_string(std::vector<pv_entry_t> & pv);
libchess::Move find_pv_move_by_hash(std::vector<pv_entry_t> & pv, uint64_t hash);
std::string move_to_str(const libchess::Move & cur_move);

uint64_t get_ts_ms();
