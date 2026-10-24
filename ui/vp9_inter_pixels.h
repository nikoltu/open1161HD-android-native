// vp9_inter_pixels.h -- VP9 inter-frame PIXELS over a whole sequence (Faze 3.4b).
//
// Ported from the grp-toolkit oracle tools/vp9_inter_pixels.py (VP9 spec §8.5.2).
// Builds on the inter entropy decoder (ui/vp9_inter.h) and the key-frame pixel
// path (ui/vp9_pixels.h). Adds: 8-tap sub-pixel motion compensation (§8.5.2.4),
// MV selection / clamp / scale (§8.5.2.1-3), compound averaging, inter-block
// reconstruction (residual added to the MC prediction), an 8-slot reference
// store, and the general (non-key-frame) loop filter with ref/mode level deltas.
// decode_sequence returns the shown frames (display order), matching what a
// reference decoder (libvpx) outputs. Pure presentation; no determinism impact.
#ifndef BWGAME_UI_VP9_INTER_PIXELS_H
#define BWGAME_UI_VP9_INTER_PIXELS_H

#include "vp9_inter.h"
#include "vp9_pixels.h"
#include "yuv_rgb.h"
#include <atomic>

namespace bwgame {
namespace vp9 {

enum { REF_SCALE_SHIFT = 14, SUBPEL_BITS = 4, SUBPEL_SHIFTS = 16, SUBPEL_MASK = 15 };

// spec 8.5.2.4 subpel_filters[4][16][8]
static const int SUBPEL_FILTERS[4][16][8] = {
	{{0,0,0,128,0,0,0,0},{0,1,-5,126,8,-3,1,0},{-1,3,-10,122,18,-6,2,0},{-1,4,-13,118,27,-9,3,-1},
	 {-1,4,-16,112,37,-11,4,-1},{-1,5,-18,105,48,-14,4,-1},{-1,5,-19,97,58,-16,5,-1},{-1,6,-19,88,68,-18,5,-1},
	 {-1,6,-19,78,78,-19,6,-1},{-1,5,-18,68,88,-19,6,-1},{-1,5,-16,58,97,-19,5,-1},{-1,4,-14,48,105,-18,5,-1},
	 {-1,4,-11,37,112,-16,4,-1},{-1,3,-9,27,118,-13,4,-1},{0,2,-6,18,122,-10,3,-1},{0,1,-3,8,126,-5,1,0}},
	{{0,0,0,128,0,0,0,0},{-3,-1,32,64,38,1,-3,0},{-2,-2,29,63,41,2,-3,0},{-2,-2,26,63,43,4,-4,0},
	 {-2,-3,24,62,46,5,-4,0},{-2,-3,21,60,49,7,-4,0},{-1,-4,18,59,51,9,-4,0},{-1,-4,16,57,53,12,-4,-1},
	 {-1,-4,14,55,55,14,-4,-1},{-1,-4,12,53,57,16,-4,-1},{0,-4,9,51,59,18,-4,-1},{0,-4,7,49,60,21,-3,-2},
	 {0,-4,5,46,62,24,-3,-2},{0,-4,4,43,63,26,-2,-2},{0,-3,2,41,63,29,-2,-2},{0,-3,1,38,64,32,-1,-3}},
	{{0,0,0,128,0,0,0,0},{-1,3,-7,127,8,-3,1,0},{-2,5,-13,125,17,-6,3,-1},{-3,7,-17,121,27,-10,5,-2},
	 {-4,9,-20,115,37,-13,6,-2},{-4,10,-23,108,48,-16,8,-3},{-4,10,-24,100,59,-19,9,-3},{-4,11,-24,90,70,-21,10,-4},
	 {-4,11,-23,80,80,-23,11,-4},{-4,10,-21,70,90,-24,11,-4},{-3,9,-19,59,100,-24,10,-4},{-3,8,-16,48,108,-23,10,-4},
	 {-2,6,-13,37,115,-20,9,-4},{-2,5,-10,27,121,-17,7,-3},{-1,3,-6,17,125,-13,5,-2},{0,1,-3,8,127,-7,3,-1}},
	{{0,0,0,128,0,0,0,0},{0,0,0,120,8,0,0,0},{0,0,0,112,16,0,0,0},{0,0,0,104,24,0,0,0},
	 {0,0,0,96,32,0,0,0},{0,0,0,88,40,0,0,0},{0,0,0,80,48,0,0,0},{0,0,0,72,56,0,0,0},
	 {0,0,0,64,64,0,0,0},{0,0,0,56,72,0,0,0},{0,0,0,48,80,0,0,0},{0,0,0,40,88,0,0,0},
	 {0,0,0,32,96,0,0,0},{0,0,0,24,104,0,0,0},{0,0,0,16,112,0,0,0},{0,0,0,8,120,0,0,0}},
};

struct frame_store_entry {
	bool valid = false;
	px::plane_t planes[3];   // B5: flat strided uint16_t
	px::plane_t crop[3];
	int w = 0, h = 0, sub_x = 1, sub_y = 1;
};

struct frame_pixel_decoder : frame_decoder {
	int bit_depth, loss;
	int dc_quant[3], ac_quant[3];
	px::plane_t cur[3];   // B5: flat strided uint16_t
	std::vector<int> Tokens;
	std::vector<int> token_cache;   // B7: reused per-block coeff-context scratch (was allocated per block)
	std::vector<std::vector<px::i64>> Dequant;   // B1: reused per-block dequant matrix (grow-only; fully overwritten each use)
	int TxType = 0;
	frame_store_entry* store;   // 8 slots
	int ref_frame_idx3[3];
	int ref_w[8], ref_h[8];

	frame_pixel_decoder(const header_t& hdr, frame_probs& probs, bool fi,
	                    const prev_frame_t* pv, frame_store_entry* st)
		: frame_decoder(hdr, probs, fi, pv), store(st) {
		bit_depth = hdr.bit_depth; loss = hdr.lossless;
		int base = hdr.base_q_idx, bi = (bit_depth - 8) >> 1;
		auto dcq = [&](int b) { return T::DC_QLOOKUP[bi][std::max(0, std::min(255, b))]; };
		auto acq = [&](int b) { return T::AC_QLOOKUP[bi][std::max(0, std::min(255, b))]; };
		dc_quant[0] = dcq(base + hdr.delta_q_y_dc);
		dc_quant[1] = dc_quant[2] = dcq(base + hdr.delta_q_uv_dc);
		ac_quant[0] = acq(base);
		ac_quant[1] = ac_quant[2] = acq(base + hdr.delta_q_uv_ac);
		int aw = ((mi_cols + 7) & ~7) * 8, ah = ((mi_rows + 7) & ~7) * 8;
		for (int pl = 0; pl < 3; ++pl) {
			int sx = pl > 0 ? sub_x : 0, sy = pl > 0 ? sub_y : 0;
			cur[pl].alloc(ah >> sy, aw >> sx);
		}
		for (int i = 0; i < 3; ++i) ref_frame_idx3[i] = hdr.ref_frame_idx[i];
		for (int i = 0; i < 8; ++i) { ref_w[i] = store[i].valid ? store[i].w : hdr.width; ref_h[i] = store[i].valid ? store[i].h : hdr.height; }
	}

	int uv_tx_size_of(int mi_size, int tx_size) {
		if (mi_size < BLOCK_8X8) return TX_4X4;
		return std::min(tx_size, T::MAX_TXSIZE[T::SS_SIZE_LOOKUP[mi_size][sub_x][sub_y]]);
	}
	enum { BLOCK_16X16 = 6 };
	int filter_size(int txsz, int is32, int pass_, int x, int y, int subx, int suby) {
		int base = (txsz == TX_4X4 && is32) ? TX_8X8 : std::min((int)TX_16X16, txsz);
		if (pass_ == 0 && subx == 1 && base == TX_16X16 && (x >> 3) == mi_cols - 1) return TX_8X8;
		if (pass_ == 1 && suby == 1 && base == TX_16X16 && (y >> 3) == mi_rows - 1) return TX_8X8;
		return base;
	}

	// --- residual: intra predict OR inter predict, then tokens + reconstruct ---
	void residual(bool_decoder& bd) override {
		int bsize = (MiSize < BLOCK_8X8) ? BLOCK_8X8 : MiSize;
		for (int plane = 0; plane < 3; ++plane) {
			int txsz = (plane > 0) ? uv_tx_size_of(MiSize, cur_tx_size) : cur_tx_size;
			int step = 1 << txsz;
			int plane_sz = plane_block_size(bsize, plane);
			int n4w = T::NUM_4X4_W[plane_sz], n4h = T::NUM_4X4_H[plane_sz];
			int subx = plane > 0 ? sub_x : 0, suby = plane > 0 ? sub_y : 0;
			int base_x = (MiCol * 8) >> subx, base_y = (MiRow * 8) >> suby;
			int maxx = (mi_cols * 8) >> subx, maxy = (mi_rows * 8) >> suby;
			if (is_inter) {
				if (MiSize < BLOCK_8X8)
					for (int yy = 0; yy < n4h; ++yy) for (int xx = 0; xx < n4w; ++xx)
						predict_inter(plane, base_x + 4 * xx, base_y + 4 * yy, 4, 4, yy * n4w + xx);
				else predict_inter(plane, base_x, base_y, n4w * 4, n4h * 4, 0);
			}
			int block_idx = 0;
			for (int y = 0; y < n4h; y += step) {
				for (int x = 0; x < n4w; x += step) {
					int start_x = base_x + 4 * x, start_y = base_y + 4 * y;
					int nonzero = 0;
					if (start_x < maxx && start_y < maxy) {
						if (!is_inter)
							predict_intra(plane, start_x, start_y, AvailL || x > 0, AvailU || y > 0, (x + step) < n4w, txsz, block_idx);
						if (!cur_skip) {
							nonzero = tokens(bd, plane, start_x, start_y, txsz, block_idx);
							reconstruct(plane, start_x, start_y, txsz);
						}
					}
					for (int i = 0; i < step; ++i) { above_nz[plane][(start_x >> 2) + i] = nonzero; left_nz[plane][(start_y >> 2) + i] = nonzero; }
					++block_idx;
				}
			}
		}
	}

	int tokens(bool_decoder& bd, int plane, int start_x, int start_y, int txsz, int block_idx) override {
		int seg_eob = 16 << (txsz << 1);
		int tt = tx_type(plane, txsz, block_idx);
		TxType = tt;
		const T::I1& scan = get_scan(tt, txsz);
		Tokens.assign(seg_eob, 0);
		int n = 4 << txsz;
		int pl_type = plane > 0 ? 1 : 0;
		const T::I3& cbook = p.coef[txsz][pl_type][is_inter ? 1 : 0];
		token_cache.assign(seg_eob, 0);
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
			if (token == ZERO_TOKEN) { Tokens[pos] = 0; check_eob = 0; }
			else { int coef = read_coef(bd, token); int sign = bd.read_literal(1); Tokens[pos] = sign ? -coef : coef; check_eob = 1; }
			++c;
		}
		return c > 0 ? 1 : 0;
	}

	// --- reconstruct (duplicated from pixel_decoder: dequant + inv transform) ---
	void reconstruct(int plane, int x, int y, int txsz) {
		int dq_denom = (txsz == TX_32X32) ? 2 : 1;
		int n = 2 + txsz, n0 = 1 << n;
		int acq = ac_quant[plane], dcq = dc_quant[plane];
		// B1: reuse the member Dequant (grow-only; fully overwritten below, no clear).
		if ((int)Dequant.size() < n0) Dequant.resize(n0);
		for (int i = 0; i < n0; ++i) if ((int)Dequant[i].size() < n0) Dequant[i].resize(n0);
		for (int i = 0; i < n0; ++i) { int base = i * n0; for (int j = 0; j < n0; ++j) Dequant[i][j] = px::trunc_div((px::i64)Tokens[base + j] * acq, dq_denom); }
		Dequant[0][0] = px::trunc_div((px::i64)Tokens[0] * dcq, dq_denom);
		px::inverse_transform_2d(Dequant, n, TxType, loss);
		int maxv = (1 << bit_depth) - 1;
		for (int i = 0; i < n0; ++i) { uint16_t* frow = cur[plane][y + i]; for (int j = 0; j < n0; ++j) { px::i64 v = frow[x + j] + Dequant[i][j]; frow[x + j] = (uint16_t)(v < 0 ? 0 : (v > maxv ? maxv : (int)v)); } }
	}

	// --- intra prediction (duplicated verbatim from pixel_decoder) ---
	void predict_intra(int plane, int x, int y, bool have_left, bool have_above,
	                   bool not_on_right, int txsz, int block_idx) {
		px::plane_t& frame = cur[plane];
		int bd = bit_depth;
		int mode;
		if (plane > 0) mode = uv_mode;
		else if (MiSize >= BLOCK_8X8) mode = y_mode;
		else mode = sub_mode[block_idx];
		int log2size = txsz + 2, size = 1 << log2size;
		int subx = plane > 0 ? sub_x : 0, suby = plane > 0 ? sub_y : 0;
		int maxX = ((mi_cols * 8) >> subx) - 1, maxY = ((mi_rows * 8) >> suby) - 1;
		int half = 1 << (bd - 1);
		int A[65] = {0}, L[32] = {0}; // VP9 max tx size 32: A holds 2*size+1 (<=65), L holds size (<=32)
		if (!have_above) for (int i = 0; i < size; ++i) A[i + 1] = half - 1;
		else { const uint16_t* ar = frame[y - 1]; for (int i = 0; i < size; ++i) A[i + 1] = ar[std::min(maxX, x + i)]; }
		if (have_above && not_on_right && txsz == 0) { const uint16_t* ar = frame[y - 1]; for (int i = size; i < 2 * size; ++i) A[i + 1] = ar[std::min(maxX, x + i)]; }
		else for (int i = size; i < 2 * size; ++i) A[i + 1] = A[size];
		if (have_above && have_left) A[0] = frame[y - 1][std::min(maxX, x - 1)];
		else if (have_above) A[0] = half + 1;
		else A[0] = half - 1;
		if (have_left) for (int i = 0; i < size; ++i) L[i] = frame[std::min(maxY, y + i)][x - 1];
		else for (int i = 0; i < size; ++i) L[i] = half + 1;
		auto ar = [&](int k) { return A[k + 1]; };
		int pred[32][32] = {}; // VP9 max tx size 32x32
		if (mode == V_PRED) { for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) pred[i][j] = ar(j); }
		else if (mode == H_PRED) { for (int i = 0; i < size; ++i) { int Li = L[i]; for (int j = 0; j < size; ++j) pred[i][j] = Li; } }
		else if (mode == D207_PRED) {
			for (int j = 0; j < size; ++j) pred[size - 1][j] = L[size - 1];
			for (int i = 0; i < size - 1; ++i) pred[i][0] = px::round2(L[i] + L[i + 1], 1);
			for (int i = 0; i < size - 2; ++i) pred[i][1] = px::round2(L[i] + 2 * L[i + 1] + L[i + 2], 2);
			if (size >= 2) pred[size - 2][1] = px::round2(L[size - 2] + 3 * L[size - 1], 2);
			for (int i = size - 2; i >= 0; --i) for (int j = 2; j < size; ++j) pred[i][j] = pred[i + 1][j - 2];
		} else if (mode == D45_PRED) {
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) pred[i][j] = (i + j + 2 < size * 2) ? px::round2(ar(i + j) + 2 * ar(i + j + 1) + ar(i + j + 2), 2) : ar(2 * size - 1);
		} else if (mode == D63_PRED) {
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) { int k = i / 2 + j; pred[i][j] = (i & 1) ? px::round2(ar(k) + 2 * ar(k + 1) + ar(k + 2), 2) : px::round2(ar(k) + ar(k + 1), 1); }
		} else if (mode == D117_PRED) {
			for (int j = 0; j < size; ++j) pred[0][j] = px::round2(ar(j - 1) + ar(j), 1);
			pred[1][0] = px::round2(L[0] + 2 * ar(-1) + ar(0), 2);
			for (int j = 1; j < size; ++j) pred[1][j] = px::round2(ar(j - 2) + 2 * ar(j - 1) + ar(j), 2);
			if (size >= 3) pred[2][0] = px::round2(ar(-1) + 2 * L[0] + L[1], 2);
			for (int i = 3; i < size; ++i) pred[i][0] = px::round2(L[i - 3] + 2 * L[i - 2] + L[i - 1], 2);
			for (int i = 2; i < size; ++i) for (int j = 1; j < size; ++j) pred[i][j] = pred[i - 2][j - 1];
		} else if (mode == D135_PRED) {
			pred[0][0] = px::round2(L[0] + 2 * ar(-1) + ar(0), 2);
			for (int j = 1; j < size; ++j) pred[0][j] = px::round2(ar(j - 2) + 2 * ar(j - 1) + ar(j), 2);
			if (size >= 2) pred[1][0] = px::round2(ar(-1) + 2 * L[0] + L[1], 2);
			for (int i = 2; i < size; ++i) pred[i][0] = px::round2(L[i - 2] + 2 * L[i - 1] + L[i], 2);
			for (int i = 1; i < size; ++i) for (int j = 1; j < size; ++j) pred[i][j] = pred[i - 1][j - 1];
		} else if (mode == D153_PRED) {
			pred[0][0] = px::round2(L[0] + ar(-1), 1);
			for (int i = 1; i < size; ++i) pred[i][0] = px::round2(L[i - 1] + L[i], 1);
			if (size >= 2) { pred[0][1] = px::round2(L[0] + 2 * ar(-1) + ar(0), 2); pred[1][1] = px::round2(ar(-1) + 2 * L[0] + L[1], 2); }
			for (int i = 2; i < size; ++i) pred[i][1] = px::round2(L[i - 2] + 2 * L[i - 1] + L[i], 2);
			for (int j = 2; j < size; ++j) pred[0][j] = px::round2(ar(j - 3) + 2 * ar(j - 2) + ar(j - 1), 2);
			for (int i = 1; i < size; ++i) for (int j = 2; j < size; ++j) pred[i][j] = pred[i - 1][j - 2];
		} else if (mode == TM_PRED) {
			int am1 = ar(-1), maxv = (1 << bd) - 1;
			for (int i = 0; i < size; ++i) { int Li = L[i]; for (int j = 0; j < size; ++j) { int v = ar(j) + Li - am1; pred[i][j] = v < 0 ? 0 : (v > maxv ? maxv : v); } }
		} else {
			int avg;
			if (have_left && have_above) { int s = 0; for (int k = 0; k < size; ++k) s += L[k] + ar(k); avg = (s + size) >> (log2size + 1); }
			else if (have_left) { int s = 0; for (int k = 0; k < size; ++k) s += L[k]; avg = (s + (1 << (log2size - 1))) >> log2size; }
			else if (have_above) { int s = 0; for (int k = 0; k < size; ++k) s += ar(k); avg = (s + (1 << (log2size - 1))) >> log2size; }
			else avg = 1 << (bd - 1);
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) pred[i][j] = avg;
		}
		for (int i = 0; i < size; ++i) { uint16_t* frow = frame[y + i]; for (int j = 0; j < size; ++j) frow[x + j] = (uint16_t)pred[i][j]; }
	}

	// --- inter prediction (spec 8.5.2) ---
	static int round_q2(int v) { return (int)px::trunc_div(v < 0 ? v - 1 : v + 1, 2); }
	static int round_q4(int v) { return (int)px::trunc_div(v < 0 ? v - 2 : v + 2, 4); }

	std::vector<std::vector<int>> bip_pred[2]; // reused per-ref output for block_inter_predict
	void predict_inter(int plane, int x, int y, int w, int hgt, int block_idx) {
		int is_compound = ref_frame[1] > IF_NONE ? 1 : 0;
		std::vector<std::vector<int>>* preds = bip_pred;
		for (int rl = 0; rl < 1 + is_compound; ++rl) {
			mv_t m = mv_select(plane, rl, block_idx);
			mv_t cm = mv_clamp(plane, m);
			long sx, sy; int stepx, stepy;
			mv_scale(plane, rl, x, y, cm, sx, sy, stepx, stepy);
			block_inter_predict(plane, rl, sx, sy, stepx, stepy, w, hgt, preds[rl]);
		}
		px::plane_t& frame = cur[plane];
		if (!is_compound) {
			for (int i = 0; i < hgt; ++i) { uint16_t* row = frame[y + i]; for (int j = 0; j < w; ++j) row[x + j] = (uint16_t)preds[0][i][j]; }
		} else {
			for (int i = 0; i < hgt; ++i) { uint16_t* row = frame[y + i]; for (int j = 0; j < w; ++j) row[x + j] = (uint16_t)px::round2(preds[0][i][j] + preds[1][i][j], 1); }
		}
	}

	mv_t mv_select(int plane, int rl, int block_idx) {
		if (plane == 0 || MiSize >= BLOCK_8X8) return block_mvs[rl][block_idx];
		int sx = sub_x, sy = sub_y;
		mv_t* bm = block_mvs[rl];
		if (sx == 0 && sy == 0) return bm[block_idx];
		if (sx == 0 && sy == 1) return mv_t{ round_q2(bm[block_idx].r + bm[block_idx + 2].r), round_q2(bm[block_idx].c + bm[block_idx + 2].c) };
		if (sx == 1 && sy == 0) return mv_t{ round_q2(bm[block_idx].r + bm[block_idx + 1].r), round_q2(bm[block_idx].c + bm[block_idx + 1].c) };
		return mv_t{ round_q4(bm[0].r + bm[1].r + bm[2].r + bm[3].r), round_q4(bm[0].c + bm[1].c + bm[2].c + bm[3].c) };
	}

	mv_t mv_clamp(int plane, mv_t m) {
		int sx = plane == 0 ? 0 : sub_x, sy = plane == 0 ? 0 : sub_y;
		int bh = T::NUM_8X8_H[MiSize], bw = T::NUM_8X8_W[MiSize];
		long mb_top = (-(long)(MiRow * MI_SIZE * 16)) >> sy;
		long mb_bot = ((long)(mi_rows - bh - MiRow) * MI_SIZE * 16) >> sy;
		long mb_left = (-(long)(MiCol * MI_SIZE * 16)) >> sx;
		long mb_right = ((long)(mi_cols - bw - MiCol) * MI_SIZE * 16) >> sx;
		long spel_l = (long)(INTERP_EXTEND + ((bw * MI_SIZE) >> sx)) << SUBPEL_BITS;
		long spel_r = spel_l - SUBPEL_SHIFTS;
		long spel_t = (long)(INTERP_EXTEND + ((bh * MI_SIZE) >> sy)) << SUBPEL_BITS;
		long spel_b = spel_t - SUBPEL_SHIFTS;
		long c0 = px::clip3l(mb_top - spel_t, mb_bot + spel_b, (2L * m.r) >> sy);
		long c1 = px::clip3l(mb_left - spel_l, mb_right + spel_r, (2L * m.c) >> sx);
		return mv_t{ (int)c0, (int)c1 };
	}

	void mv_scale(int plane, int rl, int x, int y, mv_t cmv, long& out_x, long& out_y, int& step_x, int& step_y) {
		int ref_idx = ref_frame_idx3[ref_frame[rl] - IF_LAST];
		long rw = ref_w[ref_idx], rh = ref_h[ref_idx];
		long W = h.width, H = h.height;
		long x_scale = (rw << REF_SCALE_SHIFT) / W;
		long y_scale = (rh << REF_SCALE_SHIFT) / H;
		long base_x = ((long)x * x_scale) >> REF_SCALE_SHIFT;
		long base_y = ((long)y * y_scale) >> REF_SCALE_SHIFT;
		long luma_x = plane > 0 ? ((long)x << sub_x) : x;
		long luma_y = plane > 0 ? ((long)y << sub_y) : y;
		long frac_x = ((16 * luma_x * x_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;
		long frac_y = ((16 * luma_y * y_scale) >> REF_SCALE_SHIFT) & SUBPEL_MASK;
		long dx = (((long)cmv.c * x_scale) >> REF_SCALE_SHIFT) + frac_x;
		long dy = (((long)cmv.r * y_scale) >> REF_SCALE_SHIFT) + frac_y;
		step_x = (int)((16 * x_scale) >> REF_SCALE_SHIFT);
		step_y = (int)((16 * y_scale) >> REF_SCALE_SHIFT);
		out_x = (base_x << SUBPEL_BITS) + dx;
		out_y = (base_y << SUBPEL_BITS) + dy;
	}

	// reused scratch for block_inter_predict (grow-only; every used cell is overwritten each call)
	std::vector<std::vector<int>> bip_inter;
	void block_inter_predict(int plane, int rl, long x, long y, int step_x, int step_y, int w, int hgt,
	                         std::vector<std::vector<int>>& pred) {
		int ref_idx = ref_frame_idx3[ref_frame[rl] - IF_LAST];
		const px::plane_t& ref = store[ref_idx].planes[plane];
		int sx = plane == 0 ? 0 : sub_x, sy = plane == 0 ? 0 : sub_y;
		long last_x = ((ref_w[ref_idx] + sx) >> sx) - 1;
		long last_y = ((ref_h[ref_idx] + sy) >> sy) - 1;
		const int (*filt)[8] = SUBPEL_FILTERS[interp_filter];
		int inter_h = (int)((((hgt - 1) * (long)step_y + 15) >> 4) + 8);
		int maxv = (1 << bit_depth) - 1;
		if ((int)bip_inter.size() < inter_h) bip_inter.resize(inter_h);
		for (int r = 0; r < inter_h; ++r) if ((int)bip_inter[r].size() < w) bip_inter[r].resize(w);
		std::vector<std::vector<int>>& inter = bip_inter;
		// horizontal tap range is monotonic in c (step_x > 0): min at c=0, max at c=w-1.
		// If the whole row stays in-bounds we can drop the per-tap clip3l (hot path).
		long b_lo = (x >> 4) - 3;
		long b_hi = ((x + (long)step_x * (w - 1)) >> 4) - 3;
		bool x_interior = (b_lo >= 0 && b_hi + 7 <= last_x);
		for (int r = 0; r < inter_h; ++r) {
			long ry = px::clip3l(0, last_y, (y >> 4) + r - 3);
			const uint16_t* rrow = ref[(size_t)ry];
			std::vector<int>& irow = inter[r];
			if (x_interior) {
				for (int c = 0; c < w; ++c) {
					long pp = x + (long)step_x * c;
					const int* f = filt[pp & 15];
					const uint16_t* rp = &rrow[(pp >> 4) - 3];
					long s = (long)f[0] * rp[0] + (long)f[1] * rp[1] + (long)f[2] * rp[2] + (long)f[3] * rp[3]
						+ (long)f[4] * rp[4] + (long)f[5] * rp[5] + (long)f[6] * rp[6] + (long)f[7] * rp[7];
					int v = (int)px::round2(s, 7);
					irow[c] = v < 0 ? 0 : (v > maxv ? maxv : v);
				}
			} else {
				for (int c = 0; c < w; ++c) {
					long pp = x + (long)step_x * c;
					const int* f = filt[pp & 15];
					long b = (pp >> 4) - 3;
					long s = (long)f[0] * rrow[px::clip3l(0, last_x, b)] + (long)f[1] * rrow[px::clip3l(0, last_x, b + 1)]
						+ (long)f[2] * rrow[px::clip3l(0, last_x, b + 2)] + (long)f[3] * rrow[px::clip3l(0, last_x, b + 3)]
						+ (long)f[4] * rrow[px::clip3l(0, last_x, b + 4)] + (long)f[5] * rrow[px::clip3l(0, last_x, b + 5)]
						+ (long)f[6] * rrow[px::clip3l(0, last_x, b + 6)] + (long)f[7] * rrow[px::clip3l(0, last_x, b + 7)];
					int v = (int)px::round2(s, 7);
					irow[c] = v < 0 ? 0 : (v > maxv ? maxv : v);
				}
			}
		}
		if ((int)pred.size() < hgt) pred.resize(hgt);
		for (int r = 0; r < hgt; ++r) if ((int)pred[r].size() < w) pred[r].resize(w);
		for (int r = 0; r < hgt; ++r) {
			long pp = (y & 15) + (long)step_y * r;
			const int* f = filt[pp & 15];
			long b = pp >> 4;
			std::vector<int>& prow = pred[r];
			for (int c = 0; c < w; ++c) {
				long s = (long)f[0] * inter[b][c] + (long)f[1] * inter[b + 1][c] + (long)f[2] * inter[b + 2][c] + (long)f[3] * inter[b + 3][c]
					+ (long)f[4] * inter[b + 4][c] + (long)f[5] * inter[b + 5][c] + (long)f[6] * inter[b + 6][c] + (long)f[7] * inter[b + 7][c];
				int v = (int)px::round2(s, 7);
				prow[c] = v < 0 ? 0 : (v > maxv ? maxv : v);
			}
		}
	}

	// --- general loop filter (ref/mode deltas) ---
	int lut[4][2];
	void apply_loop_filter() {
		int level = h.loop_filter_level;
		if (level == 0) return;
		int n_shift = level >> 5;
		int deltas_on = h.loop_filter_delta_enabled;
		const int* ref_d = h.loop_filter_ref_deltas;
		const int* mode_d = h.loop_filter_mode_deltas;
		if (!deltas_on) { for (int r = 0; r < 4; ++r) for (int m = 0; m < 2; ++m) lut[r][m] = level; }
		else {
			lut[0][0] = lut[0][1] = px::clip3(0, 63, level + (ref_d[0] << n_shift));
			for (int r = 1; r < 4; ++r) for (int m = 0; m < 2; ++m) lut[r][m] = px::clip3(0, 63, level + (ref_d[r] << n_shift) + (mode_d[m] << n_shift));
		}
		for (int row = 0; row < mi_rows; row += 8)
			for (int col = 0; col < mi_cols; col += 8)
				for (int plane = 0; plane < 3; ++plane) { sb_loop_filter(plane, 0, row, col, bit_depth); sb_loop_filter(plane, 1, row, col, bit_depth); }
	}
	int lvl_at(int r, int c) {
		int ref = ref_grid[r][c][0];
		if (ref <= IF_INTRA) return lut[0][0];
		int mode = y_modes[r][c];
		int mt = (mode == MODE_NEARESTMV || mode == MODE_NEARMV || mode == MODE_NEWMV) ? 1 : 0;
		return lut[ref][mt];
	}
	void strength_of(int lvl, int sharp, int& limit, int& blimit, int& thresh) {
		int shift = sharp > 4 ? 2 : (sharp > 0 ? 1 : 0);
		limit = sharp > 0 ? px::clip3(1, 9 - sharp, lvl >> shift) : std::max(1, lvl >> shift);
		blimit = 2 * (lvl + 2) + limit;
		thresh = lvl >> 4;
	}
	void sb_loop_filter(int plane, int pass_, int row, int col, int bd) {
		px::plane_t& frame = cur[plane];
		int subx = plane == 0 ? 0 : sub_x, suby = plane == 0 ? 0 : sub_y;
		int dx, dy, sub, edge_len;
		if (pass_ == 0) { dx = 1; dy = 0; sub = subx; edge_len = 64 >> suby; }
		else { dx = 0; dy = 1; sub = suby; edge_len = 64 >> subx; }
		int sharp = h.loop_filter_sharpness;
		for (int edge = 0; edge < (16 >> sub); ++edge) {
			for (int i = 0; i < edge_len; ++i) {
				int x, y;
				if (pass_ == 0) { x = col * 8 + edge * (4 << subx); y = row * 8 + (i << suby); }
				else { x = col * 8 + (i << subx); y = row * 8 + edge * (4 << suby); }
				if (x >= 8 * mi_cols || y >= 8 * mi_rows) continue;
				if (pass_ == 0 && x == 0) continue;
				if (pass_ == 1 && y == 0) continue;
				int loop_col = ((x >> 3) >> subx) << subx, loop_row = ((y >> 3) >> suby) << suby;
				int mi_size = mi_sizes[loop_row][loop_col];
				int tx_size = tx_sizes[loop_row][loop_col];
				int txsz = plane > 0 ? uv_tx_size_of(mi_size, tx_size) : tx_size;
				int sb_size = sub == 0 ? mi_size : std::max((int)BLOCK_16X16, mi_size);
				int skip = skips[loop_row][loop_col];
				bool is_intra = ref_grid[loop_row][loop_col][0] <= IF_INTRA;
				int is_block_edge;
				if (pass_ == 0) is_block_edge = (x % (8 * T::NUM_8X8_W[sb_size]) == 0) ? 1 : 0;
				else is_block_edge = (y % (8 * T::NUM_8X8_H[sb_size]) == 0) ? 1 : 0;
				int is_tx_edge;
				if (pass_ == 1 && subx == 1 && (mi_cols & 1) && (edge & 1) && (x + 8) >= mi_cols * 8) is_tx_edge = 0;
				else if (edge % (1 << txsz) == 0) is_tx_edge = 1;
				else is_tx_edge = 0;
				int is32 = (edge % 8 == 0) ? 1 : 0;
				int apply;
				if (is_block_edge) apply = 1;
				else if (is_tx_edge && is_intra) apply = 1;
				else if (is_tx_edge && !skip) apply = 1;
				else apply = 0;
				if (!apply) continue;
				int lvl = lvl_at(loop_row, loop_col);
				if (lvl <= 0) continue;
				int limit, blimit, thresh; strength_of(lvl, sharp, limit, blimit, thresh);
				int fs = filter_size(txsz, is32, pass_, x, y, subx, suby);
				px::lf_sample(frame, x >> subx, y >> suby, limit, blimit, thresh, dx, dy, fs, bd);
			}
		}
	}
};

// crop planes to display size (chroma sub-sampled)
static inline void crop_planes(const px::plane_t cur[3], const header_t& h, px::plane_t out[3]) {
	int w = h.width, ht = h.height, sx = h.ss_x, sy = h.ss_y;
	int cw = (w + sx) >> sx, ch = (ht + sy) >> sy;
	out[0].alloc(ht, w);
	for (int i = 0; i < ht; ++i) { const uint16_t* s = cur[0][i]; uint16_t* d = out[0][i]; for (int j = 0; j < w; ++j) d[j] = s[j]; }
	out[1].alloc(ch, cw); out[2].alloc(ch, cw);
	for (int i = 0; i < ch; ++i) {
		const uint16_t* s1 = cur[1][i]; uint16_t* d1 = out[1][i];
		const uint16_t* s2 = cur[2][i]; uint16_t* d2 = out[2][i];
		for (int j = 0; j < cw; ++j) { d1[j] = s1[j]; d2[j] = s2[j]; }
	}
}

// Decode a whole VP9 clip to pixels; FNV-1a-64 hash all shown (display-order)
// frames. Mirrors vp9_inter_pixels.py:decode_sequence. Returns the hash and the
// number of shown frames.
// out_rgb (optional, needs as_rgb=true): appends each shown frame's packed RGB
// (w*h*3, top-down) so a caller can play the clip back; out_w/out_h receive the
// (last) frame dims. cancel (optional): checked once per input frame so a
// background decode can bail fast. All default-null => byte-for-byte the
// original hash path (the yuv_rgb harness still passes unchanged).
static inline uint64_t decode_sequence_hash(const uint8_t* blob, size_t bloblen,
		const std::vector<webm::block_t>& frames, int* out_shown, bool as_rgb = false,
		std::vector<std::vector<uint8_t>>* out_rgb = nullptr, int* out_w = nullptr, int* out_h = nullptr,
		const std::atomic<bool>* cancel = nullptr) {
	std::vector<frame_store_entry> store(8);
	std::vector<frame_probs> contexts(4, frame_probs::make_default());
	prev_frame_t prev_a, prev_b; prev_frame_t* prev = nullptr;
	header_parser parser;
	uint64_t hsh = 1469598103934665603ULL;
	auto mix = [&](int v) { hsh ^= (uint8_t)(v & 0xFF); hsh *= 1099511628211ULL; hsh ^= (uint8_t)((v >> 8) & 0xFF); hsh *= 1099511628211ULL; };
	// as_rgb=false: hash the decoded planes (codec space, byte-exact vs libvpx).
	// as_rgb=true: hash RGB (Faze 3.5 colour step), COLOUR-SPACE AWARE:
	//   cs=7 (sRGB, profile-1 4:4:4, the SC:R HD portraits): planes are G/B/R --
	//     a lossless reorder, NO BT.601 matrix (applying it would shift colours).
	//   else (BT.601 4:2:0, VP8-style / rare cs=0 VP9): the BT.601 matrix, chroma
	//     subsampled (sx/sy inferred from the crop widths).
	int clip_cs = yuv::CS_BT_601; // updated per decoded frame below
	auto hash_frame = [&](const px::plane_t pl[3]) {
		if (!as_rgb) {
			for (int p = 0; p < 3; ++p) { const px::plane_t& P = pl[p]; for (int j = 0; j < P.rows(); ++j) { const uint16_t* row = P[j]; for (int i = 0; i < P.cols(); ++i) mix(row[i]); } }
			return;
		}
		int h = pl[0].rows(); if (h == 0) return;
		int w = pl[0].cols();
		uint8_t rgb[3];
		std::vector<uint8_t>* dst = nullptr;
		if (out_rgb) {
			if (out_w) *out_w = w;
			if (out_h) *out_h = h;
			out_rgb->emplace_back();
			dst = &out_rgb->back();
			dst->resize((size_t)w * h * 3);
		}
		size_t di = 0;
		if (clip_cs == yuv::CS_SRGB) {
			// B9: hoist the per-row plane pointers out of the inner loop (contiguous after B5).
			for (int j = 0; j < h; ++j) {
				const uint16_t* g = pl[0][j]; const uint16_t* b = pl[1][j]; const uint16_t* r = pl[2][j];
				for (int i = 0; i < w; ++i) {
					yuv::srgb(g[i], b[i], r[i], rgb);
					mix(rgb[0]); mix(rgb[1]); mix(rgb[2]);
					if (dst) { (*dst)[di++] = rgb[0]; (*dst)[di++] = rgb[1]; (*dst)[di++] = rgb[2]; }
				}
			}
			return;
		}
		int ch = pl[1].rows(), cw = ch ? pl[1].cols() : 0;
		int sx = (cw && cw < w) ? 1 : 0, sy = (ch && ch < h) ? 1 : 0;
		for (int j = 0; j < h; ++j) {
			const uint16_t* Y = pl[0][j];
			const uint16_t* U = pl[1][j >> sy];
			const uint16_t* V = pl[2][j >> sy];
			for (int i = 0; i < w; ++i) {
				yuv::bt601(Y[i], U[i >> sx], V[i >> sx], rgb);
				mix(rgb[0]); mix(rgb[1]); mix(rgb[2]);
				if (dst) { (*dst)[di++] = rgb[0]; (*dst)[di++] = rgb[1]; (*dst)[di++] = rgb[2]; }
			}
		}
	};
	int shown = 0;
	for (const auto& fr : frames) {
		if (cancel && cancel->load()) break;
		const uint8_t* block = blob + fr.offset; size_t blen = fr.size;
		auto parts = webm::vp9_superframe_split(block, blen);
		for (auto& pr : parts) {
			size_t lo = pr.first, hi = pr.second;
			if (hi <= lo || lo >= blen || (block[lo] >> 6) != 2) continue;
			header_t h = parser.parse(block + lo, hi - lo);
			if (!h.ok) continue;
			clip_cs = h.color_space; // colour-space aware RGB (sRGB vs BT.601)
			if (h.show_existing >= 0) {
				if (store[h.show_existing].valid) { hash_frame(store[h.show_existing].crop); ++shown; }
				continue;
			}
			bool fi = h.keyframe || h.intra_only;
			if (fi || h.error_resilient) contexts.assign(4, frame_probs::make_default());
			int ci = h.frame_context_idx;
			frame_probs probs = contexts[ci];
			inter_parse_compressed(probs, h, block, blen, (int)(lo + h.uncompressed_bytes));
			bool use_prev = (prev && prev->valid && prev->w == h.width && prev->h == h.height && prev->show == 1 && !h.error_resilient && !fi);
			frame_pixel_decoder dec(h, probs, fi, use_prev ? prev : nullptr, store.data());
			dec.use_prev_mvs = use_prev;
			// run tiles
			size_t tile_start = lo + h.uncompressed_bytes + h.header_size_in_bytes;
			int tile_cols = 1 << h.tile_cols_log2, tile_rows = 1 << h.tile_rows_log2;
			size_t pos = tile_start;
			for (int tr = 0; tr < tile_rows; ++tr) for (int tc = 0; tc < tile_cols; ++tc) {
				bool last = (tr == tile_rows - 1) && (tc == tile_cols - 1);
				size_t size;
				if (last) size = hi - pos;
				else { size = ((size_t)block[pos] << 24) | ((size_t)block[pos + 1] << 16) | ((size_t)block[pos + 2] << 8) | block[pos + 3]; pos += 4; }
				int r0 = tile_offset(tr, dec.mi_rows, h.tile_rows_log2), r1 = tile_offset(tr + 1, dec.mi_rows, h.tile_rows_log2);
				int c0 = tile_offset(tc, dec.mi_cols, h.tile_cols_log2), c1 = tile_offset(tc + 1, dec.mi_cols, h.tile_cols_log2);
				bool_decoder bd(block, blen, (int)pos, (int)size);
				for (int pl = 0; pl < 3; ++pl) std::fill(dec.above_nz[pl].begin(), dec.above_nz[pl].end(), 0);
				std::fill(dec.above_part.begin(), dec.above_part.end(), 0);
				dec.decode_tile(bd, r0, r1, c0, c1);
				pos += size;
			}
			dec.apply_loop_filter();
			// store entry
			frame_store_entry entry;
			entry.valid = true; entry.w = h.width; entry.h = h.height; entry.sub_x = h.ss_x; entry.sub_y = h.ss_y;
			for (int p = 0; p < 3; ++p) entry.planes[p] = dec.cur[p];
			crop_planes(dec.cur, h, entry.crop);
			if (h.refresh_frame_context) contexts[ci] = probs;
			int rf = h.refresh_frame_flags;
			for (int i = 0; i < 8; ++i) if (rf & (1 << i)) store[i] = entry;
			if (h.show_frame == 1) { hash_frame(entry.crop); ++shown; }
			prev_frame_t& nxt = (prev == &prev_a) ? prev_b : prev_a;
			nxt.mv = std::move(dec.mv_grid); nxt.ref = std::move(dec.ref_grid);
			nxt.w = h.width; nxt.h = h.height; nxt.show = h.show_frame; nxt.valid = true;
			prev = &nxt;
		}
	}
	if (out_shown) *out_shown = shown;
	return hsh;
}

} // namespace vp9
} // namespace bwgame

#endif
