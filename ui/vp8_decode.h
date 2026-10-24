// vp8_decode.h -- VP8 full decoder (Faze 3.x-c/d/e of open1161HD).
//
// Ported from the grp-toolkit oracle tools/vp8_decode.py (RFC 6386): mode +
// coefficient decoding, dequant, inverse WHT/DCT, all 14 intra prediction
// modes, the normal in-loop deblocking filter, and inter frames (stage e):
// motion-vector decoding, 6-tap sub-pel prediction, chroma-MV derivation and
// reference-frame management. A stateful vp8_decoder persists the probability
// tables and reference frames that inter frames build on. Pure presentation;
// no determinism impact.
#ifndef BWGAME_UI_VP8_DECODE_H
#define BWGAME_UI_VP8_DECODE_H

#include "vp8.h"
#include "vp8_tables.h"
#include "yuv_rgb.h"
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <memory>

namespace bwgame {
namespace vp8 {

namespace T = tables;

// Intra modes 0..4, inter modes 5..9 (RFC 6386 §9, matching the oracle enum).
enum { DC_PRED = 0, V_PRED = 1, H_PRED = 2, TM_PRED = 3, B_PRED = 4,
       NEARESTMV = 5, NEARMV = 6, ZEROMV = 7, NEWMV = 8, SPLITMV = 9,
       B_DC_PRED = 0, B_TM_PRED = 1, B_VE_PRED = 2, B_HE_PRED = 3, B_LD_PRED = 4,
       B_RD_PRED = 5, B_VR_PRED = 6, B_VL_PRED = 7, B_HD_PRED = 8, B_HU_PRED = 9,
       LEFT4X4 = 10, ABOVE4X4 = 11, ZERO4X4 = 12, NEW4X4 = 13,
       CURRENT_FRAME = 0, LAST_FRAME = 1, GOLDEN_FRAME = 2, ALTREF_FRAME = 3,
       DCT_0 = 0, DCT_4 = 4, DCT_CAT1 = 5, DCT_EOB = 11 };

static const int CAT_BASE[6] = { 5, 7, 11, 19, 35, 67 };
static const int ABOVE_CTX[25] = { 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3, 4,5,4,5, 6,7,6,7, 8 };
static const int LEFT_CTX[25]  = { 0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3, 4,4,5,5, 6,6,7,7, 8 };

struct mv_t {
	int x = 0, y = 0;
	bool operator==(const mv_t& o) const { return x == o.x && y == o.y; }
	bool operator!=(const mv_t& o) const { return !(*this == o); }
};

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline int implied_bmode(int ymode) {
	switch (ymode) { case DC_PRED: return B_DC_PRED; case V_PRED: return B_VE_PRED;
		case H_PRED: return B_HE_PRED; case TM_PRED: return B_TM_PRED; default: return B_DC_PRED; }
}

template <class ProbFn>
static inline int treed_read(bool_decoder& bd, const T::I1& tree, ProbFn prob, int start = 0) {
	int i = start;
	for (;;) { i = tree[i + bd.get_bool(prob(i >> 1))]; if (i <= 0) return -i; }
}
static inline int treed(bool_decoder& bd, const T::I1& tree, const T::I1& probs, int start = 0) {
	return treed_read(bd, tree, [&](int k) { return probs[k]; }, start);
}

// ---- transforms (RFC 6386 §14) ----
enum { COSPI8SQRT2MINUS1 = 20091, SINPI8SQRT2 = 35468 };
static inline int mul1(int x) { return x + ((x * COSPI8SQRT2MINUS1) >> 16); }
static inline int mul2(int x) { return (x * SINPI8SQRT2) >> 16; }

static inline void idct4x4(const int* b, int* o) {
	int t[16];
	for (int i = 0; i < 4; ++i) {
		int a1 = b[i] + b[i + 8], b1 = b[i] - b[i + 8];
		int c1 = mul2(b[i + 4]) - mul1(b[i + 12]);
		int d1 = mul1(b[i + 4]) + mul2(b[i + 12]);
		t[i] = a1 + d1; t[i + 12] = a1 - d1; t[i + 4] = b1 + c1; t[i + 8] = b1 - c1;
	}
	for (int i = 0; i < 16; i += 4) {
		int a1 = t[i] + t[i + 2], b1 = t[i] - t[i + 2];
		int c1 = mul2(t[i + 1]) - mul1(t[i + 3]);
		int d1 = mul1(t[i + 1]) + mul2(t[i + 3]);
		o[i] = (a1 + d1 + 4) >> 3; o[i + 3] = (a1 - d1 + 4) >> 3;
		o[i + 1] = (b1 + c1 + 4) >> 3; o[i + 2] = (b1 - c1 + 4) >> 3;
	}
}
static inline void iwht4x4(const int* b, int* o) {
	int t[16];
	for (int i = 0; i < 4; ++i) {
		int a1 = b[i] + b[i + 12], b1 = b[i + 4] + b[i + 8];
		int c1 = b[i + 4] - b[i + 8], d1 = b[i] - b[i + 12];
		t[i] = a1 + b1; t[i + 4] = c1 + d1; t[i + 8] = a1 - b1; t[i + 12] = d1 - c1;
	}
	for (int i = 0; i < 16; i += 4) {
		int a1 = t[i] + t[i + 3], b1 = t[i + 1] + t[i + 2];
		int c1 = t[i + 1] - t[i + 2], d1 = t[i] - t[i + 3];
		o[i] = (a1 + b1 + 3) >> 3; o[i + 1] = (c1 + d1 + 3) >> 3;
		o[i + 2] = (a1 - b1 + 3) >> 3; o[i + 3] = (d1 - c1 + 3) >> 3;
	}
}

// ---- plane with the spec's out-of-frame border ----
struct plane_t {
	int w, h, stride;
	std::vector<uint8_t> buf;
	plane_t(int W = 0, int H = 0) : w(W), h(H), stride(W + 1 + 4), buf((size_t)stride * (H + 1), 0) {
		for (int i = 0; i < stride; ++i) buf[i] = 127;
		for (int r = 1; r <= h; ++r) buf[(size_t)r * stride] = 129;
	}
	int at(int x, int y) const { return buf[(size_t)(y + 1) * stride + (x + 1)]; }
	// A reference pixel with coordinates clamped to the plane (edge replication,
	// matching the reference decoder's build_mc_border, RFC 6386 §18).
	int sample(int x, int y) const {
		if (x < 0) x = 0; else if (x >= w) x = w - 1;
		if (y < 0) y = 0; else if (y >= h) y = h - 1;
		return buf[(size_t)(y + 1) * stride + (x + 1)];
	}
	int row_off(int y) const { return (y + 1) * stride + 1; }
	void extend_right(int y) {
		int o = row_off(y); uint8_t v = buf[o + w - 1];
		for (int i = 0; i < 4; ++i) buf[o + w + i] = v;
	}
};

// ---- prediction ----
static inline int pred_dc(const plane_t& pl, int x, int y, int size, bool have_above, bool have_left) {
	int s = 0, n = 0;
	if (have_above) { int o = pl.row_off(y - 1); for (int i = 0; i < size; ++i) s += pl.buf[o + x + i]; n += size; }
	if (have_left) { for (int j = 0; j < size; ++j) s += pl.at(x - 1, y + j); n += size; }
	if (!n) return 128;
	int bl = 0; { int t = size; while (t > 1) { t >>= 1; ++bl; } } // size.bit_length()-1
	int shf = bl + (n == 2 * size ? 1 : 0);
	return (s + (1 << (shf - 1))) >> shf;
}
static inline void predict_block(plane_t& pl, int x, int y, int size, int mode, bool have_above, bool have_left) {
	if (mode == DC_PRED) {
		int v = pred_dc(pl, x, y, size, have_above, have_left);
		for (int j = 0; j < size; ++j) { int o = pl.row_off(y + j) + x; for (int i = 0; i < size; ++i) pl.buf[o + i] = (uint8_t)v; }
	} else if (mode == V_PRED) {
		int ao = pl.row_off(y - 1) + x;
		for (int j = 0; j < size; ++j) { int o = pl.row_off(y + j) + x; for (int i = 0; i < size; ++i) pl.buf[o + i] = pl.buf[ao + i]; }
	} else if (mode == H_PRED) {
		for (int j = 0; j < size; ++j) { int o = pl.row_off(y + j) + x; uint8_t v = (uint8_t)pl.at(x - 1, y + j); for (int i = 0; i < size; ++i) pl.buf[o + i] = v; }
	} else { // TM_PRED
		int p = pl.at(x - 1, y - 1), ao = pl.row_off(y - 1) + x;
		for (int j = 0; j < size; ++j) { int left = pl.at(x - 1, y + j), o = pl.row_off(y + j) + x;
			for (int i = 0; i < size; ++i) pl.buf[o + i] = (uint8_t)clamp255(left + pl.buf[ao + i] - p); }
	}
}
static inline void predict_subblock(plane_t& pl, int x, int y, int mode, const int* above_right) {
	int A[8], L[4], P = pl.at(x - 1, y - 1);
	for (int i = 0; i < 4; ++i) A[i] = pl.at(x + i, y - 1);
	for (int i = 0; i < 4; ++i) A[4 + i] = above_right[i];
	for (int j = 0; j < 4; ++j) L[j] = pl.at(x - 1, y + j);
	auto a3 = [](int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; };
	auto a2 = [](int a, int b) { return (a + b + 1) >> 1; };
	int b[16];
	switch (mode) {
	case B_DC_PRED: { int v = (A[0]+A[1]+A[2]+A[3]+L[0]+L[1]+L[2]+L[3] + 4) >> 3; for (int i = 0; i < 16; ++i) b[i] = v; } break;
	case B_TM_PRED: for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) b[j*4+i] = clamp255(L[j] + A[i] - P); break;
	case B_VE_PRED: { int e[4] = { a3(P,A[0],A[1]), a3(A[0],A[1],A[2]), a3(A[1],A[2],A[3]), a3(A[2],A[3],A[4]) }; for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) b[j*4+i] = e[i]; } break;
	case B_HE_PRED: { int e[4] = { a3(P,L[0],L[1]), a3(L[0],L[1],L[2]), a3(L[1],L[2],L[3]), a3(L[2],L[3],L[3]) }; for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) b[j*4+i] = e[j]; } break;
	case B_LD_PRED: { int e[7]; for (int i = 0; i < 6; ++i) e[i] = a3(A[i],A[i+1],A[i+2]); e[6] = a3(A[6],A[7],A[7]); for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) b[j*4+i] = e[i+j]; } break;
	case B_RD_PRED: { int e[7] = { a3(L[3],L[2],L[1]), a3(L[2],L[1],L[0]), a3(L[1],L[0],P), a3(L[0],P,A[0]), a3(P,A[0],A[1]), a3(A[0],A[1],A[2]), a3(A[1],A[2],A[3]) }; for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) b[j*4+i] = e[i-j+3]; } break;
	case B_VR_PRED: { int e[10] = { a3(L[2],L[1],L[0]), a3(L[1],L[0],P), a3(L[0],P,A[0]), a2(P,A[0]), a3(P,A[0],A[1]), a2(A[0],A[1]), a3(A[0],A[1],A[2]), a2(A[1],A[2]), a3(A[1],A[2],A[3]), a2(A[2],A[3]) };
		int idx[16] = { 3,5,7,9, 2,4,6,8, 1,3,5,7, 0,2,4,6 }; for (int i = 0; i < 16; ++i) b[i] = e[idx[i]]; } break;
	case B_VL_PRED: { int e[10] = { a2(A[0],A[1]), a3(A[0],A[1],A[2]), a2(A[1],A[2]), a3(A[1],A[2],A[3]), a2(A[2],A[3]), a3(A[2],A[3],A[4]), a2(A[3],A[4]), a3(A[3],A[4],A[5]), a3(A[4],A[5],A[6]), a3(A[5],A[6],A[7]) };
		int idx[16] = { 0,2,4,6, 1,3,5,7, 2,4,6,8, 3,5,7,9 }; for (int i = 0; i < 16; ++i) b[i] = e[idx[i]]; } break;
	case B_HD_PRED: { int e[10] = { a2(L[3],L[2]), a3(L[3],L[2],L[1]), a2(L[2],L[1]), a3(L[2],L[1],L[0]), a2(L[1],L[0]), a3(L[1],L[0],P), a2(L[0],P), a3(L[0],P,A[0]), a3(P,A[0],A[1]), a3(A[0],A[1],A[2]) };
		int idx[16] = { 6,7,8,9, 4,5,6,7, 2,3,4,5, 0,1,2,3 }; for (int i = 0; i < 16; ++i) b[i] = e[idx[i]]; } break;
	default: { // B_HU_PRED
		int e[10] = { a2(L[0],L[1]), a3(L[0],L[1],L[2]), a2(L[1],L[2]), a3(L[1],L[2],L[3]), a2(L[2],L[3]), a3(L[2],L[3],L[3]), L[3], L[3], L[3], L[3] };
		int idx[16] = { 0,1,2,3, 2,3,4,5, 4,5,6,7, 6,7,8,9 }; for (int i = 0; i < 16; ++i) b[i] = e[idx[i]]; } break;
	}
	for (int j = 0; j < 4; ++j) { int o = pl.row_off(y + j) + x; for (int i = 0; i < 4; ++i) pl.buf[o + i] = (uint8_t)b[j*4+i]; }
}
static inline void add_residue(plane_t& pl, int x, int y, const int* res) {
	for (int j = 0; j < 4; ++j) { int o = pl.row_off(y + j) + x; for (int i = 0; i < 4; ++i) pl.buf[o + i] = (uint8_t)clamp255(pl.buf[o + i] + res[j*4+i]); }
}

// ---- inter prediction (stage e, RFC 6386 §18) ----
// Motion-compensate one 4x4 block from `ref` into `dst` at (bx,by), then add
// the residual: a whole-pel offset plus a 6-tap sub-pel filter, horizontal
// then vertical, each rounded (sum + 64) >> 7.
static inline void inter_block(plane_t& dst, const plane_t& ref, int bx, int by, int mvx, int mvy, const int* res) {
	int ix = mvx >> 3, iy = mvy >> 3;   // whole-pel part (arithmetic shift = floor)
	int mx = mvx & 7, my = mvy & 7;     // sub-pel phase 0..7
	int ox = bx + ix, oy = by + iy;
	int pred[16];
	if (mx == 0 && my == 0) {
		for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) pred[r*4+c] = ref.sample(ox + c, oy + r);
	} else {
		const T::I1& fh = T::SIXTAP_FILTERS[mx];
		const T::I1& fv = T::SIXTAP_FILTERS[my];
		int tmp[9][4];
		// B8: the 6-tap horizontal window spans x in [ox-2, ox+6], y in [oy-2, oy+6].
		// When it is wholly inside the frame, no edge clamp is needed -- index buf
		// directly (216 clamped sample() calls per block otherwise).
		bool interior = (ox - 2 >= 0 && ox + 6 <= ref.w - 1 && oy - 2 >= 0 && oy + 6 <= ref.h - 1);
		if (interior) {
			const uint8_t* rb = ref.buf.data(); int st = ref.stride;
			for (int r = -2; r < 7; ++r) {
				const uint8_t* rr = rb + (size_t)(oy + r + 1) * st + (ox + 1); // pixel (ox, oy+r)
				for (int c = 0; c < 4; ++c) {
					const uint8_t* p = rr + c;
					int s = (p[-2] * fh[0] + p[-1] * fh[1] + p[0] * fh[2]
						+ p[1] * fh[3] + p[2] * fh[4] + p[3] * fh[5] + 64) >> 7;
					tmp[r + 2][c] = clamp255(s);
				}
			}
		} else {
			for (int r = -2; r < 7; ++r) {
				for (int c = 0; c < 4; ++c) {
					int s = (ref.sample(ox + c - 2, oy + r) * fh[0]
						+ ref.sample(ox + c - 1, oy + r) * fh[1]
						+ ref.sample(ox + c,     oy + r) * fh[2]
						+ ref.sample(ox + c + 1, oy + r) * fh[3]
						+ ref.sample(ox + c + 2, oy + r) * fh[4]
						+ ref.sample(ox + c + 3, oy + r) * fh[5] + 64) >> 7;
					tmp[r + 2][c] = clamp255(s);
				}
			}
		}
		for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
			int s = (tmp[r][c] * fv[0] + tmp[r+1][c] * fv[1] + tmp[r+2][c] * fv[2]
				+ tmp[r+3][c] * fv[3] + tmp[r+4][c] * fv[4] + tmp[r+5][c] * fv[5] + 64) >> 7;
			pred[r*4+c] = clamp255(s);
		}
	}
	for (int j = 0; j < 4; ++j) { int o = dst.row_off(by + j) + bx; for (int i = 0; i < 4; ++i) dst.buf[o + i] = (uint8_t)clamp255(pred[j*4+i] + res[j*4+i]); }
}

static inline int cdiv(int a, int b) { // C truncation toward zero (== C++ '/')
	return a / b;
}
static inline mv_t chroma_mv_from_luma(int mvx, int mvy) {
	auto hf = [](int v) { return cdiv(v + 1 + (v < 0 ? -2 : 0), 2); };
	return { hf(mvx), hf(mvy) };
}
static inline mv_t chroma_splitmv(const std::array<mv_t,16>& mvs, int b) {
	auto avg = [](int a, int b_, int c, int d) { int t = a + b_ + c + d; t += (t < 0 ? -4 : 4); return cdiv(t, 8); };
	int x = avg(mvs[b].x, mvs[b+1].x, mvs[b+4].x, mvs[b+5].x);
	int y = avg(mvs[b].y, mvs[b+1].y, mvs[b+4].y, mvs[b+5].y);
	return { x, y };
}

// ---- header ----
struct hdr_t {
	bool keyframe = false;
	int version = 0; bool show_frame = false; int first_part_size = 0;
	int width = 0, height = 0;
	int y_ac_qi = 0, y_dc_delta = 0, y2_dc_delta = 0, y2_ac_delta = 0, uv_dc_delta = 0, uv_ac_delta = 0;
	int filter_type = 0, loop_filter_level = 0, sharpness_level = 0, lf_adj_enable = 0;
	int ref_lf_deltas[4] = { 0,0,0,0 }, mode_lf_deltas[4] = { 0,0,0,0 };
	int segmentation_enabled = 0, update_mb_seg_map = 0;
	int segment_tree_probs[3] = { 255,255,255 };
	bool has_segment_features = false;
	int segment_quant[4] = { 0,0,0,0 }, segment_lf[4] = { 0,0,0,0 }, segment_feature_mode = 0;
	int sign_bias[4] = { 0,0,0,0 };
	int refresh_last = 1, refresh_gf = 1, refresh_arf = 1, copy_gf = 0, copy_arf = 0;
	int mb_no_coeff_skip = 0, prob_skip_false = 0;
	int prob_intra = 0, prob_last = 0, prob_gf = 0;
};

// ---- dequant + coefficient decode ----
struct dq_t { int y1[2], uv[2], y2[2]; };
static inline int clamp_q(int q) { return q < 0 ? 0 : (q > 127 ? 127 : q); }
static inline dq_t dequant_factors(const hdr_t& h, int seg_id) {
	int q = h.y_ac_qi;
	if (h.segmentation_enabled && h.has_segment_features) {
		int sq = h.segment_quant[seg_id];
		q = h.segment_feature_mode ? sq : q + sq;
	}
	auto dc = [&](int d) { return T::DC_QLOOKUP[clamp_q(q + d)]; };
	auto ac = [&](int d) { return T::AC_QLOOKUP[clamp_q(q + d)]; };
	int y2ac = ac(h.y2_ac_delta) * 155 / 100;
	dq_t r;
	r.y1[0] = dc(h.y_dc_delta); r.y1[1] = ac(0);
	r.uv[0] = std::min(dc(h.uv_dc_delta), 132); r.uv[1] = ac(h.uv_ac_delta);
	r.y2[0] = dc(h.y2_dc_delta) * 2; r.y2[1] = std::max(y2ac, 8);
	return r;
}
static inline const T::I1& cat_probs(int cat) {
	switch (cat) { case 0: return T::PCAT1; case 1: return T::PCAT2; case 2: return T::PCAT3;
		case 3: return T::PCAT4; case 4: return T::PCAT5; default: return T::PCAT6; }
}
static inline bool decode_coeffs(bool_decoder& bd, const T::I4& probs, int plane, int ctx, const int* dq, int* out) {
	int i = (plane == 0) ? 1 : 0;
	bool prev_zero = false, nonzero = false;
	const T::I3& tbl = probs[plane];
	while (i < 16) {
		const T::I1& p = tbl[T::COEFF_BANDS[i]][ctx];
		int tok = treed(bd, T::COEFF_TREE, p, prev_zero ? 2 : 0);
		if (tok == DCT_EOB) break;
		if (tok == DCT_0) { ctx = 0; prev_zero = true; ++i; continue; }
		prev_zero = false; nonzero = true;
		int v;
		if (tok <= DCT_4) v = tok;
		else {
			int cat = tok - DCT_CAT1, extra = 0;
			const T::I1& cp = cat_probs(cat);
			for (size_t k = 0; k + 1 < cp.size(); ++k) extra = extra + extra + bd.get_bool(cp[k]);
			v = CAT_BASE[cat] + extra;
		}
		ctx = (v == 1) ? 1 : 2;
		if (bd.get_bool(128)) v = -v;
		out[T::ZIGZAG[i]] = v * (i == 0 ? dq[0] : dq[1]);
		++i;
	}
	return nonzero;
}

// ---- MV component decode (RFC 6386 §17) ----
static inline int read_mv_component(bool_decoder& bd, const int* mvc) {
	enum { IS_SHORT = 0, SIGN = 1, SHORT = 2, BITS = 9, LONG_W = 10 };
	int x;
	if (bd.get_bool(mvc[IS_SHORT])) {              // long
		x = 0;
		for (int i = 0; i < 3; ++i) x += bd.get_bool(mvc[BITS + i]) << i;
		for (int i = LONG_W - 1; i > 3; --i) x += bd.get_bool(mvc[BITS + i]) << i;
		if (!(x & 0xFFF0) || bd.get_bool(mvc[BITS + 3])) x += 8;
	} else {                                       // short
		x = treed_read(bd, T::SMALL_MV_TREE, [&](int k) { return mvc[SHORT + k]; });
	}
	if (x && bd.get_bool(mvc[SIGN])) x = -x;
	return x << 1;
}
// mv probs mvp[2][19]; component 0 = row (y), component 1 = col (x).
static inline mv_t read_mv(bool_decoder& bd, const int mvp[2][19]) {
	int y = read_mv_component(bd, mvp[0]);
	int x = read_mv_component(bd, mvp[1]);
	return { x, y };
}

// ---- loop filter (RFC 6386 §15, normal) ----
static inline int s8(int x) { return x < -128 ? -128 : (x > 127 ? 127 : x); }
static inline int clamp_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline void common_adjust(std::vector<uint8_t>& buf, int pos, int step, bool outer) {
	int p1 = buf[pos - 2*step], p0 = buf[pos - step], q0 = buf[pos], q1 = buf[pos + step];
	int a = 3 * (q0 - p0) + (outer ? s8(p1 - q1) : 0);
	a = s8(a);
	int f1 = std::min(a + 4, 127) >> 3, f2 = std::min(a + 3, 127) >> 3;
	buf[pos - step] = (uint8_t)clamp_u8(p0 + f2);
	buf[pos] = (uint8_t)clamp_u8(q0 - f1);
	if (!outer) { int aa = (f1 + 1) >> 1; buf[pos + step] = (uint8_t)clamp_u8(q1 - aa); buf[pos - 2*step] = (uint8_t)clamp_u8(p1 + aa); }
}
static inline void mb_edge(std::vector<uint8_t>& buf, int pos, int step) {
	int p2 = buf[pos-3*step], p1 = buf[pos-2*step], p0 = buf[pos-step], q0 = buf[pos], q1 = buf[pos+step], q2 = buf[pos+2*step];
	int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
	int a = (27 * w + 63) >> 7; buf[pos-step] = (uint8_t)clamp_u8(p0 + a); buf[pos] = (uint8_t)clamp_u8(q0 - a);
	a = (18 * w + 63) >> 7; buf[pos-2*step] = (uint8_t)clamp_u8(p1 + a); buf[pos+step] = (uint8_t)clamp_u8(q1 - a);
	a = (9 * w + 63) >> 7; buf[pos-3*step] = (uint8_t)clamp_u8(p2 + a); buf[pos+2*step] = (uint8_t)clamp_u8(q2 - a);
}
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline bool threshold_ok(const std::vector<uint8_t>& buf, int pos, int step, int E, int I) {
	int p3 = buf[pos-4*step], p2 = buf[pos-3*step], p1 = buf[pos-2*step], p0 = buf[pos-step];
	int q0 = buf[pos], q1 = buf[pos+step], q2 = buf[pos+2*step], q3 = buf[pos+3*step];
	if (iabs(p0 - q0) * 2 + (iabs(p1 - q1) >> 1) > 2 * E + I) return false;
	return iabs(p3-p2) <= I && iabs(p2-p1) <= I && iabs(p1-p0) <= I && iabs(q3-q2) <= I && iabs(q2-q1) <= I && iabs(q1-q0) <= I;
}
static inline bool hev(const std::vector<uint8_t>& buf, int pos, int step, int thr) {
	return iabs(buf[pos-2*step]-buf[pos-step]) > thr || iabs(buf[pos+step]-buf[pos]) > thr;
}
static inline void filter_edge(std::vector<uint8_t>& buf, int start, int step, int run, int count, int E, int I, int hevt, bool mb) {
	int pos = start;
	for (int c = 0; c < count; ++c) {
		if (threshold_ok(buf, pos, step, E, I)) {
			bool hv = hev(buf, pos, step, hevt);
			if (mb) { if (hv) common_adjust(buf, pos, step, true); else mb_edge(buf, pos, step); }
			else common_adjust(buf, pos, step, hv);
		}
		pos += run;
	}
}

// ---- per-MB info ----
struct mb_info {
	int ymode = DC_PRED, uvmode = DC_PRED, ref = CURRENT_FRAME, seg = 0;
	bool skip = false, has_coeffs = false;
	mv_t mv;
	std::array<int, 16> smodes;
	std::array<mv_t, 16> smvs;
	mb_info() { smodes.fill(B_DC_PRED); }
};

// ---- decoded frame ----
struct frame {
	hdr_t h;
	int mb_cols = 0, mb_rows = 0, width = 0, height = 0;
	bool show_frame = true;
	plane_t y, u, v;
	// per-MB { ymode, seg, has_coeffs, ref } for the loop filter
	std::vector<std::array<int, 4>> mb;
};
typedef frame keyframe_frame; // back-compat alias for the stage c/d harness

static inline void loop_filter(frame& fr, const hdr_t& h); // below

static inline int filter_level(const hdr_t& h, int seg_id, int ymode, int ref_frame) {
	int lvl = h.loop_filter_level;
	if (h.segmentation_enabled && h.has_segment_features) {
		int sl = h.segment_lf[seg_id];
		lvl = h.segment_feature_mode ? sl : lvl + sl;
	}
	lvl = lvl < 0 ? 0 : (lvl > 63 ? 63 : lvl);
	if (h.lf_adj_enable) {
		lvl += h.ref_lf_deltas[ref_frame];
		if (ref_frame == CURRENT_FRAME) { if (ymode == B_PRED) lvl += h.mode_lf_deltas[0]; }
		else if (ymode == ZEROMV) lvl += h.mode_lf_deltas[1];
		else if (ymode == SPLITMV) lvl += h.mode_lf_deltas[3];
		else lvl += h.mode_lf_deltas[2];
		lvl = lvl < 0 ? 0 : (lvl > 63 ? 63 : lvl);
	}
	return lvl;
}

// ---- stateful decoder ----
struct entropy_t {
	T::I4 coeff;
	int mv[2][19];
	int ymode[4];
	int uvmode[3];
};

class vp8_decoder {
public:
	std::shared_ptr<frame> refs[4]; // index by LAST/GOLDEN/ALTREF
	bool have_entropy = false;
	entropy_t entropy;
	// persistent segmentation / loop filter (reset on keyframe)
	bool seg_enabled = false; int seg_update_map = 0; int seg_tree[3] = { 255,255,255 };
	bool seg_has_features = false; int seg_quant[4] = { 0,0,0,0 }, seg_lf[4] = { 0,0,0,0 }, seg_abs = 0;
	int lf_type = 0, lf_level = 0, lf_sharp = 0, lf_delta_enabled = 0;
	int lf_ref_deltas[4] = { 0,0,0,0 }, lf_mode_deltas[4] = { 0,0,0,0 };
	int width = 0, height = 0, mb_cols = 0, mb_rows = 0;

	hdr_t hdr;

	void reset_entropy() {
		entropy.coeff = T::DEFAULT_COEFF_PROBS;
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 19; ++j) entropy.mv[i][j] = T::DEFAULT_MV_PROBS[i][j];
		for (int i = 0; i < 4; ++i) entropy.ymode[i] = T::DEFAULT_Y_MODE_PROBS[i];
		for (int i = 0; i < 3; ++i) entropy.uvmode[i] = T::DEFAULT_UV_MODE_PROBS[i];
		have_entropy = true;
	}

	// Decode one frame. Returns null on failure; keyframe resets all state.
	std::shared_ptr<frame> decode_frame(const uint8_t* data, size_t n, bool filtered = true) {
		bool_decoder b(data, n, 0);
		std::vector<std::pair<size_t,size_t>> parts;
		if (!parse_header(data, n, b, parts)) return nullptr;
		bool key = hdr.keyframe;
		if (!key && !refs[LAST_FRAME]) return nullptr; // inter before keyframe

		auto frp = std::make_shared<frame>();
		frame& fr = *frp;
		fr.h = hdr; fr.width = width; fr.height = height;
		fr.mb_cols = mb_cols; fr.mb_rows = mb_rows; fr.show_frame = hdr.show_frame;
		fr.y = plane_t(mb_cols * 16, mb_rows * 16);
		fr.u = plane_t(mb_cols * 8, mb_rows * 8);
		fr.v = plane_t(mb_cols * 8, mb_rows * 8);
		fr.mb.assign((size_t)mb_cols * mb_rows, { DC_PRED, 0, 0, CURRENT_FRAME });

		std::vector<bool_decoder> res;
		for (auto& p : parts) res.emplace_back(data + p.first, p.second);
		int np = (int)res.size();

		dq_t dqs[4]; for (int s = 0; s < 4; ++s) dqs[s] = dequant_factors(hdr, s);

		std::vector<std::array<int, 9>> above_ctx(mb_cols);
		for (auto& a : above_ctx) a.fill(0);
		std::vector<mb_info> above_row(mb_cols + 1); // [0] = above-left border

		for (int mby = 0; mby < mb_rows; ++mby) {
			std::array<int, 9> left_ctx; left_ctx.fill(0);
			bool_decoder& rbd = res[mby & (np - 1)];
			std::vector<mb_info> cur_row(mb_cols + 1);
			mb_info left; // border
			for (int mbx = 0; mbx < mb_cols; ++mbx) {
				mb_info cur;
				mb_info& above = above_row[mbx + 1];
				mb_info& aboveleft = above_row[mbx];

				if (hdr.update_mb_seg_map) cur.seg = read_segment_id(b);
				if (hdr.mb_no_coeff_skip) cur.skip = b.get_bool(hdr.prob_skip_false) != 0;

				if (key) {
					decode_kf_mb_mode(b, cur, left, above);
				} else {
					if (b.get_bool(hdr.prob_intra)) {
						int bounds[4] = { -((mbx + 1) << 7), (mb_cols - mbx) << 7, -((mby + 1) << 7), (mb_rows - mby) << 7 };
						decode_mvs(b, cur, left, above, aboveleft, bounds);
					} else {
						decode_intra_mb_mode(b, cur);
					}
				}

				reconstruct_mb(fr, mbx, mby, cur, rbd, dqs, above_ctx, left_ctx);
				fr.mb[(size_t)mby * mb_cols + mbx] = { cur.ymode, cur.seg, cur.has_coeffs ? 1 : 0, cur.ref };
				cur_row[mbx + 1] = cur;
				left = cur;
			}
			above_row = cur_row;
			fr.y.extend_right(mby * 16 + 15);
			fr.u.extend_right(mby * 8 + 7);
			fr.v.extend_right(mby * 8 + 7);
		}

		if (filtered && hdr.loop_filter_level) loop_filter(fr, hdr);

		if (!refresh_entropy_) entropy = saved_entropy_;
		update_refs(frp);
		return frp;
	}

private:
	bool refresh_entropy_ = true;
	entropy_t saved_entropy_;

	bool parse_header(const uint8_t* data, size_t n, bool_decoder& b, std::vector<std::pair<size_t,size_t>>& parts) {
		frame_tag tag = parse_frame_tag(data, n);
		if (!tag.ok) return false;
		hdr = hdr_t();
		hdr.keyframe = tag.keyframe; hdr.version = tag.version;
		hdr.show_frame = tag.show_frame; hdr.first_part_size = tag.first_part_size;
		bool key = tag.keyframe;
		size_t part0_at;
		if (key) {
			if (n < 10 || data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a) return false;
			int w_raw = data[6] | (data[7] << 8), h_raw = data[8] | (data[9] << 8);
			width = w_raw & 0x3FFF; height = h_raw & 0x3FFF;
			mb_cols = (width + 15) / 16; mb_rows = (height + 15) / 16;
			part0_at = 10;
		} else {
			part0_at = 3;
		}
		hdr.width = width; hdr.height = height;

		b = bool_decoder(data, n, part0_at);
		if (key) b.literal(2); // colour space + clamping

		// -- segmentation (persistent; reset on keyframe) --
		if (key) { seg_enabled = false; seg_update_map = 0; seg_tree[0] = seg_tree[1] = seg_tree[2] = 255;
			seg_has_features = false; for (int i = 0; i < 4; ++i) { seg_quant[i] = 0; seg_lf[i] = 0; } seg_abs = 0; }
		seg_enabled = b.literal(1) != 0;
		seg_update_map = 0;
		if (seg_enabled) {
			seg_update_map = b.literal(1);
			int update_data = b.literal(1);
			if (update_data) {
				seg_abs = b.literal(1);
				for (int i = 0; i < 4; ++i) seg_quant[i] = b.flagged(7);
				for (int i = 0; i < 4; ++i) seg_lf[i] = b.flagged(6);
				seg_has_features = true;
			}
			if (seg_update_map)
				for (int i = 0; i < 3; ++i) seg_tree[i] = b.get_bool(128) ? b.literal(8) : 255;
		}

		// -- loop filter (persistent; reset on keyframe) --
		if (key) { for (int i = 0; i < 4; ++i) { lf_ref_deltas[i] = 0; lf_mode_deltas[i] = 0; } }
		lf_type = b.literal(1);
		lf_level = b.literal(6);
		lf_sharp = b.literal(3);
		lf_delta_enabled = b.literal(1);
		if (lf_delta_enabled && b.literal(1)) {
			for (int i = 0; i < 4; ++i) lf_ref_deltas[i] = b.flagged(6);
			for (int i = 0; i < 4; ++i) lf_mode_deltas[i] = b.flagged(6);
		}

		// -- token partitions --
		int nparts = 1 << b.literal(2);
		size_t first = part0_at + (size_t)hdr.first_part_size;
		size_t off = first + 3 * (size_t)(nparts - 1);
		for (int i = 0; i < nparts; ++i) {
			size_t sz;
			if (i < nparts - 1) { const uint8_t* p = data + first + 3 * i; if (first + 3 * i + 3 > n) return false; sz = p[0] | (p[1] << 8) | (p[2] << 16); }
			else sz = n - off;
			if (off + sz > n) return false;
			parts.emplace_back(off, sz);
			off += sz;
		}

		// -- quantiser (fresh every frame) --
		hdr.y_ac_qi = b.literal(7);
		hdr.y_dc_delta = b.flagged(4);
		hdr.y2_dc_delta = b.flagged(4);
		hdr.y2_ac_delta = b.flagged(4);
		hdr.uv_dc_delta = b.flagged(4);
		hdr.uv_ac_delta = b.flagged(4);

		// -- reference header --
		int refresh_entropy;
		if (key) {
			hdr.refresh_gf = hdr.refresh_arf = hdr.refresh_last = 1;
			hdr.copy_gf = hdr.copy_arf = 0;
			hdr.sign_bias[0] = hdr.sign_bias[1] = hdr.sign_bias[2] = hdr.sign_bias[3] = 0;
			refresh_entropy = b.literal(1);
		} else {
			hdr.refresh_gf = b.literal(1);
			hdr.refresh_arf = b.literal(1);
			hdr.copy_gf = hdr.refresh_gf ? 0 : b.literal(2);
			hdr.copy_arf = hdr.refresh_arf ? 0 : b.literal(2);
			hdr.sign_bias[2] = b.literal(1);
			hdr.sign_bias[3] = b.literal(1);
			refresh_entropy = b.literal(1);
			hdr.refresh_last = b.literal(1);
		}

		// -- entropy (persistent; defaults restored on keyframe) --
		if (key || !have_entropy) reset_entropy();
		refresh_entropy_ = refresh_entropy != 0;
		if (!refresh_entropy_) saved_entropy_ = entropy;

		for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) for (int k = 0; k < 3; ++k) for (int t = 0; t < 11; ++t)
			if (b.get_bool(T::COEFF_UPDATE_PROBS[i][j][k][t])) entropy.coeff[i][j][k][t] = b.literal(8);
		hdr.mb_no_coeff_skip = b.literal(1);
		hdr.prob_skip_false = hdr.mb_no_coeff_skip ? b.literal(8) : 0;
		if (!key) {
			hdr.prob_intra = b.literal(8);
			hdr.prob_last = b.literal(8);
			hdr.prob_gf = b.literal(8);
			if (b.literal(1)) for (int i = 0; i < 4; ++i) entropy.ymode[i] = b.literal(8);
			if (b.literal(1)) for (int i = 0; i < 3; ++i) entropy.uvmode[i] = b.literal(8);
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 19; ++j)
				if (b.get_bool(T::MV_UPDATE_PROBS[i][j])) { int x = b.literal(7); entropy.mv[i][j] = x ? (x << 1) : 1; }
		}

		// translate persistent state into the flat hdr the shared helpers use
		hdr.filter_type = lf_type;
		hdr.loop_filter_level = lf_level;
		hdr.sharpness_level = lf_sharp;
		hdr.lf_adj_enable = lf_delta_enabled;
		for (int i = 0; i < 4; ++i) { hdr.ref_lf_deltas[i] = lf_ref_deltas[i]; hdr.mode_lf_deltas[i] = lf_mode_deltas[i]; }
		hdr.segmentation_enabled = seg_enabled ? 1 : 0;
		hdr.update_mb_seg_map = seg_update_map;
		for (int i = 0; i < 3; ++i) hdr.segment_tree_probs[i] = seg_tree[i];
		if (seg_enabled && seg_has_features) {
			hdr.has_segment_features = true;
			for (int i = 0; i < 4; ++i) { hdr.segment_quant[i] = seg_quant[i]; hdr.segment_lf[i] = seg_lf[i]; }
			hdr.segment_feature_mode = seg_abs;
		}
		return true;
	}

	int read_segment_id(bool_decoder& b) {
		if (b.get_bool(seg_tree[0])) return 2 + b.get_bool(seg_tree[2]);
		return b.get_bool(seg_tree[1]);
	}

	// -- neighbour MV helpers --
	static mv_t above_block_mv(const mb_info& self, const mb_info& above, int b) {
		if (b < 4) return above.ymode == SPLITMV ? above.smvs[b + 12] : above.mv;
		return self.smvs[b - 4];
	}
	static mv_t left_block_mv(const mb_info& self, const mb_info& left, int b) {
		if (!(b & 3)) return left.ymode == SPLITMV ? left.smvs[b + 3] : left.mv;
		return self.smvs[b - 1];
	}

	void find_near_mvs(const mb_info& self, const mb_info& left, const mb_info& above, const mb_info& aboveleft,
			const int* sign_bias, mv_t near[4], int cnt[4]) {
		for (int i = 0; i < 4; ++i) { near[i] = mv_t{}; cnt[i] = 0; }
		int mi = 0, ci = 0;
		auto biased = [&](const mb_info& mb) -> mv_t {
			if (sign_bias[mb.ref] ^ sign_bias[self.ref]) return { -mb.mv.x, -mb.mv.y };
			return mb.mv;
		};
		if (above.ref != CURRENT_FRAME) {
			if (above.mv != mv_t{}) { ++mi; near[mi] = biased(above); ++ci; }
			cnt[ci] += 2;
		}
		if (left.ref != CURRENT_FRAME) {
			if (left.mv != mv_t{}) { mv_t m = biased(left); if (m != near[mi]) { ++mi; near[mi] = m; ++ci; } cnt[ci] += 2; }
			else cnt[0] += 2;
		}
		if (aboveleft.ref != CURRENT_FRAME) {
			if (aboveleft.mv != mv_t{}) { mv_t m = biased(aboveleft); if (m != near[mi]) { ++mi; near[mi] = m; ++ci; } cnt[ci] += 1; }
			else cnt[0] += 1;
		}
		if (cnt[3] && near[mi] == near[1]) cnt[1] += 1;
		cnt[3] = ((above.ymode == SPLITMV) + (left.ymode == SPLITMV)) * 2 + (aboveleft.ymode == SPLITMV);
		if (cnt[2] > cnt[1]) { std::swap(cnt[1], cnt[2]); std::swap(near[1], near[2]); }
		if (cnt[1] >= cnt[0]) near[0] = near[1];
	}

	static mv_t clamp_mv(mv_t mv, const int* bounds) {
		int x = mv.x < bounds[0] ? bounds[0] : (mv.x > bounds[1] ? bounds[1] : mv.x);
		int y = mv.y < bounds[2] ? bounds[2] : (mv.y > bounds[3] ? bounds[3] : mv.y);
		return { x, y };
	}
	int submv_ref(bool_decoder& b, mv_t l, mv_t a) {
		bool lez = (l == mv_t{}), aez = (a == mv_t{}), lea = (l == a);
		int ctx = lea && lez ? 4 : (lea ? 3 : (aez ? 2 : (lez ? 1 : 0)));
		return treed(b, T::SUBMV_REF_TREE, T::SUBMV_REF_PROBS2[ctx]);
	}
	void decode_split_mv(bool_decoder& b, mb_info& self, const mb_info& left, const mb_info& above, mv_t best) {
		int pid = treed(b, T::SPLIT_MV_TREE, T::SPLIT_MV_PROBS);
		const T::I1& part = T::MV_PARTITIONS[pid];
		int mask = 0, j = 0;
		while (mask < 0xFFFF) {
			int k = 0; while (part[k] != j) ++k;
			mv_t lm = left_block_mv(self, left, k), am = above_block_mv(self, above, k);
			int sub = submv_ref(b, lm, am);
			mv_t mv;
			if (sub == LEFT4X4) mv = lm;
			else if (sub == ABOVE4X4) mv = am;
			else if (sub == ZERO4X4) mv = mv_t{};
			else { mv_t d = read_mv(b, entropy.mv); mv = { d.x + best.x, d.y + best.y }; }
			for (int kk = 0; kk < 16; ++kk) if (part[kk] == j) { self.smvs[kk] = mv; mask |= 1 << kk; }
			++j;
		}
	}
	void decode_mvs(bool_decoder& b, mb_info& self, const mb_info& left, const mb_info& above, const mb_info& aboveleft, const int* bounds) {
		self.ref = b.get_bool(hdr.prob_last) ? (2 + b.get_bool(hdr.prob_gf)) : 1;
		mv_t near[4]; int cnt[4];
		find_near_mvs(self, left, above, aboveleft, hdr.sign_bias, near, cnt);
		int probs[4]; for (int i = 0; i < 4; ++i) probs[i] = T::MV_COUNTS_TO_PROBS[cnt[i]][i];
		int mode = treed_read(b, T::MV_REF_TREE, [&](int k) { return probs[k]; });
		self.ymode = mode; self.uvmode = mode;
		if (mode == NEARESTMV) self.mv = clamp_mv(near[1], bounds);
		else if (mode == NEARMV) self.mv = clamp_mv(near[2], bounds);
		else if (mode == ZEROMV) self.mv = mv_t{};
		else if (mode == NEWMV) { mv_t best = clamp_mv(near[0], bounds); mv_t d = read_mv(b, entropy.mv); self.mv = { d.x + best.x, d.y + best.y }; }
		else { mv_t best = clamp_mv(near[0], bounds); decode_split_mv(b, self, left, above, best); self.mv = self.smvs[15]; }
	}
	void decode_intra_mb_mode(bool_decoder& b, mb_info& self) {
		self.ymode = treed_read(b, T::Y_MODE_TREE, [&](int k) { return entropy.ymode[k]; });
		if (self.ymode == B_PRED) for (int i = 0; i < 16; ++i) self.smodes[i] = treed(b, T::BMODE_TREE, T::DEFAULT_B_MODE_PROBS);
		else self.smodes.fill(implied_bmode(self.ymode));
		self.uvmode = treed_read(b, T::UV_MODE_TREE, [&](int k) { return entropy.uvmode[k]; });
		self.ref = CURRENT_FRAME; self.mv = mv_t{};
	}
	void decode_kf_mb_mode(bool_decoder& b, mb_info& self, const mb_info& left, const mb_info& above) {
		self.ymode = treed(b, T::KF_YMODE_TREE, T::KF_YMODE_PROB);
		if (self.ymode == B_PRED) {
			for (int i = 0; i < 16; ++i) {
				int a = (i < 4) ? above.smodes[i + 12] : self.smodes[i - 4];
				if (i < 4 && above.ymode != B_PRED) a = implied_bmode(above.ymode);
				int l = (i & 3) ? self.smodes[i - 1] : left.smodes[i + 3];
				if (!(i & 3) && left.ymode != B_PRED) l = implied_bmode(left.ymode);
				self.smodes[i] = treed(b, T::BMODE_TREE, T::KF_BMODE_PROB[a][l]);
			}
		} else self.smodes.fill(implied_bmode(self.ymode));
		self.uvmode = treed(b, T::UV_MODE_TREE, T::KF_UV_MODE_PROB);
		self.ref = CURRENT_FRAME; self.mv = mv_t{};
	}

	void reconstruct_mb(frame& fr, int mbx, int mby, mb_info& self, bool_decoder& rbd, const dq_t* dqs,
			std::vector<std::array<int, 9>>& above_ctx, std::array<int, 9>& left_ctx) {
		int ymode = self.ymode;
		bool has_y2 = ymode != B_PRED && ymode != SPLITMV;
		int coeffs[25][16];
		memset(coeffs, 0, sizeof coeffs);
		const dq_t& dq = dqs[self.seg];
		bool has_coeffs = false;

		if (self.skip) {
			for (int k = 0; k < 8; ++k) { above_ctx[mbx][k] = 0; left_ctx[k] = 0; }
			if (has_y2) { above_ctx[mbx][8] = 0; left_ctx[8] = 0; }
		} else {
			if (has_y2) {
				int c = above_ctx[mbx][8] + left_ctx[8];
				bool nz = decode_coeffs(rbd, entropy.coeff, 1, c, dq.y2, coeffs[24]);
				above_ctx[mbx][8] = left_ctx[8] = nz ? 1 : 0; has_coeffs |= nz;
			}
			int yplane = has_y2 ? 0 : 3;
			for (int bl = 0; bl < 24; ++bl) {
				int pl; const int* f;
				if (bl < 16) { pl = yplane; f = dq.y1; } else { pl = 2; f = dq.uv; }
				int c = above_ctx[mbx][ABOVE_CTX[bl]] + left_ctx[LEFT_CTX[bl]];
				bool nz = decode_coeffs(rbd, entropy.coeff, pl, c, f, coeffs[bl]);
				above_ctx[mbx][ABOVE_CTX[bl]] = nz ? 1 : 0;
				left_ctx[LEFT_CTX[bl]] = nz ? 1 : 0;
				has_coeffs |= nz;
			}
		}
		self.has_coeffs = has_coeffs;
		if (has_y2) { int dc[16]; iwht4x4(coeffs[24], dc); for (int bl = 0; bl < 16; ++bl) coeffs[bl][0] = dc[bl]; }

		int px = mbx * 16, py = mby * 16, cx = mbx * 8, cy = mby * 8;
		int tmp[16];

		if (ymode <= B_PRED) { // intra
			bool have_a = mby > 0, have_l = mbx > 0;
			if (ymode == B_PRED) {
				int top_right[4]; for (int i = 0; i < 4; ++i) top_right[i] = fr.y.at(px + 16 + i, py - 1);
				for (int bl = 0; bl < 16; ++bl) {
					int bx = px + (bl & 3) * 4, by = py + (bl >> 2) * 4;
					int ar[4];
					if ((bl & 3) == 3) for (int i = 0; i < 4; ++i) ar[i] = top_right[i];
					else for (int i = 0; i < 4; ++i) ar[i] = fr.y.at(bx + 4 + i, by - 1);
					predict_subblock(fr.y, bx, by, self.smodes[bl], ar);
					idct4x4(coeffs[bl], tmp); add_residue(fr.y, bx, by, tmp);
				}
			} else {
				predict_block(fr.y, px, py, 16, ymode, have_a, have_l);
				for (int bl = 0; bl < 16; ++bl) { idct4x4(coeffs[bl], tmp); add_residue(fr.y, px + (bl & 3) * 4, py + (bl >> 2) * 4, tmp); }
			}
			plane_t* uv[2] = { &fr.u, &fr.v };
			int base[2] = { 16, 20 };
			for (int pi = 0; pi < 2; ++pi) {
				predict_block(*uv[pi], cx, cy, 8, self.uvmode, have_a, have_l);
				for (int k = 0; k < 4; ++k) { idct4x4(coeffs[base[pi] + k], tmp); add_residue(*uv[pi], cx + (k & 1) * 4, cy + (k >> 1) * 4, tmp); }
			}
		} else { // inter
			const frame& ref = *refs[self.ref];
			mv_t cmvs[4];
			if (ymode != SPLITMV) {
				mv_t m = self.mv;
				for (int bl = 0; bl < 16; ++bl) { idct4x4(coeffs[bl], tmp); inter_block(fr.y, ref.y, px + (bl & 3) * 4, py + (bl >> 2) * 4, m.x, m.y, tmp); }
				mv_t cm = chroma_mv_from_luma(m.x, m.y);
				for (int k = 0; k < 4; ++k) cmvs[k] = cm;
			} else {
				for (int bl = 0; bl < 16; ++bl) { mv_t m = self.smvs[bl]; idct4x4(coeffs[bl], tmp); inter_block(fr.y, ref.y, px + (bl & 3) * 4, py + (bl >> 2) * 4, m.x, m.y, tmp); }
				int g[4] = { 0, 2, 8, 10 };
				for (int k = 0; k < 4; ++k) cmvs[k] = chroma_splitmv(self.smvs, g[k]);
			}
			const plane_t* rpl[2] = { &ref.u, &ref.v };
			plane_t* dpl[2] = { &fr.u, &fr.v };
			int base[2] = { 16, 20 };
			for (int pi = 0; pi < 2; ++pi) for (int k = 0; k < 4; ++k) {
				idct4x4(coeffs[base[pi] + k], tmp);
				inter_block(*dpl[pi], *rpl[pi], cx + (k & 1) * 4, cy + (k >> 1) * 4, cmvs[k].x, cmvs[k].y, tmp);
			}
		}
	}

	void update_refs(std::shared_ptr<frame> frp) {
		if (hdr.copy_arf == 1) refs[ALTREF_FRAME] = refs[LAST_FRAME];
		else if (hdr.copy_arf == 2) refs[ALTREF_FRAME] = refs[GOLDEN_FRAME];
		if (hdr.copy_gf == 1) refs[GOLDEN_FRAME] = refs[LAST_FRAME];
		else if (hdr.copy_gf == 2) refs[GOLDEN_FRAME] = refs[ALTREF_FRAME];
		if (hdr.refresh_gf) refs[GOLDEN_FRAME] = frp;
		if (hdr.refresh_arf) refs[ALTREF_FRAME] = frp;
		if (hdr.refresh_last) refs[LAST_FRAME] = frp;
	}
};

// ---- loop filter (normal, both key and inter) ----
static inline void loop_filter(frame& fr, const hdr_t& h) {
	if (h.filter_type != 0) return; // simple filter not used by SC:R corpus
	int sharp = h.sharpness_level;
	bool key = h.keyframe;
	int yst = fr.y.stride, cst = fr.u.stride;
	for (int mby = 0; mby < fr.mb_rows; ++mby) {
		for (int mbx = 0; mbx < fr.mb_cols; ++mbx) {
			const auto& info = fr.mb[(size_t)mby * fr.mb_cols + mbx];
			int ymode = info[0], seg_id = info[1], has_coeffs = info[2], ref_frame = info[3];
			int lvl = filter_level(h, seg_id, ymode, ref_frame);
			if (lvl == 0) continue;
			int interior = lvl;
			if (sharp) { interior >>= (sharp > 4 ? 2 : 1); interior = std::min(interior, 9 - sharp); }
			interior = std::max(1, interior);
			int hevt;
			if (key) hevt = (lvl >= 15 ? 1 : 0) + (lvl >= 40 ? 1 : 0);
			else if (lvl >= 40) hevt = 3;
			else if (lvl >= 20) hevt = 2;
			else if (lvl >= 15) hevt = 1;
			else hevt = 0;
			bool inner = has_coeffs || ymode == B_PRED || ymode == SPLITMV;
			int yx = mbx * 16, yy = mby * 16, cx = mbx * 8, cy = mby * 8;
			if (mbx) {
				filter_edge(fr.y.buf, fr.y.row_off(yy) + yx, 1, yst, 16, lvl + 2, interior, hevt, true);
				filter_edge(fr.u.buf, fr.u.row_off(cy) + cx, 1, cst, 8, lvl + 2, interior, hevt, true);
				filter_edge(fr.v.buf, fr.v.row_off(cy) + cx, 1, cst, 8, lvl + 2, interior, hevt, true);
			}
			if (inner) {
				for (int dx : {4, 8, 12}) filter_edge(fr.y.buf, fr.y.row_off(yy) + yx + dx, 1, yst, 16, lvl, interior, hevt, false);
				filter_edge(fr.u.buf, fr.u.row_off(cy) + cx + 4, 1, cst, 8, lvl, interior, hevt, false);
				filter_edge(fr.v.buf, fr.v.row_off(cy) + cx + 4, 1, cst, 8, lvl, interior, hevt, false);
			}
			if (mby) {
				filter_edge(fr.y.buf, fr.y.row_off(yy) + yx, yst, 1, 16, lvl + 2, interior, hevt, true);
				filter_edge(fr.u.buf, fr.u.row_off(cy) + cx, cst, 1, 8, lvl + 2, interior, hevt, true);
				filter_edge(fr.v.buf, fr.v.row_off(cy) + cx, cst, 1, 8, lvl + 2, interior, hevt, true);
			}
			if (inner) {
				for (int dy : {4, 8, 12}) filter_edge(fr.y.buf, fr.y.row_off(yy + dy) + yx, yst, 1, 16, lvl, interior, hevt, false);
				filter_edge(fr.u.buf, fr.u.row_off(cy + 4) + cx, cst, 1, 8, lvl, interior, hevt, false);
				filter_edge(fr.v.buf, fr.v.row_off(cy + 4) + cx, cst, 1, 8, lvl, interior, hevt, false);
			}
		}
	}
}

// Decode a single VP8 keyframe to pixels (back-compat wrapper for the stage
// c/d harness). Returns false if `data` is not a keyframe.
static inline bool decode_keyframe(const uint8_t* data, size_t n, keyframe_frame& fr, bool filtered = true) {
	frame_tag tag = parse_frame_tag(data, n);
	if (!tag.ok || !tag.keyframe) return false;
	vp8_decoder dec;
	auto f = dec.decode_frame(data, n, filtered);
	if (!f) return false;
	fr = *f;
	return true;
}

// Visible frame -> packed RGB (BT.601), byte-exact vs grp-toolkit Frame.rgb().
// This is the portrait pipeline's colour step after VP8 decode.
static inline std::vector<uint8_t> to_rgb(const frame& fr) {
	int w = fr.width, h = fr.height;
	std::vector<uint8_t> out((size_t)w * h * 3);
	yuv::to_rgb(w, h, 1, 1,
		[&](int x, int y) { return (int)fr.y.buf[fr.y.row_off(y) + x]; },
		[&](int cx, int cy) { return (int)fr.u.buf[fr.u.row_off(cy) + cx]; },
		[&](int cx, int cy) { return (int)fr.v.buf[fr.v.row_off(cy) + cx]; },
		out.data());
	return out;
}

// FNV-1a-64 over the visible Y, U, V (codec space, byte-exact vs libvpx).
static inline uint64_t plane_hash(const frame& fr) {
	uint64_t hsh = 1469598103934665603ULL;
	auto mix = [&](int v) { hsh ^= (uint8_t)v; hsh *= 1099511628211ULL; };
	int w = fr.width, hgt = fr.height, cw = (w + 1) >> 1, ch = (hgt + 1) >> 1;
	for (int j = 0; j < hgt; ++j) { int o = fr.y.row_off(j); for (int i = 0; i < w; ++i) mix(fr.y.buf[o + i]); }
	for (int j = 0; j < ch; ++j) { int o = fr.u.row_off(j); for (int i = 0; i < cw; ++i) mix(fr.u.buf[o + i]); }
	for (int j = 0; j < ch; ++j) { int o = fr.v.row_off(j); for (int i = 0; i < cw; ++i) mix(fr.v.buf[o + i]); }
	return hsh;
}

} // namespace vp8
} // namespace bwgame

#endif
