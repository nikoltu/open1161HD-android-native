// vp9_pixels.h -- VP9 key-frame pixel reconstruction (Faze 3.3a of open1161HD).
//
// Ported from the grp-toolkit oracle tools/vp9_pixels.py (VP9 spec §8.5-§8.7),
// transcribed from the spec prose (no reference to libvpx). Extends the entropy
// decoder (ui/vp9_entropy.h) to KEEP coefficients and produce reconstructed
// planes: dequant (§8.6.1) -> inverse DCT/ADST/WHT 2D (§8.7) -> intra prediction
// (§8.5.1, 10 modes) -> reconstruction (§8.6.2) -> in-loop deblocking filter
// (§8.8, key-frame form: narrow/wide, filter mask, per-edge size). Validated
// byte-exact against the oracle (which is byte-exact vs libvpx) across all key
// frames, filtered and unfiltered. Pure presentation; no determinism impact.
//
// Transform intermediates use int64: dequantised coefficients * cos64 (2^14)
// overflow int32, so 64-bit is required to match the arbitrary-precision oracle.
#ifndef BWGAME_UI_VP9_PIXELS_H
#define BWGAME_UI_VP9_PIXELS_H

#include "vp9_entropy.h"

namespace bwgame {
namespace vp9 {

enum { V_PRED = 1, H_PRED = 2, D45_PRED = 3, D135_PRED = 4, D117_PRED = 5,
       D153_PRED = 6, D207_PRED = 7, D63_PRED = 8 };

namespace px {

static const int SINPI_1_9 = 5283, SINPI_2_9 = 9929, SINPI_3_9 = 13377, SINPI_4_9 = 15212;
static const int COS64_[33] = {
	16384, 16364, 16305, 16207, 16069, 15893, 15679, 15426,
	15137, 14811, 14449, 14053, 13623, 13160, 12665, 12140,
	11585, 11003, 10394, 9760, 9102, 8423, 7723, 7005,
	6270, 5520, 4756, 3981, 3196, 2404, 1606, 804, 0 };

typedef long long i64;

static inline i64 round2(i64 x, int n) { return n == 0 ? x : (x + ((i64)1 << (n - 1))) >> n; }
static inline int brev(int numbits, int x) {
	int t = 0;
	for (int i = 0; i < numbits; ++i) t |= ((x >> i) & 1) << (numbits - 1 - i);
	return t;
}
static inline i64 cos64(int angle) {
	int a = angle & 127;
	if (a <= 32) return COS64_[a];
	if (a <= 64) return -COS64_[64 - a];
	if (a <= 96) return -COS64_[a - 64];
	return COS64_[128 - a];
}
static inline i64 sin64(int angle) { return cos64(angle - 32); }

static inline void B(i64* Tr, int a, int b, int angle, int flip) {
	i64 ca = cos64(angle), sa = sin64(angle);
	i64 x = Tr[a] * ca - Tr[b] * sa;
	i64 y = Tr[a] * sa + Tr[b] * ca;
	Tr[a] = round2(x, 14); Tr[b] = round2(y, 14);
	if (flip) { i64 t = Tr[a]; Tr[a] = Tr[b]; Tr[b] = t; }
}
static inline void H(i64* Tr, int a, int b, int flip) {
	if (flip) { int t = a; a = b; b = t; }
	i64 x = Tr[a], y = Tr[b];
	Tr[a] = x + y; Tr[b] = x - y;
}
static inline void SB(i64* S, i64* Tr, int a, int b, int angle, int flip) {
	i64 ca = cos64(angle), sa = sin64(angle);
	S[a] = Tr[a] * ca - Tr[b] * sa;
	S[b] = Tr[a] * sa + Tr[b] * ca;
	if (flip) { i64 t = S[a]; S[a] = S[b]; S[b] = t; }
}
static inline void SH(i64* S, i64* Tr, int a, int b) {
	Tr[a] = round2(S[a] + S[b], 14);
	Tr[b] = round2(S[a] - S[b], 14);
}

static inline void idct_permute(i64* Tr, int n) {
	int sz = 1 << n;
	i64 c[32];
	for (int i = 0; i < sz; ++i) c[i] = Tr[i];
	for (int i = 0; i < sz; ++i) Tr[i] = c[brev(n, i)];
}
static void idct(i64* Tr, int n) {
	int n0 = 1 << n, n1 = 1 << (n - 1), n2 = 1 << (n - 2), n3 = (n >= 3) ? (1 << (n - 3)) : 0;
	if (n == 2) B(Tr, 0, 1, 16, 1); else idct(Tr, n - 1);
	for (int i = 0; i < n2; ++i) B(Tr, n1 + i, n0 - 1 - i, 32 - brev(5, n1 + i), 0);
	if (n >= 3)
		for (int i = 0; i < n3; ++i) for (int j = 0; j < 2; ++j)
			H(Tr, n1 + 4 * i + 2 * j, n1 + 1 + 4 * i + 2 * j, j);
	if (n == 5) {
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j)
			B(Tr, n0 - n + 3 - n2 * j - 4 * i, n1 + n - 4 + n2 * j + 4 * i, 28 - 16 * i + 56 * j, 1);
		for (int i = 0; i < 2; ++i) for (int j = 0; j < 4; ++j)
			H(Tr, n1 + n3 * j + i, n1 + n2 - 5 + n3 * j - i, j & 1);
	}
	if (n >= 4) {
		for (int i = 0; i < (n == 5 ? 2 : 1); ++i) for (int j = 0; j < 2; ++j)
			B(Tr, n0 - n + 2 - i - n2 * j, n1 + n - 3 + i + n2 * j, 24 + 48 * j, 1);
		for (int i = 0; i < 2 * n - 6; ++i) for (int j = 0; j < 2; ++j)
			H(Tr, n1 + n2 * j + i, n1 + n2 - 1 + n2 * j - i, j & 1);
	}
	if (n >= 3)
		for (int i = 0; i < n3; ++i) B(Tr, n0 - n3 - 1 - i, n1 + n3 + i, 16, 1);
	for (int i = 0; i < n1; ++i) H(Tr, i, n0 - 1 - i, 0);
}

static inline void adst_in_perm(i64* Tr, int n) {
	int n0 = 1 << n, n1 = 1 << (n - 1);
	i64 c[32];
	for (int i = 0; i < n0; ++i) c[i] = Tr[i];
	for (int i = 0; i < n1; ++i) { Tr[2 * i] = c[n0 - 1 - 2 * i]; Tr[2 * i + 1] = c[2 * i]; }
}
static inline void adst_out_perm(i64* Tr, int n) {
	int n0 = 1 << n;
	i64 c[32];
	for (int i = 0; i < n0; ++i) c[i] = Tr[i];
	if (n == 4) {
		for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b)
			for (int cc = 0; cc < 2; ++cc) for (int d = 0; d < 2; ++d)
				Tr[8 * a + 4 * b + 2 * cc + d] = c[8 * (d ^ cc) + 4 * (cc ^ b) + 2 * (b ^ a) + a];
	} else {
		for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) for (int cc = 0; cc < 2; ++cc)
			Tr[4 * a + 2 * b + cc] = c[4 * (cc ^ b) + 2 * (b ^ a) + a];
	}
}
static inline void iadst4(i64* Tr) {
	i64 s0 = (i64)SINPI_1_9 * Tr[0], s1 = (i64)SINPI_2_9 * Tr[0], s2 = (i64)SINPI_3_9 * Tr[1];
	i64 s3 = (i64)SINPI_4_9 * Tr[2], s4 = (i64)SINPI_1_9 * Tr[2], s5 = (i64)SINPI_2_9 * Tr[3];
	i64 s6 = (i64)SINPI_4_9 * Tr[3];
	i64 v = Tr[0] - Tr[2] + Tr[3];
	i64 s7 = (i64)SINPI_3_9 * v;
	i64 x0 = s0 + s3 + s5, x1 = s1 - s4 - s6, x2 = s7, x3 = s2;
	i64 y0 = x0 + x3, y1 = x1 + x3, y2 = x2, y3 = x0 + x1 - x3;
	Tr[0] = round2(y0, 14); Tr[1] = round2(y1, 14); Tr[2] = round2(y2, 14); Tr[3] = round2(y3, 14);
}
static inline void iadst8(i64* Tr) {
	i64 S[8] = { 0 };
	adst_in_perm(Tr, 3);
	for (int i = 0; i < 4; ++i) SB(S, Tr, 2 * i, 1 + 2 * i, 30 - 8 * i, 1);
	for (int i = 0; i < 4; ++i) SH(S, Tr, i, 4 + i);
	for (int i = 0; i < 2; ++i) SB(S, Tr, 4 + 3 * i, 5 + i, 24 - 16 * i, 1);
	for (int i = 0; i < 2; ++i) SH(S, Tr, 4 + i, 6 + i);
	for (int i = 0; i < 2; ++i) H(Tr, i, 2 + i, 0);
	for (int i = 0; i < 2; ++i) B(Tr, 2 + 4 * i, 3 + 4 * i, 16, 1);
	adst_out_perm(Tr, 3);
	for (int i = 0; i < 4; ++i) Tr[1 + 2 * i] = -Tr[1 + 2 * i];
}
static inline void iadst16(i64* Tr) {
	i64 S[16] = { 0 };
	adst_in_perm(Tr, 4);
	for (int i = 0; i < 8; ++i) SB(S, Tr, 2 * i, 1 + 2 * i, 31 - 4 * i, 1);
	for (int i = 0; i < 8; ++i) SH(S, Tr, i, 8 + i);
	for (int i = 0; i < 4; ++i) SB(S, Tr, 8 + 2 * i, 9 + 2 * i, 28 - 16 * i, 1);
	for (int i = 0; i < 4; ++i) SH(S, Tr, 8 + i, 12 + i);
	for (int i = 0; i < 4; ++i) H(Tr, i, 4 + i, 0);
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) SB(S, Tr, 4 + 8 * i + 3 * j, 5 + 8 * i + j, 24 - 16 * j, 1);
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) SH(S, Tr, 4 + 8 * j + i, 6 + 8 * j + i);
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) H(Tr, 8 * j + i, 2 + 8 * j + i, 0);
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) B(Tr, 2 + 4 * j + 8 * i, 3 + 4 * j + 8 * i, 48 + 64 * (i ^ j), 0);
	adst_out_perm(Tr, 4);
	for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) Tr[1 + 12 * j + 2 * i] = -Tr[1 + 12 * j + 2 * i];
}
static inline void iadst(i64* Tr, int n) { if (n == 2) iadst4(Tr); else if (n == 3) iadst8(Tr); else iadst16(Tr); }
static inline void iwht(i64* Tr, int shift) {
	i64 a = Tr[0] >> shift, c = Tr[1] >> shift, d = Tr[2] >> shift, b = Tr[3] >> shift;
	a += c; d -= b;
	i64 e = (a - d) >> 1;
	b = e - b; c = e - c; a -= b; d += c;
	Tr[0] = a; Tr[1] = b; Tr[2] = c; Tr[3] = d;
}

// spec 8.7.2 -- separable 2D inverse transform, in place on n0 x n0 Dequant.
static inline void inverse_transform_2d(std::vector<std::vector<i64>>& Dequant, int n, int tx_type, bool lossless) {
	int n0 = 1 << n;
	for (int i = 0; i < n0; ++i) {
		i64* Tr = Dequant[i].data();
		if (lossless) iwht(Tr, 2);
		else if (tx_type == DCT_DCT || tx_type == ADST_DCT) { idct_permute(Tr, n); idct(Tr, n); }
		else iadst(Tr, n);
	}
	int sh = std::min(6, n + 2);
	i64 Tr[32];
	for (int j = 0; j < n0; ++j) {
		for (int i = 0; i < n0; ++i) Tr[i] = Dequant[i][j];
		if (lossless) iwht(Tr, 0);
		else if (tx_type == DCT_DCT || tx_type == DCT_ADST) { idct_permute(Tr, n); idct(Tr, n); }
		else iadst(Tr, n);
		if (lossless) for (int i = 0; i < n0; ++i) Dequant[i][j] = Tr[i];
		else for (int i = 0; i < n0; ++i) Dequant[i][j] = round2(Tr[i], sh);
	}
}
static inline i64 trunc_div(i64 a, i64 b) {
	if (b == 1) return a;
	i64 q = (a < 0 ? -a : a) / b;
	return a >= 0 ? q : -q;
}

// ---- loop filter (spec 8.8) ----
static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static inline long clip3l(long lo, long hi, long v) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int iabs(int v) { return v < 0 ? -v : v; }

// B5: flat strided uint16_t plane. operator[] returns a row pointer so every
// existing frame[y][x] / rrow[x] access site keeps working. uint16_t is exact
// for all VP9 sample values (<=12-bit; portraits/cinematics are 8-bit); the
// contiguous layout gives the loop filter and vertical MC pass cache locality.
struct plane_t {
	std::vector<uint16_t> buf;
	int rows_ = 0, cols_ = 0, stride_ = 0;
	void alloc(int rows, int cols) { rows_ = rows; cols_ = cols; stride_ = cols; buf.assign((size_t)rows * cols, (uint16_t)0); }
	uint16_t* operator[](int y) { return buf.data() + (size_t)y * stride_; }
	const uint16_t* operator[](int y) const { return buf.data() + (size_t)y * stride_; }
	int rows() const { return rows_; }
	int cols() const { return cols_; }
	bool empty() const { return buf.empty(); }
};

static inline void lf_narrow(plane_t& frame, int x, int y, int dx, int dy, int hev, int bd) {
	int half = 0x80 << (bd - 8);
	int lim = 1 << (bd - 1);
	auto cl = [&](int v) { return v < -lim ? -lim : (v > lim - 1 ? lim - 1 : v); };
	int q0 = frame[y][x], q1 = frame[y + dy][x + dx];
	int p0 = frame[y - dy][x - dx], p1 = frame[y - dy * 2][x - dx * 2];
	int ps1 = p1 - half, ps0 = p0 - half, qs0 = q0 - half, qs1 = q1 - half;
	int filt = hev ? cl(ps1 - qs1) : 0;
	filt = cl(filt + 3 * (qs0 - ps0));
	int filter1 = cl(filt + 4) >> 3;
	int filter2 = cl(filt + 3) >> 3;
	frame[y][x] = cl(qs0 - filter1) + half;
	frame[y - dy][x - dx] = cl(ps0 + filter2) + half;
	if (!hev) {
		int f = (int)round2(filter1, 1);
		frame[y + dy][x + dx] = cl(qs1 - f) + half;
		frame[y - dy * 2][x - dx * 2] = cl(ps1 + f) + half;
	}
}

static inline void lf_wide(plane_t& frame, int x, int y, int dx, int dy, int log2size) {
	int n = (1 << (log2size - 1)) - 1;
	std::vector<int> F(2 * n, 0); // index i+n for i in [-n, n-1]
	for (int i = -n; i < n; ++i) {
		int t = frame[y + i * dy][x + i * dx];
		for (int j = -n; j <= n; ++j) {
			int pp = clip3(-(n + 1), n, i + j);
			t += frame[y + pp * dy][x + pp * dx];
		}
		F[i + n] = (int)round2(t, log2size);
	}
	for (int i = -n; i < n; ++i) frame[y + i * dy][x + i * dx] = F[i + n];
}

static inline void lf_sample(plane_t& frame, int x, int y, int limit, int blimit,
                             int thresh, int dx, int dy, int filter_size, int bd) {
	// TX_8X8=1, TX_16X16=2 (matches vp9_entropy enums)
	auto S = [&](int k) { return frame[y + dy * k][x + dx * k]; };
	int q[8], p[8], nq = 4;
	for (int k = 0; k < 4; ++k) { q[k] = S(k); p[k] = S(-1 - k); }
	if (filter_size >= 2) { for (int k = 4; k < 8; ++k) { q[k] = S(k); p[k] = S(-1 - k); } nq = 8; }
	int tb = thresh << (bd - 8);
	int hev = (iabs(p[1] - p[0]) > tb || iabs(q[1] - q[0]) > tb) ? 1 : 0;
	int lb = limit << (bd - 8), bb = blimit << (bd - 8);
	bool m = iabs(p[3] - p[2]) > lb || iabs(p[2] - p[1]) > lb || iabs(p[1] - p[0]) > lb
		|| iabs(q[1] - q[0]) > lb || iabs(q[2] - q[1]) > lb || iabs(q[3] - q[2]) > lb
		|| iabs(p[0] - q[0]) * 2 + iabs(p[1] - q[1]) / 2 > bb;
	if (m) return;
	int thr = 1 << (bd - 8);
	int flat = 0;
	if (filter_size >= 1)
		flat = (iabs(p[1] - p[0]) > thr || iabs(q[1] - q[0]) > thr || iabs(p[2] - p[0]) > thr
			|| iabs(q[2] - q[0]) > thr || iabs(p[3] - p[0]) > thr || iabs(q[3] - q[0]) > thr) ? 0 : 1;
	int flat2 = 0;
	if (filter_size >= 2) {
		flat2 = 1;
		for (int k = 4; k < 8; ++k) if (iabs(p[k] - p[0]) > thr || iabs(q[k] - q[0]) > thr) { flat2 = 0; break; }
	}
	(void)nq;
	if (filter_size == 0 || flat == 0) lf_narrow(frame, x, y, dx, dy, hev, bd);
	else if (filter_size == 1 || flat2 == 0) lf_wide(frame, x, y, dx, dy, 3);
	else lf_wide(frame, x, y, dx, dy, 4);
}

} // namespace px

struct pixel_decoder : keyframe_decoder {
	int bit_depth;
	bool loss;
	int dc_quant[3], ac_quant[3];
	px::plane_t cur[3];   // reconstructed planes (B5: flat strided uint16_t)
	std::vector<int> Tokens;
	std::vector<int> token_cache;   // B7: reused per-block coeff-context scratch (was allocated per block)
	std::vector<std::vector<px::i64>> Dequant;   // B1: reused per-block dequant matrix (grow-only; fully overwritten each use)
	int TxType = 0;

	pixel_decoder(const header_t& hdr, probs_t& pr) : keyframe_decoder(hdr, pr) {
		bit_depth = hdr.bit_depth;
		loss = hdr.lossless;
		int base = hdr.base_q_idx, bd_idx = (bit_depth - 8) >> 1;
		auto dcq = [&](int b) { return T::DC_QLOOKUP[bd_idx][std::max(0, std::min(255, b))]; };
		auto acq = [&](int b) { return T::AC_QLOOKUP[bd_idx][std::max(0, std::min(255, b))]; };
		dc_quant[0] = dcq(base + hdr.delta_q_y_dc);
		dc_quant[1] = dc_quant[2] = dcq(base + hdr.delta_q_uv_dc);
		ac_quant[0] = acq(base);
		ac_quant[1] = ac_quant[2] = acq(base + hdr.delta_q_uv_ac);
		int aw = ((mi_cols + 7) & ~7) * 8, ah = ((mi_rows + 7) & ~7) * 8;
		for (int pl = 0; pl < 3; ++pl) {
			int sx = pl > 0 ? sub_x : 0, sy = pl > 0 ? sub_y : 0;
			cur[pl].alloc(ah >> sy, aw >> sx);
		}
	}

	void residual(bool_decoder& bd) override {
		int bsize = (MiSize < BLOCK_8X8) ? BLOCK_8X8 : MiSize;
		for (int plane = 0; plane < 3; ++plane) {
			int txsz = (plane > 0) ? uv_tx_size() : cur_tx_size;
			int step = 1 << txsz;
			int plane_sz = plane_block_size(bsize, plane);
			int n4w = T::NUM_4X4_W[plane_sz], n4h = T::NUM_4X4_H[plane_sz];
			int subx = plane > 0 ? sub_x : 0, suby = plane > 0 ? sub_y : 0;
			int base_x = (MiCol * 8) >> subx, base_y = (MiRow * 8) >> suby;
			int maxx = (mi_cols * 8) >> subx, maxy = (mi_rows * 8) >> suby;
			int block_idx = 0;
			for (int y = 0; y < n4h; y += step) {
				for (int x = 0; x < n4w; x += step) {
					int start_x = base_x + 4 * x, start_y = base_y + 4 * y;
					int nonzero = 0;
					if (start_x < maxx && start_y < maxy) {
						predict_intra(plane, start_x, start_y,
						              AvailL || x > 0, AvailU || y > 0,
						              (x + step) < n4w, txsz, block_idx);
						if (!cur_skip) {
							nonzero = tokens(bd, plane, start_x, start_y, txsz, block_idx);
							reconstruct(plane, start_x, start_y, txsz);
						}
					}
					for (int i = 0; i < step; ++i) {
						above_nz[plane][(start_x >> 2) + i] = nonzero;
						left_nz[plane][(start_y >> 2) + i] = nonzero;
					}
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
		const T::I3& cbook = p.coef[txsz][pl_type][0];
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
				for (int i = 0; i < numpts; ++i) {
					if (x4 + i < maxX) above |= above_nz[plane][x4 + i];
					if (y4 + i < maxY) left |= left_nz[plane][y4 + i];
				}
				ctx = above + left;
			} else {
				int nb0, nb1; neighbors(pos, n, tt, nb0, nb1);
				ctx = (1 + token_cache[nb0] + token_cache[nb1]) >> 1;
			}
			const T::I1& probs = cbook[band][ctx];
			if (check_eob) { if (!bd.read_bool(probs[0])) break; }
			int token = read_tree(bd, T::TOKEN_TREE,
				[&](int nd) { return pareto(nd, probs[std::min(2, 1 + nd)]); });
			token_cache[pos] = T::ENERGY_CLASS[token];
			if (token == ZERO_TOKEN) { Tokens[pos] = 0; check_eob = 0; }
			else {
				int coef = read_coef(bd, token);
				int sign = bd.read_literal(1);
				Tokens[pos] = sign ? -coef : coef;
				check_eob = 1;
			}
			++c;
		}
		return c > 0 ? 1 : 0;
	}

	void reconstruct(int plane, int x, int y, int txsz) {
		int dq_denom = (txsz == TX_32X32) ? 2 : 1;
		int n = 2 + txsz, n0 = 1 << n;
		int acq = ac_quant[plane], dcq = dc_quant[plane];
		// B1: reuse the member Dequant instead of a per-block heap alloc (up to 33
		// allocs for a 32x32). Grow-only: every entry is overwritten below, so the
		// old zero-init was dead and no clearing is needed; extra rows/cols from a
		// prior larger block are simply not touched (loops run [0,n0)).
		if ((int)Dequant.size() < n0) Dequant.resize(n0);
		for (int i = 0; i < n0; ++i) if ((int)Dequant[i].size() < n0) Dequant[i].resize(n0);
		for (int i = 0; i < n0; ++i) {
			int base = i * n0;
			for (int j = 0; j < n0; ++j)
				Dequant[i][j] = px::trunc_div((px::i64)Tokens[base + j] * acq, dq_denom);
		}
		Dequant[0][0] = px::trunc_div((px::i64)Tokens[0] * dcq, dq_denom);
		px::inverse_transform_2d(Dequant, n, TxType, loss);
		int maxv = (1 << bit_depth) - 1;
		for (int i = 0; i < n0; ++i) {
			uint16_t* frow = cur[plane][y + i];
			for (int j = 0; j < n0; ++j) {
				px::i64 v = frow[x + j] + Dequant[i][j];
				frow[x + j] = (uint16_t)(v < 0 ? 0 : (v > maxv ? maxv : (int)v));
			}
		}
	}

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
		if (have_above && not_on_right && txsz == 0) {
			const uint16_t* ar = frame[y - 1];
			for (int i = size; i < 2 * size; ++i) A[i + 1] = ar[std::min(maxX, x + i)];
		} else for (int i = size; i < 2 * size; ++i) A[i + 1] = A[size];
		if (have_above && have_left) A[0] = frame[y - 1][std::min(maxX, x - 1)];
		else if (have_above) A[0] = half + 1;
		else A[0] = half - 1;
		if (have_left) for (int i = 0; i < size; ++i) L[i] = frame[std::min(maxY, y + i)][x - 1];
		else for (int i = 0; i < size; ++i) L[i] = half + 1;
		auto ar = [&](int k) { return A[k + 1]; };

		int pred[32][32] = {}; // VP9 max tx size 32x32
		if (mode == V_PRED) {
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) pred[i][j] = ar(j);
		} else if (mode == H_PRED) {
			for (int i = 0; i < size; ++i) { int Li = L[i]; for (int j = 0; j < size; ++j) pred[i][j] = Li; }
		} else if (mode == D207_PRED) {
			for (int j = 0; j < size; ++j) pred[size - 1][j] = L[size - 1];
			for (int i = 0; i < size - 1; ++i) pred[i][0] = px::round2(L[i] + L[i + 1], 1);
			for (int i = 0; i < size - 2; ++i) pred[i][1] = px::round2(L[i] + 2 * L[i + 1] + L[i + 2], 2);
			if (size >= 2) pred[size - 2][1] = px::round2(L[size - 2] + 3 * L[size - 1], 2);
			for (int i = size - 2; i >= 0; --i) for (int j = 2; j < size; ++j) pred[i][j] = pred[i + 1][j - 2];
		} else if (mode == D45_PRED) {
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j)
				pred[i][j] = (i + j + 2 < size * 2) ? px::round2(ar(i + j) + 2 * ar(i + j + 1) + ar(i + j + 2), 2) : ar(2 * size - 1);
		} else if (mode == D63_PRED) {
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) {
				int k = i / 2 + j;
				pred[i][j] = (i & 1) ? px::round2(ar(k) + 2 * ar(k + 1) + ar(k + 2), 2) : px::round2(ar(k) + ar(k + 1), 1);
			}
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
		} else { // DC_PRED
			int avg;
			if (have_left && have_above) { int s = 0; for (int k = 0; k < size; ++k) s += L[k] + ar(k); avg = (s + size) >> (log2size + 1); }
			else if (have_left) { int s = 0; for (int k = 0; k < size; ++k) s += L[k]; avg = (s + (1 << (log2size - 1))) >> log2size; }
			else if (have_above) { int s = 0; for (int k = 0; k < size; ++k) s += ar(k); avg = (s + (1 << (log2size - 1))) >> log2size; }
			else avg = 1 << (bd - 1);
			for (int i = 0; i < size; ++i) for (int j = 0; j < size; ++j) pred[i][j] = avg;
		}
		for (int i = 0; i < size; ++i) {
			uint16_t* frow = frame[y + i];
			for (int j = 0; j < size; ++j) frow[x + j] = (uint16_t)pred[i][j];
		}
	}

	// -- loop filter (spec 8.8), key-frame form --
	enum { BLOCK_16X16 = 6 };
	int uv_tx_size_of(int mi_size, int tx_size) {
		if (mi_size < BLOCK_8X8) return TX_4X4;
		return std::min(tx_size, T::MAX_TXSIZE[T::SS_SIZE_LOOKUP[mi_size][sub_x][sub_y]]);
	}
	int filter_size(int txsz, int is32, int pass_, int x, int y, int subx, int suby) {
		int base = (txsz == TX_4X4 && is32) ? TX_8X8 : std::min((int)TX_16X16, txsz);
		if (pass_ == 0 && subx == 1 && base == TX_16X16 && (x >> 3) == mi_cols - 1) return TX_8X8;
		if (pass_ == 1 && suby == 1 && base == TX_16X16 && (y >> 3) == mi_rows - 1) return TX_8X8;
		return base;
	}
	void apply_loop_filter() {
		int level = h.loop_filter_level;
		if (level == 0) return;
		int sharp = h.loop_filter_sharpness;
		int n_shift = level >> 5;
		int lvl = level; // key frame: segment 0, INTRA ref, modeType 0
		if (h.loop_filter_delta_enabled)
			lvl = px::clip3(0, 63, level + (h.loop_filter_ref_deltas[0] << n_shift));
		if (lvl <= 0) return;
		int shift = sharp > 4 ? 2 : (sharp > 0 ? 1 : 0);
		int limit = sharp > 0 ? px::clip3(1, 9 - sharp, lvl >> shift) : std::max(1, lvl >> shift);
		int blimit = 2 * (lvl + 2) + limit;
		int thresh = lvl >> 4;
		for (int row = 0; row < mi_rows; row += 8)
			for (int col = 0; col < mi_cols; col += 8)
				for (int plane = 0; plane < 3; ++plane) {
					sb_loop_filter(plane, 0, row, col, limit, blimit, thresh, bit_depth);
					sb_loop_filter(plane, 1, row, col, limit, blimit, thresh, bit_depth);
				}
	}
	void sb_loop_filter(int plane, int pass_, int row, int col, int limit, int blimit, int thresh, int bd) {
		px::plane_t& frame = cur[plane];
		int subx = plane == 0 ? 0 : sub_x, suby = plane == 0 ? 0 : sub_y;
		int dx, dy, sub, edge_len;
		if (pass_ == 0) { dx = 1; dy = 0; sub = subx; edge_len = 64 >> suby; }
		else { dx = 0; dy = 1; sub = suby; edge_len = 64 >> subx; }
		for (int edge = 0; edge < (16 >> sub); ++edge) {
			for (int i = 0; i < edge_len; ++i) {
				int x, y;
				if (pass_ == 0) { x = col * 8 + edge * (4 << subx); y = row * 8 + (i << suby); }
				else { x = col * 8 + (i << subx); y = row * 8 + edge * (4 << suby); }
				if (x >= 8 * mi_cols || y >= 8 * mi_rows) continue;
				if (pass_ == 0 && x == 0) continue;
				if (pass_ == 1 && y == 0) continue;
				int loop_col = ((x >> 3) >> subx) << subx;
				int loop_row = ((y >> 3) >> suby) << suby;
				int mi_size = mi_sizes[loop_row][loop_col];
				int tx_size = tx_sizes[loop_row][loop_col];
				int txsz = plane > 0 ? uv_tx_size_of(mi_size, tx_size) : tx_size;
				int sb_size = sub == 0 ? mi_size : std::max((int)BLOCK_16X16, mi_size);
				int is_block_edge;
				if (pass_ == 0) is_block_edge = (x % (8 * T::NUM_8X8_W[sb_size]) == 0) ? 1 : 0;
				else is_block_edge = (y % (8 * T::NUM_8X8_H[sb_size]) == 0) ? 1 : 0;
				int is_tx_edge;
				if (pass_ == 1 && subx == 1 && (mi_cols & 1) && (edge & 1) && (x + 8) >= mi_cols * 8)
					is_tx_edge = 0;
				else if (edge % (1 << txsz) == 0) is_tx_edge = 1;
				else is_tx_edge = 0;
				int is32 = (edge % 8 == 0) ? 1 : 0;
				if (!(is_block_edge || is_tx_edge)) continue;
				int fs = filter_size(txsz, is32, pass_, x, y, subx, suby);
				px::lf_sample(frame, x >> subx, y >> suby, limit, blimit, thresh, dx, dy, fs, bd);
			}
		}
	}
};

// FNV-1a 64 over the three reconstructed planes (two bytes per sample).
static inline uint64_t plane_hash(const pixel_decoder& dec) {
	uint64_t h = 1469598103934665603ULL;
	auto mix = [&](uint8_t b) { h ^= b; h *= 1099511628211ULL; };
	for (int pl = 0; pl < 3; ++pl) {
		const px::plane_t& p = dec.cur[pl];
		for (int j = 0; j < p.rows(); ++j) {
			const uint16_t* row = p[j];
			for (int i = 0; i < p.cols(); ++i) { int v = row[i]; mix((uint8_t)(v & 0xFF)); mix((uint8_t)((v >> 8) & 0xFF)); }
		}
	}
	return h;
}

struct kf_pixels_result {
	bool ok = false;
	int loop_filter_level = 0;
	int width = 0, height = 0;
	uint64_t hash = 0;
};

// Decode a VP9 key frame to reconstructed (pre-loop-filter) planes and hash
// them. Mirrors vp9_pixels.py:decode_keyframe (loop filter is a no-op when
// loop_filter_level==0, which is the frame set this stage validates).
static inline kf_pixels_result decode_keyframe_pixels(const uint8_t* data, size_t datalen,
                                                      size_t lo, size_t hi) {
	kf_pixels_result res;
	header_parser hp;
	header_t h = hp.parse(data + lo, hi - lo);
	if (!h.ok || h.show_existing >= 0 || !h.keyframe) return res;
	probs_t probs;
	probs.parse(h, data, datalen, (int)(lo + h.uncompressed_bytes));
	if (probs.pad != 0) return res;

	pixel_decoder dec(h, probs);
	size_t tile_start = lo + h.uncompressed_bytes + h.header_size_in_bytes;
	int tile_cols = 1 << h.tile_cols_log2, tile_rows = 1 << h.tile_rows_log2;
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
			pos += size;
		}
	dec.apply_loop_filter();   // spec 8.8; no-op when loop_filter_level == 0
	res.ok = true;
	res.loop_filter_level = h.loop_filter_level;
	res.width = h.width; res.height = h.height;
	res.hash = plane_hash(dec);
	return res;
}

} // namespace vp9
} // namespace bwgame

#endif
