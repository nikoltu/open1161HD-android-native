#ifndef BWGAME_PARALLAX_H
#define BWGAME_PARALLAX_H

// Space-platform star parallax. SD: 1.16.1 parallax\star.spk (u16 layers, u16
// counts, 8-byte {x,y,bitmap offset} stars, {u16 w,u16 h,pixels} bitmaps),
// drawn into the indexed frame where the tile pixel is 0. HD: SC:R star.spk
// (u32 layers, u32 data offset, {u32 count,u16 w,u16 h} per layer, 12-byte
// {x,y,u,v,w,h} stars) + star.dds.grp (one atlas per layer).

#include "ddsgrp.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bwgame {
namespace parallax {

// Layer scroll per 256 px of camera movement (PyMS values, not measured against SC:R).
static const int speed[5] = {16, 21, 26, 31, 36};
static const int field_w = 648, field_h = 488;

static inline int wrap(int v, int m) { v %= m; return v < 0 ? v + m : v; }

struct sd_star { int x, y, w, h; std::vector<uint8_t> px; };
struct sd_t { bool ok = false; std::vector<std::vector<sd_star>> layers; };

static inline sd_t parse_sd(const uint8_t* d, size_t n) {
	sd_t out;
	if (n < 2) return out;
	auto r16 = [&](size_t o) { return o + 2 <= n ? (int)(d[o] | d[o + 1] << 8) : -1; };
	int layers = r16(0);
	if (layers < 1 || layers > 5) return out;
	size_t pos = 2 + 2 * (size_t)layers;
	for (int l = 0; l != layers; ++l) {
		int count = r16(2 + 2 * (size_t)l);
		if (count < 0) return out;
		std::vector<sd_star> v;
		for (int i = 0; i != count; ++i, pos += 8) {
			if (pos + 8 > n) return out;
			size_t off = (size_t)d[pos + 4] | (size_t)d[pos + 5] << 8 | (size_t)d[pos + 6] << 16 | (size_t)d[pos + 7] << 24;
			int w = r16(off), h = r16(off + 2);
			if (w <= 0 || h <= 0 || off + 4 + (size_t)w * h > n) return out;
			v.push_back({r16(pos), r16(pos + 2), w, h, std::vector<uint8_t>(d + off + 4, d + off + 4 + (size_t)w * h)});
		}
		out.layers.push_back(std::move(v));
	}
	out.ok = true;
	return out;
}

// Paints stars into an indexed frame wherever the frame pixel is still 0.
static inline void draw_sd(const sd_t& sd, uint8_t* data, size_t pitch, int sw, int sh, int cam_x, int cam_y) {
	if (!sd.ok) return;
	for (size_t l = 0; l != sd.layers.size(); ++l) {
		int ox = cam_x * speed[l] / 256, oy = cam_y * speed[l] / 256;
		for (auto& s : sd.layers[l]) {
			int bx = wrap(s.x - ox, field_w), by = wrap(s.y - oy, field_h);
			for (int ty = by - field_h; ty < sh; ty += field_h) {
				if (ty + s.h <= 0) continue;
				for (int tx = bx - field_w; tx < sw; tx += field_w) {
					if (tx + s.w <= 0) continue;
					for (int y = 0; y != s.h; ++y) {
						int dy = ty + y;
						if (dy < 0 || dy >= sh) continue;
						uint8_t* row = data + (size_t)dy * pitch;
						const uint8_t* src = s.px.data() + (size_t)y * s.w;
						for (int x = 0; x != s.w; ++x) {
							int dx = tx + x;
							if (dx < 0 || dx >= sw || !src[x] || row[dx]) continue;
							row[dx] = src[x];
						}
					}
				}
			}
		}
	}
}

struct hd_star { int x, y, u, v, w, h; };
struct hd_layer { int w = 0, h = 0; std::vector<hd_star> stars; ddsgrp::frame_t atlas, scaled; int scaled_ws = 0; };
struct hd_t { bool ok = false; std::vector<hd_layer> layers; };

static inline hd_t parse_hd(const uint8_t* d, size_t n, const uint8_t* g, size_t gn) {
	hd_t out;
	auto r16 = [&](size_t o) { return (int)(d[o] | d[o + 1] << 8); };
	auto r32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
	if (n < 8) return out;
	uint32_t layers = r32(0), pos = r32(4);
	if (layers < 1 || layers > 5 || 8 + 8 * (size_t)layers > n) return out;
	ddsgrp::file_t grp = ddsgrp::parse(g, gn);
	if (!grp.ok || grp.frame_count() < layers) return out;
	for (uint32_t l = 0; l != layers; ++l) {
		hd_layer L;
		uint32_t count = r32(8 + 8 * (size_t)l);
		L.w = r16(12 + 8 * (size_t)l);
		L.h = r16(14 + 8 * (size_t)l);
		if (L.w < field_w || L.h < field_h) return out;
		for (uint32_t i = 0; i != count; ++i, pos += 12) {
			if ((size_t)pos + 12 > n) return out;
			L.stars.push_back({r16(pos), r16(pos + 2), r16(pos + 4), r16(pos + 6), r16(pos + 8), r16(pos + 10)});
		}
		L.atlas = grp.decode(l);
		if (L.atlas.w <= 0 || L.atlas.rgba.size() < (size_t)L.atlas.w * L.atlas.h) return out;
		out.layers.push_back(std::move(L));
	}
	out.ok = true;
	return out;
}

// Fills the buffer with black and the star field; `ws` buffer px per logical px.
static inline void draw_hd(hd_t& hd, uint32_t* data, size_t pitch, int sw, int sh, int cam_x, int cam_y, int ws) {
	for (int y = 0; y != sh; ++y) std::fill_n(data + (size_t)y * pitch, (size_t)sw, 0xff000000u);
	if (!hd.ok) return;
	for (size_t l = 0; l != hd.layers.size(); ++l) {
		auto& L = hd.layers[l];
		// layer px per logical px, as a ratio (k = L.w / 648)
		int kn = L.w, kd = field_w;
		if (L.scaled_ws != ws) {
			int aw = (int)((int64_t)L.atlas.w * ws * kd / kn), ah = (int)((int64_t)L.atlas.h * ws * kd / kn);
			L.scaled = ddsgrp::scale_box(L.atlas, aw < 1 ? 1 : aw, ah < 1 ? 1 : ah);
			L.scaled_ws = ws;
		}
		const auto& A = L.scaled;
		if (A.w <= 0) continue;
		int ox = cam_x * speed[l] / 256, oy = cam_y * speed[l] / 256;
		int pw = field_w * ws, ph = field_h * ws;
		for (auto& s : L.stars) {
			int bx = wrap((int)((int64_t)s.x * kd / kn) - ox, field_w) * ws;
			int by = wrap((int)((int64_t)s.y * kd / kn) - oy, field_h) * ws;
			int u = (int)((int64_t)s.u * ws * kd / kn), v = (int)((int64_t)s.v * ws * kd / kn);
			int w = (int)((int64_t)s.w * ws * kd / kn), h = (int)((int64_t)s.h * ws * kd / kn);
			if (w < 1) w = 1;
			if (h < 1) h = 1;
			if (u + w > A.w || v + h > A.h) continue;
			for (int ty = by - ph; ty < sh; ty += ph) {
				if (ty + h <= 0) continue;
				for (int tx = bx - pw; tx < sw; tx += pw) {
					if (tx + w <= 0) continue;
					for (int y = 0; y != h; ++y) {
						int dy = ty + y;
						if (dy < 0 || dy >= sh) continue;
						uint32_t* row = data + (size_t)dy * pitch;
						const uint32_t* src = A.rgba.data() + (size_t)(v + y) * A.w + u;
						for (int x = 0; x != w; ++x) {
							int dx = tx + x;
							if (dx < 0 || dx >= sw) continue;
							uint32_t c = src[x], a = c >> 24;
							if (a < 8) continue;
							uint32_t o = row[dx];
							// additive over black: stars overlap only rarely
							uint32_t r = std::min<uint32_t>(255, ((o >> 16) & 0xff) + (((c >> 16) & 0xff) * a + 127) / 255);
							uint32_t gg = std::min<uint32_t>(255, ((o >> 8) & 0xff) + (((c >> 8) & 0xff) * a + 127) / 255);
							uint32_t b = std::min<uint32_t>(255, (o & 0xff) + ((c & 0xff) * a + 127) / 255);
							row[dx] = 0xff000000u | r << 16 | gg << 8 | b;
						}
					}
				}
			}
		}
	}
}

} // namespace parallax
} // namespace bwgame

#endif
