#include "Fathom/src/tbprobe.h"
#include "libchess/Position.h"
#include "utils.h"

struct pos
{
	uint64_t white;
	uint64_t black;
	uint64_t kings;
	uint64_t queens;
	uint64_t rooks;
	uint64_t bishops;
	uint64_t knights;
	uint64_t pawns;
	uint8_t castling;
	uint8_t rule50;
	uint8_t ep;
	bool turn;
};

static std::optional<libchess::Move> get_best_dtz_move(struct pos & pos, unsigned *results, unsigned wdl)
{
	unsigned selected_move = 0;
	int best_dtz = 1000;

	for(unsigned i = 0; results[i] != TB_RESULT_FAILED; i++) {
		if (TB_GET_WDL(results[i]) == wdl) {
			int dtz = TB_GET_DTZ(results[i]);

			if (dtz < best_dtz) {
				selected_move = results[i];
				best_dtz = dtz;
			}
		}
	}

	if (selected_move != 0) {
		unsigned from     = TB_GET_FROM(selected_move);
		unsigned to       = TB_GET_TO(selected_move);
		unsigned promotes = TB_GET_PROMOTES(selected_move);

		char to_type = 0x00;

		switch (promotes)
		{
			case TB_PROMOTES_QUEEN:
				to_type = 'q'; break;
			case TB_PROMOTES_ROOK:
				to_type = 'r'; break;
			case TB_PROMOTES_BISHOP:
				to_type = 'b'; break;
			case TB_PROMOTES_KNIGHT:
				to_type = 'n'; break;
		}

		std::string move_str = myformat("%c%c%c%c", 'a' + (from & 7), '1' + (from >> 3), 'a' + (to & 7), '1' + (to >> 3));

		if (to_type)
			move_str += to_type;

//			printf("found %s (%zu)\n", move_str.c_str(), move_str.size());

		return libchess::Move::from(move_str);
	}

	return {};
}

std::optional<libchess::Move> probe_fathom(libchess::Position & lpos)
{
	struct pos pos;
	pos.turn = lpos.side_to_move() == libchess::constants::WHITE;
	pos.white = lpos.color_bb(libchess::constants::WHITE);
	pos.black = lpos.color_bb(libchess::constants::BLACK);
	pos.kings = lpos.piece_type_bb(libchess::constants::KING);
	pos.queens = lpos.piece_type_bb(libchess::constants::QUEEN);
	pos.rooks = lpos.piece_type_bb(libchess::constants::ROOK);
	pos.bishops = lpos.piece_type_bb(libchess::constants::BISHOP);
	pos.knights = lpos.piece_type_bb(libchess::constants::KNIGHT);
	pos.pawns = lpos.piece_type_bb(libchess::constants::PAWN);
	auto crights = lpos.castling_rights();
	pos.castling = 0;
	if (crights.is_allowed(libchess::constants::WHITE_KINGSIDE))
		pos.castling = TB_CASTLING_K;
	if (crights.is_allowed(libchess::constants::WHITE_QUEENSIDE))
		pos.castling = TB_CASTLING_Q;
	if (crights.is_allowed(libchess::constants::BLACK_KINGSIDE))
		pos.castling = TB_CASTLING_k;
	if (crights.is_allowed(libchess::constants::BLACK_QUEENSIDE))
		pos.castling = TB_CASTLING_q;
	pos.rule50 = lpos.halfmoves();
	std::optional<libchess::Square> ep = lpos.enpassant_square();
	pos.ep = ep.has_value() ? ep.value() : 0;
#if 0
	printf("white: %lx\n", pos.white);
	printf("black: %lx\n", pos.black);
	printf("kings: %lx\n", pos.kings);
	printf("queens: %lx\n", pos.queens);
	printf("rooks: %lx\n", pos.rooks);
	printf("bishops: %lx\n", pos.bishops);
	printf("knights: %lx\n", pos.knights);
	printf("pawns: %lx\n", pos.pawns);
	printf("rule50: %d\n", pos.rule50);
	printf("castling: %d\n", pos.castling);
	printf("ep: %d\n", pos.ep);
	printf("turn: %d\n", pos.turn);
#endif
	uint64_t start_ts = get_ts_ms();
	unsigned results[TB_MAX_MOVES];
	unsigned res = tb_probe_root(pos.white, pos.black, pos.kings, pos.queens, pos.rooks, pos.bishops, pos.knights, pos.pawns, pos.rule50, pos.castling, pos.ep, pos.turn, results);
	uint64_t end_ts = get_ts_ms();

	printf("# took %dms\n", int(end_ts - start_ts));

	if (res == TB_RESULT_FAILED) {
		printf("# TB_RESULT_FAILED\n");
		return {};
	}

	//printf("# %d\n", TB_GET_WDL(res));

	std::optional<libchess::Move> m;
	
	m = get_best_dtz_move(pos, results, TB_WIN);
	if (m.has_value())
		return m;

	m = get_best_dtz_move(pos, results, TB_CURSED_WIN);
	if (m.has_value())
		return m;

	m = get_best_dtz_move(pos, results, TB_DRAW);
	if (m.has_value())
		return m;

	m = get_best_dtz_move(pos, results, TB_BLESSED_LOSS);
	if (m.has_value())
		return m;

	return {};
}
