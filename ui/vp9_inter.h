// vp9_inter.h -- VP9 inter-frame ENTROPY decode over a whole clip (Faze 3.4a).
//
// Ported from the grp-toolkit oracle tools/vp9_inter.py (VP9 spec §6.4.11-6.4.20,
// §6.5, §9.3.2). Decodes the inter frames' bitstream (the part that consumes
// bits): is_inter / reference frames / inter modes / motion vectors, the MV
// reference search (find_mv_refs §6.5) the entropy decode depends on, and the
// non-key-frame partition / mode contexts. No pixels / motion compensation --
// that is 3.4b (ui/vp9_inter_pixels.h).
//
// Oracle (same shape as 3.2c): each tile of every frame must finish on zero
// exit padding. Tracks the 4 frame contexts (frame_parallel=1 => no backward
// adaptation, only forward compressed-header updates) and the previous frame's
// MV grids, exactly as a decoder must. Pure presentation; no determinism impact.
#ifndef BWGAME_UI_VP9_INTER_H
#define BWGAME_UI_VP9_INTER_H

#include "vp9_entropy.h"
#include "webm.h"   // superframe_split + block list for the clip driver

namespace bwgame {
namespace vp9 {

enum { IF_NONE = -1, IF_INTRA = 0, IF_LAST = 1, IF_GOLDEN = 2, IF_ALTREF = 3 };
enum { MODE_NEARESTMV = 10, MODE_NEARMV = 11, MODE_ZEROMV = 12, MODE_NEWMV = 13 };
enum { MV_BORDER = 128, MI_SIZE = 8, INTER_CLASS0_SIZE = 2, COMPANDED_MVREF_THRESH = 8,
       BORDERINPIXELS = 160, INTERP_EXTEND = 4, MVREF_NEIGHBOURS = 8, MAX_MV_REF_CANDIDATES = 2,
       MVJ_ZERO = 0, MVJ_HNZVZ = 1, MVJ_HZVNZ = 2, MVJ_HNZVNZ = 3 };

struct mv_t {
	int r = 0, c = 0;
	bool operator==(const mv_t& o) const { return r == o.r && c == o.c; }
	bool operator!=(const mv_t& o) const { return !(*this == o); }
};

namespace itab {
static const T::I1 INTER_MODE_TREE = { -2, 2, 0, 4, -1, -3 };
static const T::I1 INTERP_FILTER_TREE = { 0, 2, -1, -2 };
static const T::I1 MV_JOINT_TREE = { 0, 2, -1, 4, -2, -3 };
static const T::I1 MV_CLASS_TREE = { 0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9, -10 };
static const T::I1 MV_FR_TREE = { 0, 2, -1, 4, -2, -3 };
static const int MV_REF_BLOCKS[13][8][2] = {
	{{-1,0},{0,-1},{-1,-1},{-2,0},{0,-2},{-2,-1},{-1,-2},{-2,-2}},
	{{-1,0},{0,-1},{-1,-1},{-2,0},{0,-2},{-2,-1},{-1,-2},{-2,-2}},
	{{-1,0},{0,-1},{-1,-1},{-2,0},{0,-2},{-2,-1},{-1,-2},{-2,-2}},
	{{-1,0},{0,-1},{-1,-1},{-2,0},{0,-2},{-2,-1},{-1,-2},{-2,-2}},
	{{0,-1},{-1,0},{1,-1},{-1,-1},{0,-2},{-2,0},{-2,-1},{-1,-2}},
	{{-1,0},{0,-1},{-1,1},{-1,-1},{-2,0},{0,-2},{-1,-2},{-2,-1}},
	{{-1,0},{0,-1},{-1,1},{1,-1},{-1,-1},{-3,0},{0,-3},{-3,-3}},
	{{0,-1},{-1,0},{2,-1},{-1,-1},{-1,1},{0,-3},{-3,0},{-3,-3}},
	{{-1,0},{0,-1},{-1,2},{-1,-1},{1,-1},{-3,0},{0,-3},{-3,-3}},
	{{-1,1},{1,-1},{-1,2},{2,-1},{-1,-1},{-3,0},{0,-3},{-3,-3}},
	{{0,-1},{-1,0},{4,-1},{-1,2},{-1,-1},{0,-3},{-3,0},{2,-1}},
	{{-1,0},{0,-1},{-1,4},{2,-1},{-1,-1},{-3,0},{0,-3},{-1,2}},
	{{-1,3},{3,-1},{-1,4},{4,-1},{-1,-1},{-1,0},{0,-1},{-1,6}},
};
static const int MODE_2_COUNTER[14] = { 9,9,9,9,9,9,9,9,9,9,0,0,3,1 };
static const int COUNTER_TO_CONTEXT[19] = { 2,3,4,1,3,9,0,9,9,5,5,9,5,9,9,9,9,9,6 };
static const int IDX_N_COLUMN_TO_SUBBLOCK[4][2] = { {1,2},{1,3},{3,2},{3,3} };
} // namespace itab

static const mv_t ZERO_MV{ 0, 0 };

static inline bool use_mv_hp(const mv_t& mv) {
	return ((mv.r < 0 ? -mv.r : mv.r) >> 3) < COMPANDED_MVREF_THRESH &&
	       ((mv.c < 0 ? -mv.c : mv.c) >> 3) < COMPANDED_MVREF_THRESH;
}

// Full per-frame probability set, loaded from a frame context, forward-updated.
struct frame_probs {
	T::I6 coef; T::I3 tx_probs; T::I1 skip;
	T::I2 inter_mode, interp_filter; T::I1 is_inter, comp_mode; T::I2 single_ref;
	T::I1 comp_ref; T::I2 y_mode, uv_mode, partition;
	T::I1 mv_joint, mv_sign; T::I2 mv_class; T::I1 mv_class0_bit; T::I2 mv_bits;
	T::I3 mv_class0_fr; T::I2 mv_fr; T::I1 mv_class0_hp, mv_hp;
	int tx_mode = 0;
	int reference_mode = 0, comp_fixed_ref = 0, comp_var_ref[2] = { 0, 0 };

	static frame_probs make_default() {
		frame_probs p;
		p.coef = T::DEFAULT_COEF_PROBS; p.tx_probs = T::DEFAULT_TX_PROBS; p.skip = T::DEFAULT_SKIP_PROB;
		p.inter_mode = T::DEFAULT_INTER_MODE_PROBS; p.interp_filter = T::DEFAULT_INTERP_FILTER_PROBS;
		p.is_inter = T::DEFAULT_IS_INTER_PROB; p.comp_mode = T::DEFAULT_COMP_MODE_PROB;
		p.single_ref = T::DEFAULT_SINGLE_REF_PROB; p.comp_ref = T::DEFAULT_COMP_REF_PROB;
		p.y_mode = T::DEFAULT_Y_MODE_PROBS; p.uv_mode = T::DEFAULT_UV_MODE_PROBS;
		p.partition = T::DEFAULT_PARTITION_PROBS;
		p.mv_joint = T::DEFAULT_MV_JOINT_PROBS; p.mv_sign = T::DEFAULT_MV_SIGN_PROB;
		p.mv_class = T::DEFAULT_MV_CLASS_PROBS; p.mv_class0_bit = T::DEFAULT_MV_CLASS0_BIT_PROB;
		p.mv_bits = T::DEFAULT_MV_BITS_PROB; p.mv_class0_fr = T::DEFAULT_MV_CLASS0_FR_PROBS;
		p.mv_fr = T::DEFAULT_MV_FR_PROBS; p.mv_class0_hp = T::DEFAULT_MV_CLASS0_HP_PROB;
		p.mv_hp = T::DEFAULT_MV_HP_PROB;
		return p;
	}
};

static inline int inter_du(bool_decoder& bd, int cur) {
	if (bd.read_bool(252)) return inv_remap_prob(decode_term_subexp_v(bd), cur);
	return cur;
}
static inline int inter_umv(bool_decoder& bd, int cur) {
	if (bd.read_bool(252)) return (bd.read_literal(7) << 1) | 1;
	return cur;
}
static inline void setup_compound_reference_mode(frame_probs& p, const int* sb) {
	if (sb[IF_LAST] == sb[IF_GOLDEN]) { p.comp_fixed_ref = IF_ALTREF; p.comp_var_ref[0] = IF_LAST; p.comp_var_ref[1] = IF_GOLDEN; }
	else if (sb[IF_LAST] == sb[IF_ALTREF]) { p.comp_fixed_ref = IF_GOLDEN; p.comp_var_ref[0] = IF_LAST; p.comp_var_ref[1] = IF_ALTREF; }
	else { p.comp_fixed_ref = IF_LAST; p.comp_var_ref[0] = IF_GOLDEN; p.comp_var_ref[1] = IF_ALTREF; }
}

// Apply compressed-header forward updates to p; return non-zero exit padding.
static inline int inter_parse_compressed(frame_probs& p, const header_t& h,
                                         const uint8_t* data, size_t datalen, int start) {
	bool_decoder bd(data, datalen, start, h.header_size_in_bytes);
	bool frame_is_intra = h.keyframe || h.intra_only;
	const int* sign_bias = h.ref_frame_sign_bias;
	int hp_mv = h.allow_high_precision_mv;

	int tx_mode;
	if (h.lossless) tx_mode = 0;
	else { tx_mode = bd.read_literal(2); if (tx_mode == 3) tx_mode += bd.read_literal(1); }
	p.tx_mode = tx_mode;
	if (tx_mode == TX_MODE_SELECT) {
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 1; ++j) p.tx_probs[1][i][j] = inter_du(bd, p.tx_probs[1][i][j]);
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) p.tx_probs[2][i][j] = inter_du(bd, p.tx_probs[2][i][j]);
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 3; ++j) p.tx_probs[3][i][j] = inter_du(bd, p.tx_probs[3][i][j]);
	}
	int max_tx = TX_MODE_TO_BIGGEST[tx_mode];
	for (int txsz = 0; txsz <= max_tx; ++txsz)
		if (bd.read_literal(1))
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) for (int k = 0; k < 6; ++k) {
				int maxl = (k == 0) ? 3 : 6;
				for (int l = 0; l < maxl; ++l) for (int m = 0; m < 3; ++m)
					p.coef[txsz][i][j][k][l][m] = inter_du(bd, p.coef[txsz][i][j][k][l][m]);
			}
	for (int i = 0; i < SKIP_CONTEXTS; ++i) p.skip[i] = inter_du(bd, p.skip[i]);

	p.reference_mode = SINGLE_REFERENCE; p.comp_fixed_ref = 0; p.comp_var_ref[0] = p.comp_var_ref[1] = 0;
	if (!frame_is_intra) {
		for (int i = 0; i < INTER_MODE_CONTEXTS; ++i) for (int j = 0; j < INTER_MODES - 1; ++j) p.inter_mode[i][j] = inter_du(bd, p.inter_mode[i][j]);
		if (h.interp_filter == INTERP_SWITCHABLE)
			for (int j = 0; j < INTERP_FILTER_CONTEXTS; ++j) for (int i = 0; i < SWITCHABLE_FILTERS - 1; ++i) p.interp_filter[j][i] = inter_du(bd, p.interp_filter[j][i]);
		for (int i = 0; i < IS_INTER_CONTEXTS; ++i) p.is_inter[i] = inter_du(bd, p.is_inter[i]);
		bool comp_allowed = false;
		for (int i = 1; i < REFS_PER_FRAME; ++i) if (sign_bias[i + 1] != sign_bias[1]) comp_allowed = true;
		if (comp_allowed) {
			if (bd.read_literal(1) == 0) p.reference_mode = SINGLE_REFERENCE;
			else { p.reference_mode = (bd.read_literal(1) == 0) ? COMPOUND_REFERENCE : REFERENCE_MODE_SELECT; setup_compound_reference_mode(p, sign_bias); }
		}
		if (p.reference_mode == REFERENCE_MODE_SELECT)
			for (int i = 0; i < COMP_MODE_CONTEXTS; ++i) p.comp_mode[i] = inter_du(bd, p.comp_mode[i]);
		if (p.reference_mode != COMPOUND_REFERENCE)
			for (int i = 0; i < REF_CONTEXTS; ++i) { p.single_ref[i][0] = inter_du(bd, p.single_ref[i][0]); p.single_ref[i][1] = inter_du(bd, p.single_ref[i][1]); }
		if (p.reference_mode != SINGLE_REFERENCE)
			for (int i = 0; i < REF_CONTEXTS; ++i) p.comp_ref[i] = inter_du(bd, p.comp_ref[i]);
		for (int i = 0; i < BLOCK_SIZE_GROUPS; ++i) for (int j = 0; j < INTRA_MODES - 1; ++j) p.y_mode[i][j] = inter_du(bd, p.y_mode[i][j]);
		for (int i = 0; i < PARTITION_CONTEXTS; ++i) for (int j = 0; j < PARTITION_TYPES - 1; ++j) p.partition[i][j] = inter_du(bd, p.partition[i][j]);
		for (int j = 0; j < MV_JOINTS - 1; ++j) p.mv_joint[j] = inter_umv(bd, p.mv_joint[j]);
		for (int i = 0; i < 2; ++i) {
			p.mv_sign[i] = inter_umv(bd, p.mv_sign[i]);
			for (int j = 0; j < MV_CLASSES - 1; ++j) p.mv_class[i][j] = inter_umv(bd, p.mv_class[i][j]);
			p.mv_class0_bit[i] = inter_umv(bd, p.mv_class0_bit[i]);
			for (int j = 0; j < MV_OFFSET_BITS; ++j) p.mv_bits[i][j] = inter_umv(bd, p.mv_bits[i][j]);
		}
		for (int i = 0; i < 2; ++i) {
			for (int j = 0; j < INTER_CLASS0_SIZE; ++j) for (int k = 0; k < MV_FR_SIZE - 1; ++k) p.mv_class0_fr[i][j][k] = inter_umv(bd, p.mv_class0_fr[i][j][k]);
			for (int k = 0; k < MV_FR_SIZE - 1; ++k) p.mv_fr[i][k] = inter_umv(bd, p.mv_fr[i][k]);
		}
		if (hp_mv) for (int i = 0; i < 2; ++i) { p.mv_class0_hp[i] = inter_umv(bd, p.mv_class0_hp[i]); p.mv_hp[i] = inter_umv(bd, p.mv_hp[i]); }
	}
	if (bd.bits_left < 0) return -1;
	return bd.exit_padding_nonzero();
}

// Previous decoded frame's grids, for UsePrevFrameMvs.
struct prev_frame_t {
	std::vector<std::vector<std::array<mv_t, 2>>> mv;
	std::vector<std::vector<std::array<int, 2>>> ref;
	int w = 0, h = 0, show = 0;
	bool valid = false;
};

struct frame_decoder : keyframe_decoder {
	frame_probs& fp;
	bool frame_is_intra;
	int ref_sign_bias[4];
	int allow_hp;
	int interp;            // INTERP_SWITCHABLE or 0..3
	int reference_mode, comp_fixed_ref, comp_var_ref[2];
	const prev_frame_t* prev;
	bool use_prev_mvs = false;

	// inter grids (per 8x8 cell)
	std::vector<std::vector<int>> y_modes, interp_grid;
	std::vector<std::vector<std::array<int, 2>>> ref_grid;
	std::vector<std::vector<std::array<mv_t, 2>>> mv_grid;
	std::vector<std::vector<std::array<std::array<mv_t, 4>, 2>>> submv_grid;

	// per-block scratch
	int is_inter = 0;
	int ref_frame[2] = { IF_INTRA, IF_NONE };
	mv_t mv[2], block_mvs[2][4];
	int interp_filter = 0;
	int left_ref[2], above_ref[2];
	bool left_intra = false, above_intra = false, left_single = false, above_single = false;
	mv_t nearest_mv[2], near_mv[2], best_mv[2];
	int mode_context[4] = { 0, 0, 0, 0 };
	int ref_mv_count = 0;
	mv_t ref_list_mv[2];
	mv_t cand_mv[2];
	int cand_frame[2];

	frame_decoder(const header_t& hdr, frame_probs& probs, bool fi, const prev_frame_t* pv)
		: keyframe_decoder(hdr, probs_t()), fp(probs), frame_is_intra(fi), prev(pv) {
		// refill the base entropy probs (coef/tx/skip/tx_mode) from frame_probs
		p.coef = fp.coef; p.tx_probs = fp.tx_probs; p.skip = fp.skip; p.tx_mode = fp.tx_mode;
		for (int i = 0; i < 4; ++i) ref_sign_bias[i] = hdr.ref_frame_sign_bias[i];
		allow_hp = hdr.allow_high_precision_mv;
		interp = hdr.has_interp ? hdr.interp_filter : 0;
		reference_mode = fp.reference_mode; comp_fixed_ref = fp.comp_fixed_ref;
		comp_var_ref[0] = fp.comp_var_ref[0]; comp_var_ref[1] = fp.comp_var_ref[1];
		int R = mi_rows, C = mi_cols;
		y_modes.assign(R, std::vector<int>(C, DC_PRED));
		interp_grid.assign(R, std::vector<int>(C, 0));
		ref_grid.assign(R, std::vector<std::array<int, 2>>(C, { IF_INTRA, IF_NONE }));
		mv_grid.assign(R, std::vector<std::array<mv_t, 2>>(C, { ZERO_MV, ZERO_MV }));
		submv_grid.assign(R, std::vector<std::array<std::array<mv_t, 4>, 2>>(C));
	}

	// non-key frames use partition_probs (not kf_partition_probs)
	int read_partition(bool_decoder& bd, int r, int c, int bsize, int num8x8, bool has_rows, bool has_cols) override {
		int bsl = T::MI_WIDTH_LOG2[bsize];
		int boffset = T::MI_WIDTH_LOG2[BLOCK_64X64] - bsl;
		int above = 0, left = 0;
		for (int i = 0; i < num8x8; ++i) { above |= above_part[c + i]; left |= left_part[(r + i) & 7]; }
		above = (above & (1 << boffset)) ? 1 : 0;
		left = (left & (1 << boffset)) ? 1 : 0;
		int ctx = bsl * 4 + left * 2 + above;
		const T::I1& probs = frame_is_intra ? T::KF_PARTITION_PROBS[ctx] : fp.partition[ctx];
		auto P = [&](int node) { int n2 = (has_rows && has_cols) ? node : (has_cols ? 1 : 2); return probs[n2]; };
		if (has_rows && has_cols) return read_tree(bd, T::PARTITION_TREE, P);
		if (has_cols) return read_tree(bd, T::COLS_PARTITION_TREE, P);
		if (has_rows) return read_tree(bd, T::ROWS_PARTITION_TREE, P);
		return PARTITION_SPLIT;
	}

	void intra_mode_info(bool_decoder& bd) override {
		if (frame_is_intra) {
			keyframe_decoder::intra_mode_info(bd);
			is_inter = 0;
			ref_frame[0] = IF_INTRA; ref_frame[1] = IF_NONE;
			mv[0] = mv[1] = ZERO_MV;
			for (int b = 0; b < 4; ++b) block_mvs[0][b] = block_mvs[1][b] = ZERO_MV;
			interp_filter = 0;
		} else inter_frame_mode_info(bd);
	}

	void block(bool_decoder& bd, int r, int c, int subsize) override {
		keyframe_decoder::block(bd, r, c, subsize);
		int num_h = T::NUM_8X8_H[subsize], num_w = T::NUM_8X8_W[subsize];
		for (int y = 0; y < num_h; ++y) for (int x = 0; x < num_w; ++x)
			if (r + y < mi_rows && c + x < mi_cols) {
				y_modes[r + y][c + x] = y_mode;
				ref_grid[r + y][c + x] = { ref_frame[0], ref_frame[1] };
				mv_grid[r + y][c + x] = { block_mvs[0][3], block_mvs[1][3] };
				for (int rl = 0; rl < 2; ++rl) for (int b = 0; b < 4; ++b) submv_grid[r + y][c + x][rl][b] = block_mvs[rl][b];
				interp_grid[r + y][c + x] = interp_filter;
			}
	}

	void inter_frame_mode_info(bool_decoder& bd) {
		int r = MiRow, c = MiCol;
		left_ref[0] = AvailL ? ref_grid[r][c - 1][0] : IF_INTRA;
		left_ref[1] = AvailL ? ref_grid[r][c - 1][1] : IF_NONE;
		above_ref[0] = AvailU ? ref_grid[r - 1][c][0] : IF_INTRA;
		above_ref[1] = AvailU ? ref_grid[r - 1][c][1] : IF_NONE;
		left_intra = left_ref[0] <= IF_INTRA;
		above_intra = above_ref[0] <= IF_INTRA;
		left_single = left_ref[1] <= IF_NONE;
		above_single = above_ref[1] <= IF_NONE;
		sub_mode = { { DC_PRED, DC_PRED, DC_PRED, DC_PRED } };
		cur_skip = read_skip(bd);
		is_inter = read_is_inter(bd);
		cur_tx_size = read_tx_size(bd, (!cur_skip) || (!is_inter));
		if (is_inter) inter_block_mode_info(bd);
		else intra_block_mode_info(bd);
	}

	int read_is_inter(bool_decoder& bd) {
		int ctx;
		if (AvailU && AvailL) ctx = (left_intra && above_intra) ? 3 : (int)(left_intra || above_intra);
		else if (AvailU || AvailL) ctx = 2 * (int)(AvailU ? above_intra : left_intra);
		else ctx = 0;
		return bd.read_bool(fp.is_inter[ctx]);
	}

	void intra_block_mode_info(bool_decoder& bd) {
		ref_frame[0] = IF_INTRA; ref_frame[1] = IF_NONE;
		mv[0] = mv[1] = ZERO_MV;
		for (int b = 0; b < 4; ++b) block_mvs[0][b] = block_mvs[1][b] = ZERO_MV;
		interp_filter = 0;
		int mode = DC_PRED;
		if (MiSize >= BLOCK_8X8) {
			const T::I1& probs = fp.y_mode[T::SIZE_GROUP_LOOKUP[MiSize]];
			mode = read_tree(bd, T::INTRA_MODE_TREE, [&](int n) { return probs[n]; });
			y_mode = mode;
			sub_mode = { { mode, mode, mode, mode } };
		} else {
			int n4w = T::NUM_4X4_W[MiSize], n4h = T::NUM_4X4_H[MiSize];
			sub_mode = { { DC_PRED, DC_PRED, DC_PRED, DC_PRED } };
			const T::I1& probs = fp.y_mode[0];
			int idy = 0;
			while (idy < 2) { int idx = 0; while (idx < 2) {
				mode = read_tree(bd, T::INTRA_MODE_TREE, [&](int n) { return probs[n]; });
				for (int y2 = 0; y2 < n4h; ++y2) for (int x2 = 0; x2 < n4w; ++x2) sub_mode[(idy + y2) * 2 + idx + x2] = mode;
				idx += n4w; } idy += n4h; }
			y_mode = mode;
		}
		const T::I1& uvp = fp.uv_mode[y_mode];
		uv_mode = read_tree(bd, T::INTRA_MODE_TREE, [&](int n) { return uvp[n]; });
	}

	void inter_block_mode_info(bool_decoder& bd) {
		read_ref_frames(bd);
		nearest_mv[0] = nearest_mv[1] = near_mv[0] = near_mv[1] = best_mv[0] = best_mv[1] = ZERO_MV;
		for (int j = 0; j < 2; ++j)
			if (ref_frame[j] > IF_INTRA) { find_mv_refs(ref_frame[j], -1); find_best_ref_mvs(j); }
		int is_compound = ref_frame[1] > IF_INTRA ? 1 : 0;
		if (MiSize >= BLOCK_8X8) {
			const T::I1& probs = fp.inter_mode[mode_context[ref_frame[0]]];
			int im = read_tree(bd, itab::INTER_MODE_TREE, [&](int n) { return probs[n]; });
			y_mode = MODE_NEARESTMV + im;
		}
		if (interp == INTERP_SWITCHABLE) interp_filter = read_interp_filter(bd);
		else interp_filter = interp;
		mv[0] = mv[1] = ZERO_MV;
		for (int b = 0; b < 4; ++b) block_mvs[0][b] = block_mvs[1][b] = ZERO_MV;
		if (MiSize < BLOCK_8X8) {
			int n4w = T::NUM_4X4_W[MiSize], n4h = T::NUM_4X4_H[MiSize];
			int idy = 0;
			while (idy < 2) { int idx = 0; while (idx < 2) {
				const T::I1& probs = fp.inter_mode[mode_context[ref_frame[0]]];
				int im = read_tree(bd, itab::INTER_MODE_TREE, [&](int n) { return probs[n]; });
				y_mode = MODE_NEARESTMV + im;
				if (y_mode == MODE_NEARESTMV || y_mode == MODE_NEARMV)
					for (int j = 0; j < 1 + is_compound; ++j) append_sub8x8_mvs(idy * 2 + idx, j);
				assign_mv(bd, is_compound);
				for (int y2 = 0; y2 < n4h; ++y2) for (int x2 = 0; x2 < n4w; ++x2) {
					int b = (idy + y2) * 2 + idx + x2;
					for (int rl = 0; rl < 1 + is_compound; ++rl) block_mvs[rl][b] = mv[rl];
				}
				idx += n4w; } idy += n4h; }
		} else {
			assign_mv(bd, is_compound);
			for (int rl = 0; rl < 1 + is_compound; ++rl) for (int b = 0; b < 4; ++b) block_mvs[rl][b] = mv[rl];
		}
	}

	int read_interp_filter(bool_decoder& bd) {
		int r = MiRow, c = MiCol;
		int left = (AvailL && left_ref[0] > IF_INTRA) ? interp_grid[r][c - 1] : 3;
		int above = (AvailU && above_ref[0] > IF_INTRA) ? interp_grid[r - 1][c] : 3;
		int ctx;
		if (left == above) ctx = left;
		else if (left == 3 && above != 3) ctx = above;
		else if (left != 3 && above == 3) ctx = left;
		else ctx = 3;
		const T::I1& probs = fp.interp_filter[ctx];
		return read_tree(bd, itab::INTERP_FILTER_TREE, [&](int n) { return probs[n]; });
	}

	void read_ref_frames(bool_decoder& bd) {
		int comp;
		if (reference_mode == REFERENCE_MODE_SELECT) comp = bd.read_bool(fp.comp_mode[comp_mode_ctx()]);
		else comp = (reference_mode == COMPOUND_REFERENCE) ? 1 : 0;
		if (comp) {
			int idx = ref_sign_bias[comp_fixed_ref];
			int cr = bd.read_bool(fp.comp_ref[comp_ref_ctx()]);
			ref_frame[idx] = comp_fixed_ref;
			ref_frame[1 - idx] = comp_var_ref[cr];
		} else {
			int p1 = bd.read_bool(fp.single_ref[single_ref_ctx(0)][0]);
			if (p1) { int p2 = bd.read_bool(fp.single_ref[single_ref_ctx(1)][1]); ref_frame[0] = p2 ? IF_ALTREF : IF_GOLDEN; ref_frame[1] = IF_NONE; }
			else { ref_frame[0] = IF_LAST; ref_frame[1] = IF_NONE; }
		}
	}

	int comp_mode_ctx() {
		bool au = AvailU, al = AvailL; int cf = comp_fixed_ref;
		if (au && al) {
			if (above_single && left_single) return (int)((above_ref[0] == cf) ^ (left_ref[0] == cf));
			if (above_single) return 2 + (int)(above_ref[0] == cf || above_intra);
			if (left_single) return 2 + (int)(left_ref[0] == cf || left_intra);
			return 4;
		}
		if (au) return above_single ? (int)(above_ref[0] == cf) : 3;
		if (al) return left_single ? (int)(left_ref[0] == cf) : 3;
		return 1;
	}

	int comp_ref_ctx() {
		int fix = ref_sign_bias[comp_fixed_ref], var = 1 - fix;
		const int* cv = comp_var_ref;
		bool au = AvailU, al = AvailL, ai = above_intra, li = left_intra, asg = above_single, lsg = left_single;
		const int* ar = above_ref; const int* lr = left_ref;
		if (au && al) {
			if (ai && li) return 2;
			if (li) return 1 + 2 * (int)((asg ? ar[0] : ar[var]) != cv[1]);
			if (ai) return 1 + 2 * (int)((lsg ? lr[0] : lr[var]) != cv[1]);
			int vrfa = asg ? ar[0] : ar[var];
			int vrfl = lsg ? lr[0] : lr[var];
			if (vrfa == vrfl && cv[1] == vrfa) return 0;
			if (lsg && asg) {
				if ((vrfa == comp_fixed_ref && vrfl == cv[0]) || (vrfl == comp_fixed_ref && vrfa == cv[0])) return 4;
				return vrfa == vrfl ? 3 : 1;
			}
			if (lsg || asg) {
				int vrfc = lsg ? vrfa : vrfl;
				int rfs = asg ? vrfa : vrfl;
				if (vrfc == cv[1] && rfs != cv[1]) return 1;
				if (rfs == cv[1] && vrfc != cv[1]) return 2;
				return 4;
			}
			return vrfa == vrfl ? 4 : 2;
		}
		if (au) { if (ai) return 2; if (asg) return 3 * (int)(ar[0] != cv[1]); return 4 * (int)(ar[var] != cv[1]); }
		if (al) { if (li) return 2; if (lsg) return 3 * (int)(lr[0] != cv[1]); return 4 * (int)(lr[var] != cv[1]); }
		return 2;
	}

	int single_ref_ctx(int which) {
		bool au = AvailU, al = AvailL, ai = above_intra, li = left_intra, asg = above_single, lsg = left_single;
		const int* ar = above_ref; const int* lr = left_ref;
		const int L = IF_LAST, G = IF_GOLDEN, A = IF_ALTREF;
		if (which == 0) {
			if (au && al) {
				if (ai && li) return 2;
				if (li) { if (asg) return 4 * (int)(ar[0] == L); return 1 + (int)(ar[0] == L || ar[1] == L); }
				if (ai) { if (lsg) return 4 * (int)(lr[0] == L); return 1 + (int)(lr[0] == L || lr[1] == L); }
				if (asg && lsg) return 2 * (int)(ar[0] == L) + 2 * (int)(lr[0] == L);
				if (!asg && !lsg) return 1 + (int)(ar[0] == L || ar[1] == L || lr[0] == L || lr[1] == L);
				int rfs = asg ? ar[0] : lr[0];
				int crf1 = asg ? lr[0] : ar[0];
				int crf2 = asg ? lr[1] : ar[1];
				if (rfs == L) return 3 + (int)(crf1 == L || crf2 == L);
				return (int)(crf1 == L || crf2 == L);
			}
			if (au) { if (ai) return 2; if (asg) return 4 * (int)(ar[0] == L); return 1 + (int)(ar[0] == L || ar[1] == L); }
			if (al) { if (li) return 2; if (lsg) return 4 * (int)(lr[0] == L); return 1 + (int)(lr[0] == L || lr[1] == L); }
			return 2;
		}
		if (au && al) {
			if (ai && li) return 2;
			if (li) { if (asg) return ar[0] == L ? 3 : 4 * (int)(ar[0] == G); return 1 + 2 * (int)(ar[0] == G || ar[1] == G); }
			if (ai) { if (lsg) return lr[0] == L ? 3 : 4 * (int)(lr[0] == G); return 1 + 2 * (int)(lr[0] == G || lr[1] == G); }
			if (asg && lsg) {
				if (ar[0] == L && lr[0] == L) return 3;
				if (ar[0] == L) return 4 * (int)(lr[0] == G);
				if (lr[0] == L) return 4 * (int)(ar[0] == G);
				return 2 * (int)(ar[0] == G) + 2 * (int)(lr[0] == G);
			}
			if (!asg && !lsg) {
				if (ar[0] == lr[0] && ar[1] == lr[1]) return 3 * (int)(ar[0] == G || ar[1] == G);
				return 2;
			}
			int rfs = asg ? ar[0] : lr[0];
			int crf1 = asg ? lr[0] : ar[0];
			int crf2 = asg ? lr[1] : ar[1];
			if (rfs == G) return 3 + (int)(crf1 == G || crf2 == G);
			if (rfs == A) return (int)(crf1 == G || crf2 == G);
			return 1 + 2 * (int)(crf1 == G || crf2 == G);
		}
		if (au) { if (ai || (ar[0] == L && asg)) return 2; if (asg) return 4 * (int)(ar[0] == G); return 3 * (int)(ar[0] == G || ar[1] == G); }
		if (al) { if (li || (lr[0] == L && lsg)) return 2; if (lsg) return 4 * (int)(lr[0] == G); return 3 * (int)(lr[0] == G || lr[1] == G); }
		return 2;
	}

	void assign_mv(bool_decoder& bd, int is_compound) {
		mv[0] = mv[1] = ZERO_MV;
		for (int i = 0; i < 1 + is_compound; ++i) {
			if (y_mode == MODE_NEWMV) read_mv(bd, i);
			else if (y_mode == MODE_NEARESTMV) mv[i] = nearest_mv[i];
			else if (y_mode == MODE_NEARMV) mv[i] = near_mv[i];
			else mv[i] = ZERO_MV;
		}
	}

	void read_mv(bool_decoder& bd, int ref) {
		int uh = (allow_hp && use_mv_hp(best_mv[ref])) ? 1 : 0;
		int joint = read_tree(bd, itab::MV_JOINT_TREE, [&](int n) { return fp.mv_joint[n]; });
		int d0 = 0, d1 = 0;
		if (joint == MVJ_HZVNZ || joint == MVJ_HNZVNZ) d0 = read_mv_component(bd, 0, uh);
		if (joint == MVJ_HNZVZ || joint == MVJ_HNZVNZ) d1 = read_mv_component(bd, 1, uh);
		mv[ref] = mv_t{ best_mv[ref].r + d0, best_mv[ref].c + d1 };
	}

	int read_mv_component(bool_decoder& bd, int comp, int use_hp) {
		int sign = bd.read_bool(fp.mv_sign[comp]);
		int cls = read_tree(bd, itab::MV_CLASS_TREE, [&](int n) { return fp.mv_class[comp][n]; });
		int mag;
		if (cls == 0) {
			int bit = bd.read_bool(fp.mv_class0_bit[comp]);
			int fr = read_tree(bd, itab::MV_FR_TREE, [&](int n) { return fp.mv_class0_fr[comp][bit][n]; });
			int hp = use_hp ? bd.read_bool(fp.mv_class0_hp[comp]) : 1;
			mag = ((bit << 3) | (fr << 1) | hp) + 1;
		} else {
			int d = 0;
			for (int i = 0; i < cls; ++i) d |= bd.read_bool(fp.mv_bits[comp][i]) << i;
			mag = INTER_CLASS0_SIZE << (cls + 2);
			int fr = read_tree(bd, itab::MV_FR_TREE, [&](int n) { return fp.mv_fr[comp][n]; });
			int hp = use_hp ? bd.read_bool(fp.mv_hp[comp]) : 1;
			mag += ((d << 3) | (fr << 1) | hp) + 1;
		}
		return sign ? -mag : mag;
	}

	bool is_inside(int r, int c) { return 0 <= r && r < mi_rows && MiColStart <= c && c < MiColEnd; }

	void find_mv_refs(int ref_frame_, int block) {
		ref_mv_count = 0;
		ref_list_mv[0] = ref_list_mv[1] = ZERO_MV;
		int different_ref_found = 0, context_counter = 0;
		const int (*search)[2] = itab::MV_REF_BLOCKS[MiSize];
		int r = MiRow, c = MiCol;
		for (int i = 0; i < 2; ++i) {
			int cr = r + search[i][0], cc = c + search[i][1];
			if (is_inside(cr, cc)) {
				different_ref_found = 1;
				context_counter += itab::MODE_2_COUNTER[y_modes[cr][cc]];
				for (int j = 0; j < 2; ++j)
					if (ref_grid[cr][cc][j] == ref_frame_) { add_mv_ref_list(get_sub_block_mv(cr, cc, j, search[i][1], block)); break; }
			}
		}
		for (int i = 2; i < MVREF_NEIGHBOURS; ++i) {
			int cr = r + search[i][0], cc = c + search[i][1];
			if (is_inside(cr, cc)) { different_ref_found = 1; if_same_ref_frame_add(cr, cc, ref_frame_, 0); }
		}
		if (use_prev_mvs) if_same_ref_frame_add(r, c, ref_frame_, 1);
		if (different_ref_found)
			for (int i = 0; i < MVREF_NEIGHBOURS; ++i) {
				int cr = r + search[i][0], cc = c + search[i][1];
				if (is_inside(cr, cc)) if_diff_ref_frame_add(cr, cc, ref_frame_, 0);
			}
		if (use_prev_mvs) if_diff_ref_frame_add(r, c, ref_frame_, 1);
		mode_context[ref_frame_] = itab::COUNTER_TO_CONTEXT[context_counter];
		for (int i = 0; i < MAX_MV_REF_CANDIDATES; ++i)
			ref_list_mv[i] = mv_t{ clamp_mv_row(ref_list_mv[i].r, MV_BORDER), clamp_mv_col(ref_list_mv[i].c, MV_BORDER) };
	}

	void get_block_mv(int cr, int cc, int rl, int use_prev) {
		if (use_prev) { cand_mv[rl] = prev->mv[cr][cc][rl]; cand_frame[rl] = prev->ref[cr][cc][rl]; }
		else { cand_mv[rl] = mv_grid[cr][cc][rl]; cand_frame[rl] = ref_grid[cr][cc][rl]; }
	}
	mv_t get_sub_block_mv(int cr, int cc, int rl, int delta_col, int block) {
		int idx = block >= 0 ? itab::IDX_N_COLUMN_TO_SUBBLOCK[block][delta_col == 0 ? 1 : 0] : 3;
		return submv_grid[cr][cc][rl][idx];
	}
	void add_mv_ref_list(const mv_t& cmv) {
		if (ref_mv_count >= 2) return;
		if (ref_mv_count > 0 && cmv == ref_list_mv[0]) return;
		ref_list_mv[ref_mv_count] = cmv; ++ref_mv_count;
	}
	void if_same_ref_frame_add(int cr, int cc, int ref_frame_, int use_prev) {
		cand_mv[0] = cand_mv[1] = ZERO_MV; cand_frame[0] = cand_frame[1] = IF_INTRA;
		for (int j = 0; j < 2; ++j) { get_block_mv(cr, cc, j, use_prev); if (cand_frame[j] == ref_frame_) { add_mv_ref_list(cand_mv[j]); return; } }
	}
	void if_diff_ref_frame_add(int cr, int cc, int ref_frame_, int use_prev) {
		cand_mv[0] = cand_mv[1] = ZERO_MV; cand_frame[0] = cand_frame[1] = IF_INTRA;
		for (int j = 0; j < 2; ++j) get_block_mv(cr, cc, j, use_prev);
		bool mvs_same = cand_mv[0] == cand_mv[1];
		if (cand_frame[0] > IF_INTRA && cand_frame[0] != ref_frame_) scale_and_add(0, ref_frame_);
		if (cand_frame[1] > IF_INTRA && cand_frame[1] != ref_frame_ && !mvs_same) scale_and_add(1, ref_frame_);
	}
	void scale_and_add(int rl, int ref_frame_) {
		int cand = cand_frame[rl]; mv_t m = cand_mv[rl];
		if (ref_sign_bias[cand] != ref_sign_bias[ref_frame_]) m = mv_t{ -m.r, -m.c };
		add_mv_ref_list(m);
	}
	int clamp_mv_row(int mvec, int border) {
		int bh = T::NUM_8X8_H[MiSize];
		int top = -((MiRow * MI_SIZE) * 8);
		int bottom = ((mi_rows - bh - MiRow) * MI_SIZE) * 8;
		return std::max(top - border, std::min(bottom + border, mvec));
	}
	int clamp_mv_col(int mvec, int border) {
		int bw = T::NUM_8X8_W[MiSize];
		int left = -((MiCol * MI_SIZE) * 8);
		int right = ((mi_cols - bw - MiCol) * MI_SIZE) * 8;
		return std::max(left - border, std::min(right + border, mvec));
	}
	void find_best_ref_mvs(int rl) {
		int border = (BORDERINPIXELS - INTERP_EXTEND) << 3;
		for (int i = 0; i < MAX_MV_REF_CANDIDATES; ++i) {
			int dr = ref_list_mv[i].r, dc = ref_list_mv[i].c;
			if (!allow_hp || !use_mv_hp(ref_list_mv[i])) {
				if (dr & 1) dr += dr > 0 ? -1 : 1;
				if (dc & 1) dc += dc > 0 ? -1 : 1;
			}
			ref_list_mv[i] = mv_t{ clamp_mv_row(dr, border), clamp_mv_col(dc, border) };
		}
		nearest_mv[rl] = ref_list_mv[0]; near_mv[rl] = ref_list_mv[1]; best_mv[rl] = ref_list_mv[0];
	}
	void append_sub8x8_mvs(int block, int rl) {
		find_mv_refs(ref_frame[rl], block);
		mv_t sub[2]; int nsub = 0;
		if (block == 0) { sub[0] = ref_list_mv[0]; sub[1] = ref_list_mv[1]; nsub = 2; }
		else if (block <= 2) { sub[0] = block_mvs[rl][0]; nsub = 1; }
		else {
			sub[0] = block_mvs[rl][2]; nsub = 1;
			int order[2] = { 1, 0 };
			for (int oi = 0; oi < 2; ++oi) { if (nsub >= 2) break; int idx = order[oi]; if (block_mvs[rl][idx] != sub[0]) sub[nsub++] = block_mvs[rl][idx]; }
		}
		for (int n = 0; n < 2; ++n) { if (nsub >= 2) break; if (ref_list_mv[n] != sub[0]) sub[nsub++] = ref_list_mv[n]; }
		if (nsub < 2) sub[nsub++] = ZERO_MV;
		nearest_mv[rl] = sub[0]; near_mv[rl] = sub[1];
	}

	// inter blocks index coef_probs[...][is_inter]
	int tokens(bool_decoder& bd, int plane, int start_x, int start_y, int txsz, int block_idx) override {
		int seg_eob = 16 << (txsz << 1);
		int tt = tx_type(plane, txsz, block_idx);
		const T::I1& scan = get_scan(tt, txsz);
		int n = 4 << txsz;
		int pl_type = plane > 0 ? 1 : 0;
		const T::I3& cbook = p.coef[txsz][pl_type][is_inter ? 1 : 0];
		std::vector<int> token_cache(seg_eob, 0);
		int check_eob = 1, c = 0;
		int subx = plane > 0 ? sub_x : 0, suby = plane > 0 ? sub_y : 0;
		int maxX = (2 * mi_cols) >> subx, maxY = (2 * mi_rows) >> suby;
		int x4 = start_x >> 2, y4 = start_y >> 2;
		while (c < seg_eob) {
			int pos = scan[c];
			int band = (txsz == TX_4X4) ? T::COEFBAND_4X4[c] : T::COEFBAND_8X8PLUS[c];
			int ctx;
			if (c == 0) {
				int numpts = 1 << txsz, above = 0, left = 0;
				for (int i = 0; i < numpts; ++i) { if (x4 + i < maxX) above |= above_nz[plane][x4 + i]; if (y4 + i < maxY) left |= left_nz[plane][y4 + i]; }
				ctx = above + left;
			} else { int nb0, nb1; neighbors(pos, n, tt, nb0, nb1); ctx = (1 + token_cache[nb0] + token_cache[nb1]) >> 1; }
			const T::I1& probs = cbook[band][ctx];
			if (check_eob) { if (!bd.read_bool(probs[0])) break; }
			int token = read_tree(bd, T::TOKEN_TREE, [&](int nd) { return pareto(nd, probs[std::min(2, 1 + nd)]); });
			token_cache[pos] = T::ENERGY_CLASS[token];
			if (token == ZERO_TOKEN) check_eob = 0;
			else { read_coef(bd, token); bd.read_literal(1); check_eob = 1; }
			++c;
		}
		return c > 0 ? 1 : 0;
	}
};

// Decode one frame's tiles for entropy; return (padding, filled decoder unused).
static inline int inter_decode_frame_padding(const uint8_t* data, size_t datalen,
		size_t lo, size_t hi, frame_decoder& dec, const header_t& h) {
	size_t tile_start = lo + h.uncompressed_bytes + h.header_size_in_bytes;
	int tile_cols = 1 << h.tile_cols_log2, tile_rows = 1 << h.tile_rows_log2;
	int total_pad = 0;
	size_t pos = tile_start;
	for (int tr = 0; tr < tile_rows; ++tr)
		for (int tc = 0; tc < tile_cols; ++tc) {
			bool last = (tr == tile_rows - 1) && (tc == tile_cols - 1);
			size_t size;
			if (last) size = hi - pos;
			else { size = ((size_t)data[pos] << 24) | ((size_t)data[pos + 1] << 16) | ((size_t)data[pos + 2] << 8) | data[pos + 3]; pos += 4; }
			int r0 = tile_offset(tr, dec.mi_rows, h.tile_rows_log2);
			int r1 = tile_offset(tr + 1, dec.mi_rows, h.tile_rows_log2);
			int c0 = tile_offset(tc, dec.mi_cols, h.tile_cols_log2);
			int c1 = tile_offset(tc + 1, dec.mi_cols, h.tile_cols_log2);
			bool_decoder bd(data, datalen, (int)pos, (int)size);
			for (int pl = 0; pl < 3; ++pl) std::fill(dec.above_nz[pl].begin(), dec.above_nz[pl].end(), 0);
			std::fill(dec.above_part.begin(), dec.above_part.end(), 0);
			dec.decode_tile(bd, r0, r1, c0, c1);
			if (bd.bits_left < 0) return -1;
			total_pad += bd.exit_padding_nonzero();
			pos += size;
		}
	return total_pad;
}

// Decode a whole clip's entropy; return total non-zero tile padding (0 clean).
// Mirrors vp9_inter.py:decode_clip_padding.
static inline long decode_clip_padding(const uint8_t* blob, size_t bloblen,
		const std::vector<webm::block_t>& frames, int* out_frames = nullptr) {
	std::vector<frame_probs> contexts(4, frame_probs::make_default());
	prev_frame_t prev_a, prev_b;
	prev_frame_t* prev = nullptr;
	header_parser parser;
	long total = 0;
	int nfr = 0;
	for (const auto& fr : frames) {
		const uint8_t* block = blob + fr.offset;
		size_t blen = fr.size;
		auto parts = webm::vp9_superframe_split(block, blen);
		for (auto& pr : parts) {
			size_t lo = pr.first, hi = pr.second;
			if (hi <= lo || lo >= blen || (block[lo] >> 6) != 2) continue;
			header_t h = parser.parse(block + lo, hi - lo);
			if (!h.ok || h.show_existing >= 0) continue;
			bool frame_is_intra = h.keyframe || h.intra_only;
			if (frame_is_intra || h.error_resilient)
				contexts.assign(4, frame_probs::make_default());
			int idx = h.frame_context_idx;
			frame_probs probs = contexts[idx];
			int pad = inter_parse_compressed(probs, h, block, blen, (int)(lo + h.uncompressed_bytes));
			if (pad != 0) { if (out_frames) *out_frames = nfr; return -100000L + pad; }
			bool use_prev = (prev && prev->valid && prev->w == h.width && prev->h == h.height &&
			                 prev->show == 1 && !h.error_resilient && !frame_is_intra);
			frame_decoder dec(h, probs, frame_is_intra, use_prev ? prev : nullptr);
			dec.use_prev_mvs = use_prev;
			int fpad = inter_decode_frame_padding(block, blen, lo, hi, dec, h);
			if (fpad < 0) { if (out_frames) *out_frames = nfr; return -1; }
			total += fpad;
			if (h.refresh_frame_context) contexts[idx] = probs;
			// stash this frame's grids as prev for the next frame
			prev_frame_t& nxt = (prev == &prev_a) ? prev_b : prev_a;
			nxt.mv = std::move(dec.mv_grid);
			nxt.ref = std::move(dec.ref_grid);
			nxt.w = h.width; nxt.h = h.height; nxt.show = h.show_frame; nxt.valid = true;
			prev = &nxt;
			++nfr;
		}
	}
	if (out_frames) *out_frames = nfr;
	return total;
}

} // namespace vp9
} // namespace bwgame

#endif
