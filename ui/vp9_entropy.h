// vp9_entropy.h -- VP9 key-frame entropy decode (Faze 3.2c of open1161HD).
//
// Ported from the grp-toolkit oracle tools/vp9_decode.py (VP9 spec v0.6 §6-§9).
// Walks a key frame's tile partition tree, decodes intra modes and the
// coefficient tokens for every transform block, and maintains all entropy
// contexts -- everything that CONSUMES bits. It stops before intra prediction /
// inverse transforms (which produce pixels but read no bits), so the oracle is
// the same as stage 2: each tile must finish with zero exit padding (spec
// §9.2.3). Unlike the header stage this needs the real probability VALUES, so it
// exercises the default tables (ui/vp9_tables.h) and the header updates.
//
// Key frame only; inter blocks are a later stage. Pure presentation.
#ifndef BWGAME_UI_VP9_ENTROPY_H
#define BWGAME_UI_VP9_ENTROPY_H

#include "vp9.h"
#include "vp9_tables.h"
#include <algorithm>

namespace bwgame {
namespace vp9 {

namespace T = tables;

enum { DC_PRED = 0, TM_PRED = 9,
       DCT_DCT = 0, ADST_DCT = 1, DCT_ADST = 2, ADST_ADST = 3,
       PARTITION_NONE = 0, PARTITION_HORZ = 1, PARTITION_VERT = 2, PARTITION_SPLIT = 3,
       BLOCK_8X8 = 3, BLOCK_64X64 = 12, BLOCK_INVALID = 14,
       TX_4X4 = 0, TX_8X8 = 1, TX_16X16 = 2, TX_32X32 = 3, ZERO_TOKEN = 0 };

// tree decode (spec §9.3.3): n = tree[n + read_bool(P(n>>1))] until n<=0.
template <class ProbFn>
static inline int read_tree(bool_decoder& bd, const T::I1& tree, ProbFn prob) {
	int n = 0;
	for (;;) {
		n = tree[n + bd.read_bool(prob(n >> 1))];
		if (n <= 0) return -n;
	}
}

static inline int inv_recenter_nonneg(int v, int m) {
	if (v > 2 * m) return v;
	if (v & 1) return m - ((v + 1) >> 1);
	return m + (v >> 1);
}
static inline int inv_remap_prob(int delta, int prob) {
	int m = prob - 1;
	int v = T::INV_MAP_TABLE[delta];
	if ((m << 1) <= 255) return 1 + inv_recenter_nonneg(v, m);
	return 255 - inv_recenter_nonneg(v, 254 - m);
}
static inline int decode_term_subexp_v(bool_decoder& bd) {
	if (bd.read_literal(1) == 0) return bd.read_literal(4);
	if (bd.read_literal(1) == 0) return bd.read_literal(4) + 16;
	if (bd.read_literal(1) == 0) return bd.read_literal(5) + 32;
	int v = bd.read_literal(7);
	if (v < 65) return v + 64;
	return (v << 1) - 1 + bd.read_literal(1);
}

// Frame coefficient / tx / skip probabilities: defaults + compressed-header
// diff updates. (Key-frame mode/partition probs are the fixed kf_* tables.)
struct probs_t {
	T::I6 coef;
	T::I3 tx_probs;
	T::I1 skip;
	int tx_mode = 0;
	int pad = 0;

	int du(bool_decoder& bd, int cur) {
		if (bd.read_bool(252)) return inv_remap_prob(decode_term_subexp_v(bd), cur);
		return cur;
	}

	void parse(const header_t& h, const uint8_t* data, size_t datalen, int start) {
		coef = T::DEFAULT_COEF_PROBS;
		tx_probs = T::DEFAULT_TX_PROBS;
		skip = T::DEFAULT_SKIP_PROB;
		bool_decoder bd(data, datalen, start, h.header_size_in_bytes);
		int tm;
		if (h.lossless) tm = 0;
		else { tm = bd.read_literal(2); if (tm == 3) tm += bd.read_literal(1); }
		tx_mode = tm;
		if (tm == TX_MODE_SELECT) {
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 1; ++j)
				tx_probs[1][i][j] = du(bd, tx_probs[1][i][j]);
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j)
				tx_probs[2][i][j] = du(bd, tx_probs[2][i][j]);
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 3; ++j)
				tx_probs[3][i][j] = du(bd, tx_probs[3][i][j]);
		}
		int max_tx = TX_MODE_TO_BIGGEST[tm];
		for (int txsz = 0; txsz <= max_tx; ++txsz) {
			if (bd.read_literal(1)) {
				for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j)
					for (int k = 0; k < 6; ++k) {
						int maxl = (k == 0) ? 3 : 6;
						for (int l = 0; l < maxl; ++l) for (int m = 0; m < 3; ++m)
							coef[txsz][i][j][k][l][m] = du(bd, coef[txsz][i][j][k][l][m]);
					}
			}
		}
		for (int i = 0; i < SKIP_CONTEXTS; ++i) skip[i] = du(bd, skip[i]);
		pad = (bd.bits_left >= 0) ? bd.exit_padding_nonzero() : -1;
	}
};

static inline int pareto(int node, int prob) {
	if (node < 2) return prob;
	int x = (prob - 1) / 2;
	if (prob & 1) return T::PARETO_TABLE[x][node - 2];
	return (T::PARETO_TABLE[x][node - 2] + T::PARETO_TABLE[x + 1][node - 2]) >> 1;
}

struct keyframe_decoder {
	const header_t& h;
	probs_t p;   // by value: inter frames refill coef/tx/skip from frame_probs
	int mi_cols, mi_rows, sub_x, sub_y;
	std::vector<int> above_part, left_part;                 // partition ctx
	std::vector<std::vector<int>> above_nz, left_nz;        // [3][...]
	std::vector<std::vector<std::array<int, 4>>> sub_modes; // [rows][cols][4]
	std::vector<std::vector<int>> skips, tx_sizes, mi_sizes;

	// per-block scratch
	int MiRow = 0, MiCol = 0, MiSize = 0;
	int MiRowStart = 0, MiRowEnd = 0, MiColStart = 0, MiColEnd = 0;
	bool AvailU = false, AvailL = false;
	int cur_skip = 0, cur_tx_size = 0, y_mode = 0, uv_mode = 0;
	std::array<int, 4> sub_mode{ { DC_PRED, DC_PRED, DC_PRED, DC_PRED } };

	virtual ~keyframe_decoder() {}
	keyframe_decoder(const header_t& hdr, const probs_t& pr) : h(hdr), p(pr) {
		mi_cols = (h.width + 7) >> 3;
		mi_rows = (h.height + 7) >> 3;
		sub_x = h.ss_x; sub_y = h.ss_y;
		above_part.assign(mi_cols + 8, 0);
		left_part.assign(8, 0);
		above_nz.assign(3, std::vector<int>(2 * mi_cols + 16, 0));
		left_nz.assign(3, std::vector<int>(2 * mi_rows + 16, 0));
		sub_modes.assign(mi_rows, std::vector<std::array<int, 4>>(mi_cols,
			std::array<int, 4>{ { DC_PRED, DC_PRED, DC_PRED, DC_PRED } }));
		skips.assign(mi_rows, std::vector<int>(mi_cols, 0));
		tx_sizes.assign(mi_rows, std::vector<int>(mi_cols, 0));
		mi_sizes.assign(mi_rows, std::vector<int>(mi_cols, BLOCK_64X64));
	}

	void decode_tile(bool_decoder& bd, int r0, int r1, int c0, int c1) {
		MiRowStart = r0; MiRowEnd = r1; MiColStart = c0; MiColEnd = c1;
		for (int r = r0; r < r1; r += 8) {
			for (int pl = 0; pl < 3; ++pl)
				std::fill(left_nz[pl].begin(), left_nz[pl].end(), 0);
			std::fill(left_part.begin(), left_part.end(), 0);
			for (int c = c0; c < c1; c += 8) partition(bd, r, c, BLOCK_64X64);
		}
	}

	virtual void partition(bool_decoder& bd, int r, int c, int bsize) {
		if (r >= mi_rows || c >= mi_cols) return;
		int num8x8 = T::NUM_8X8_W[bsize];
		int half = num8x8 >> 1;
		bool has_rows = (r + half) < mi_rows;
		bool has_cols = (c + half) < mi_cols;
		int part = read_partition(bd, r, c, bsize, num8x8, has_rows, has_cols);
		int subsize = T::SUBSIZE_LOOKUP[part][bsize];
		if (subsize < BLOCK_8X8 || part == PARTITION_NONE) {
			block(bd, r, c, subsize);
		} else if (part == PARTITION_HORZ) {
			block(bd, r, c, subsize);
			if (has_rows) block(bd, r + half, c, subsize);
		} else if (part == PARTITION_VERT) {
			block(bd, r, c, subsize);
			if (has_cols) block(bd, r, c + half, subsize);
		} else {
			partition(bd, r, c, subsize);
			partition(bd, r, c + half, subsize);
			partition(bd, r + half, c, subsize);
			partition(bd, r + half, c + half, subsize);
		}
		if (bsize == BLOCK_8X8 || part != PARTITION_SPLIT) {
			int aw = 15 >> T::B_WIDTH_LOG2[subsize];
			int ah = 15 >> T::B_HEIGHT_LOG2[subsize];
			for (int i = 0; i < num8x8; ++i) {
				above_part[c + i] = aw;
				left_part[(r + i) & 7] = ah;
			}
		}
	}

	virtual int read_partition(bool_decoder& bd, int r, int c, int bsize, int num8x8,
	                   bool has_rows, bool has_cols) {
		int bsl = T::MI_WIDTH_LOG2[bsize];
		int boffset = T::MI_WIDTH_LOG2[BLOCK_64X64] - bsl;
		int above = 0, left = 0;
		for (int i = 0; i < num8x8; ++i) {
			above |= above_part[c + i];
			left |= left_part[(r + i) & 7];
		}
		above = (above & (1 << boffset)) ? 1 : 0;
		left = (left & (1 << boffset)) ? 1 : 0;
		int ctx = bsl * 4 + left * 2 + above;
		const T::I1& probs = T::KF_PARTITION_PROBS[ctx];
		auto P = [&](int node) {
			int node2 = (has_rows && has_cols) ? node : (has_cols ? 1 : 2);
			return probs[node2];
		};
		if (has_rows && has_cols) return read_tree(bd, T::PARTITION_TREE, P);
		if (has_cols) return read_tree(bd, T::COLS_PARTITION_TREE, P);
		if (has_rows) return read_tree(bd, T::ROWS_PARTITION_TREE, P);
		return PARTITION_SPLIT;
	}

	virtual void block(bool_decoder& bd, int r, int c, int subsize) {
		MiRow = r; MiCol = c; MiSize = subsize;
		AvailU = r > 0;
		AvailL = c > MiColStart;
		intra_mode_info(bd);
		residual(bd);
		int num_h = T::NUM_8X8_H[subsize], num_w = T::NUM_8X8_W[subsize];
		for (int y = 0; y < num_h; ++y)
			for (int x = 0; x < num_w; ++x)
				if (r + y < mi_rows && c + x < mi_cols) {
					skips[r + y][c + x] = cur_skip;
					tx_sizes[r + y][c + x] = cur_tx_size;
					mi_sizes[r + y][c + x] = subsize;
					for (int b = 0; b < 4; ++b) sub_modes[r + y][c + x][b] = sub_mode[b];
				}
	}

	virtual void intra_mode_info(bool_decoder& bd) {
		cur_skip = read_skip(bd);
		cur_tx_size = read_tx_size(bd, true);
		sub_mode = { { DC_PRED, DC_PRED, DC_PRED, DC_PRED } };
		int mode = DC_PRED;
		if (MiSize >= BLOCK_8X8) {
			mode = read_kf_y_mode(bd, 0, 0);
			y_mode = mode;
			sub_mode = { { mode, mode, mode, mode } };
		} else {
			int n4w = T::NUM_4X4_W[MiSize], n4h = T::NUM_4X4_H[MiSize];
			int idy = 0;
			while (idy < 2) {
				int idx = 0;
				while (idx < 2) {
					mode = read_kf_y_mode(bd, idx, idy);
					for (int y2 = 0; y2 < n4h; ++y2)
						for (int x2 = 0; x2 < n4w; ++x2)
							sub_mode[(idy + y2) * 2 + idx + x2] = mode;
					idx += n4w;
				}
				idy += n4h;
			}
			y_mode = mode;
		}
		const T::I1& uv_probs = T::KF_UV_MODE_PROBS[y_mode];
		uv_mode = read_tree(bd, T::INTRA_MODE_TREE, [&](int n) { return uv_probs[n]; });
	}

	int read_kf_y_mode(bool_decoder& bd, int idx, int idy) {
		int r = MiRow, c = MiCol, above, left;
		if (MiSize >= BLOCK_8X8) {
			above = AvailU ? sub_modes[r - 1][c][2] : DC_PRED;
			left = AvailL ? sub_modes[r][c - 1][1] : DC_PRED;
		} else {
			if (idy) above = sub_mode[idx];
			else above = AvailU ? sub_modes[r - 1][c][2 + idx] : DC_PRED;
			if (idx) left = sub_mode[idy * 2];
			else left = AvailL ? sub_modes[r][c - 1][1 + idy * 2] : DC_PRED;
		}
		const T::I1& probs = T::KF_Y_MODE_PROBS[above][left];
		return read_tree(bd, T::INTRA_MODE_TREE, [&](int n) { return probs[n]; });
	}

	int read_skip(bool_decoder& bd) {
		int ctx = 0, r = MiRow, c = MiCol;
		if (AvailU) ctx += skips[r - 1][c];
		if (AvailL) ctx += skips[r][c - 1];
		return bd.read_bool(p.skip[ctx]);
	}

	int read_tx_size(bool_decoder& bd, bool allow_select) {
		int max_tx = T::MAX_TXSIZE[MiSize];
		if (allow_select && p.tx_mode == TX_MODE_SELECT && MiSize >= BLOCK_8X8) {
			int ctx = tx_size_ctx();
			const T::I1& probs = p.tx_probs[max_tx][ctx];
			if (max_tx == TX_32X32) return read_tree(bd, T::TX_SIZE_32_TREE, [&](int n) { return probs[n]; });
			if (max_tx == TX_16X16) return read_tree(bd, T::TX_SIZE_16_TREE, [&](int n) { return probs[n]; });
			return read_tree(bd, T::TX_SIZE_8_TREE, [&](int n) { return probs[n]; });
		}
		return std::min(max_tx, TX_MODE_TO_BIGGEST[p.tx_mode]);
	}

	int tx_size_ctx() {
		int r = MiRow, c = MiCol;
		int max_tx = T::MAX_TXSIZE[MiSize];
		int above = max_tx, left = max_tx;
		if (AvailU && !skips[r - 1][c]) above = tx_sizes[r - 1][c];
		if (AvailL && !skips[r][c - 1]) left = tx_sizes[r][c - 1];
		if (!AvailL) left = above;
		if (!AvailU) above = left;
		return (above + left) > max_tx ? 1 : 0;
	}

	virtual void residual(bool_decoder& bd) {
		int bsize = (MiSize < BLOCK_8X8) ? BLOCK_8X8 : MiSize;
		for (int plane = 0; plane < 3; ++plane) {
			int txsz = (plane > 0) ? uv_tx_size() : cur_tx_size;
			int step = 1 << txsz;
			int plane_sz = plane_block_size(bsize, plane);
			int n4w = T::NUM_4X4_W[plane_sz], n4h = T::NUM_4X4_H[plane_sz];
			int subx = (plane > 0) ? sub_x : 0, suby = (plane > 0) ? sub_y : 0;
			int base_x = (MiCol * 8) >> subx, base_y = (MiRow * 8) >> suby;
			int maxx = (mi_cols * 8) >> subx, maxy = (mi_rows * 8) >> suby;
			int block_idx = 0;
			for (int y = 0; y < n4h; y += step) {
				for (int x = 0; x < n4w; x += step) {
					int start_x = base_x + 4 * x, start_y = base_y + 4 * y;
					int nonzero = 0;
					if (start_x < maxx && start_y < maxy && !cur_skip)
						nonzero = tokens(bd, plane, start_x, start_y, txsz, block_idx);
					for (int i = 0; i < step; ++i) {
						above_nz[plane][(start_x >> 2) + i] = nonzero;
						left_nz[plane][(start_y >> 2) + i] = nonzero;
					}
					++block_idx;
				}
			}
		}
	}

	int uv_tx_size() {
		if (MiSize < BLOCK_8X8) return TX_4X4;
		return std::min(cur_tx_size, T::MAX_TXSIZE[plane_block_size(MiSize, 1)]);
	}
	int plane_block_size(int subsize, int plane) {
		int sx = (plane > 0) ? sub_x : 0, sy = (plane > 0) ? sub_y : 0;
		return T::SS_SIZE_LOOKUP[subsize][sx][sy];
	}
	int tx_type(int plane, int txsz, int block_idx) {
		if (plane > 0 || txsz == TX_32X32) return DCT_DCT;
		if (txsz == TX_4X4) {
			if (h.lossless) return DCT_DCT;
			int m = (MiSize < BLOCK_8X8) ? sub_mode[block_idx] : y_mode;
			return T::MODE2TXFM[m];
		}
		return T::MODE2TXFM[y_mode];
	}
	const T::I1& get_scan(int tx_type_, int txsz) {
		if (txsz == TX_4X4)
			return tx_type_ == ADST_DCT ? T::ROW_SCAN_4X4 : tx_type_ == DCT_ADST ? T::COL_SCAN_4X4 : T::DEFAULT_SCAN_4X4;
		if (txsz == TX_8X8)
			return tx_type_ == ADST_DCT ? T::ROW_SCAN_8X8 : tx_type_ == DCT_ADST ? T::COL_SCAN_8X8 : T::DEFAULT_SCAN_8X8;
		if (txsz == TX_16X16)
			return tx_type_ == ADST_DCT ? T::ROW_SCAN_16X16 : tx_type_ == DCT_ADST ? T::COL_SCAN_16X16 : T::DEFAULT_SCAN_16X16;
		return T::DEFAULT_SCAN_32X32;
	}

	virtual int tokens(bool_decoder& bd, int plane, int start_x, int start_y, int txsz, int block_idx) {
		int seg_eob = 16 << (txsz << 1);
		int tt = tx_type(plane, txsz, block_idx);
		const T::I1& scan = get_scan(tt, txsz);
		int n = 4 << txsz;
		int pl_type = (plane > 0) ? 1 : 0;
		const T::I3& cbook = p.coef[txsz][pl_type][0]; // is_inter = 0
		std::vector<int> token_cache(seg_eob, 0);
		int check_eob = 1, c = 0;
		int subx = (plane > 0) ? sub_x : 0, suby = (plane > 0) ? sub_y : 0;
		int maxX = (2 * mi_cols) >> subx, maxY = (2 * mi_rows) >> suby;
		int x4 = start_x >> 2, y4 = start_y >> 2;
		while (c < seg_eob) {
			int pos = scan[c];
			int band = (txsz == TX_4X4) ? T::COEFBAND_4X4[c] : T::COEFBAND_8X8PLUS[c];
			int ctx;
			if (c == 0) {
				int numpts = 1 << txsz, above = 0, left = 0;
				for (int i = 0; i < numpts; ++i) {
					if (x4 + i < maxX) above |= above_nz[plane][x4 + i];
					if (y4 + i < maxY) left |= left_nz[plane][y4 + i];
				}
				ctx = above + left;
			} else {
				int nb0, nb1;
				neighbors(pos, n, tt, nb0, nb1);
				ctx = (1 + token_cache[nb0] + token_cache[nb1]) >> 1;
			}
			const T::I1& probs = cbook[band][ctx];
			if (check_eob) {
				if (!bd.read_bool(probs[0])) break;
			}
			int token = read_tree(bd, T::TOKEN_TREE,
				[&](int nd) { return pareto(nd, probs[std::min(2, 1 + nd)]); });
			token_cache[pos] = T::ENERGY_CLASS[token];
			if (token == ZERO_TOKEN) {
				check_eob = 0;
			} else {
				read_coef(bd, token);
				bd.read_literal(1); // sign
				check_eob = 1;
			}
			++c;
		}
		return c > 0 ? 1 : 0;
	}

	static void neighbors(int pos, int n, int tx_type_, int& nb0, int& nb1) {
		int i = pos / n, j = pos % n;
		if (i > 0 && j > 0) {
			int a = (i - 1) * n + j, a2 = i * n + j - 1;
			if (tx_type_ == DCT_ADST) { nb0 = nb1 = a; return; }
			if (tx_type_ == ADST_DCT) { nb0 = nb1 = a2; return; }
			nb0 = a; nb1 = a2; return;
		}
		if (i > 0) { nb0 = nb1 = (i - 1) * n + j; return; }
		nb0 = nb1 = i * n + j - 1;
	}

	int read_coef(bool_decoder& bd, int token) {
		const T::I1& eb = T::EXTRA_BITS[token];
		int cat = eb[0], num_extra = eb[1], coef = eb[2];
		int bd_depth = h.bit_depth;
		if (token == 10)
			for (int e = 0; e < bd_depth - 8; ++e)
				coef += bd.read_bool(255) << (5 + bd_depth - e);
		for (int e = 0; e < num_extra; ++e)
			coef += bd.read_bool(T::CAT_PROBS[cat][e]) << (num_extra - 1 - e);
		return coef;
	}
};

static inline int tile_offset(int tile_num, int mis, int log2) {
	int sbs = (mis + 7) >> 3;
	int v = ((tile_num * sbs) >> log2) << 3;
	return v < mis ? v : mis;
}

// Decode the entropy of a VP9 key frame across all tiles; return total non-zero
// exit padding (0 == clean, fully in-sync). Negative = desync. Mirrors
// vp9_decode.py:decode_keyframe_entropy.
static inline int decode_keyframe_entropy(const uint8_t* data, size_t datalen,
                                          size_t lo, size_t hi) {
	header_parser hp;
	header_t h = hp.parse(data + lo, hi - lo);
	if (!h.ok || h.show_existing >= 0 || !h.keyframe) return -2000;

	probs_t probs;
	probs.parse(h, data, datalen, (int)(lo + h.uncompressed_bytes));
	if (probs.pad != 0) return -1000 + probs.pad;

	keyframe_decoder dec(h, probs);
	size_t tile_start = lo + h.uncompressed_bytes + h.header_size_in_bytes;
	int tile_cols = 1 << h.tile_cols_log2, tile_rows = 1 << h.tile_rows_log2;
	int total_pad = 0;
	size_t pos = tile_start;
	for (int tr = 0; tr < tile_rows; ++tr) {
		for (int tc = 0; tc < tile_cols; ++tc) {
			bool last = (tr == tile_rows - 1) && (tc == tile_cols - 1);
			size_t size;
			if (last) size = hi - pos;
			else {
				size = ((size_t)data[pos] << 24) | ((size_t)data[pos + 1] << 16) |
				       ((size_t)data[pos + 2] << 8) | data[pos + 3];
				pos += 4;
			}
			int r0 = tile_offset(tr, dec.mi_rows, h.tile_rows_log2);
			int r1 = tile_offset(tr + 1, dec.mi_rows, h.tile_rows_log2);
			int c0 = tile_offset(tc, dec.mi_cols, h.tile_cols_log2);
			int c1 = tile_offset(tc + 1, dec.mi_cols, h.tile_cols_log2);
			bool_decoder bd(data, datalen, (int)pos, (int)size);
			for (int pl = 0; pl < 3; ++pl)
				std::fill(dec.above_nz[pl].begin(), dec.above_nz[pl].end(), 0);
			std::fill(dec.above_part.begin(), dec.above_part.end(), 0);
			dec.decode_tile(bd, r0, r1, c0, c1);
			if (bd.bits_left < 0) return -1;
			total_pad += bd.exit_padding_nonzero();
			pos += size;
		}
	}
	return total_pad;
}

} // namespace vp9
} // namespace bwgame

#endif
