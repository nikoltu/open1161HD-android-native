#ifndef BWGAME_SCR_FONT_H
#define BWGAME_SCR_FONT_H

// SC:R bitmap font (font/fontNN.fnt + .dds): "FONT", u32, u8 first, u8 last,
// u8 max w, u8 line h, u32, then 8 bytes per glyph {u8 xoff, u8 yoff, u8 w, u8 h,
// u16 x, u16 y} into a DXT1 atlas whose alpha is the coverage.

#include "anim.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bwgame {
namespace scr_font {

struct glyph_t { int xo = 0, yo = 0, w = 0, h = 0; std::vector<uint8_t> cov; };
struct font_t { bool ok = false; int first = 0, line_h = 0; std::vector<glyph_t> glyphs; };

static inline font_t parse(const uint8_t* fn, size_t fnn, const uint8_t* dds, size_t ddsn) {
	font_t out;
	if (fnn < 16 || fn[0] != 'F' || fn[1] != 'O' || fn[2] != 'N' || fn[3] != 'T') return out;
	int first = fn[8], last = fn[9];
	if (last < first) return out;
	std::vector<uint32_t> px;
	int aw = 0, ah = 0;
	if (!anim::detail::decode_dds(dds, ddsn, px, aw, ah) || px.size() < (size_t)aw * ah) return out;
	out.first = first;
	out.line_h = fn[11];
	for (int c = first; c <= last; ++c) {
		size_t e = 12 + 8 * (size_t)(c - first);
		if (e + 8 > fnn) return font_t();
		glyph_t g;
		g.xo = fn[e]; g.yo = fn[e + 1]; g.w = fn[e + 2]; g.h = fn[e + 3];
		int gx = fn[e + 4] | fn[e + 5] << 8, gy = fn[e + 6] | fn[e + 7] << 8;
		if (gx + g.w > aw || gy + g.h > ah) g.w = g.h = 0;
		g.cov.resize((size_t)g.w * g.h);
		for (int y = 0; y != g.h; ++y)
			for (int x = 0; x != g.w; ++x) g.cov[(size_t)y * g.w + x] = (uint8_t)(px[(size_t)(gy + y) * aw + gx + x] >> 24);
		out.glyphs.push_back(std::move(g));
	}
	out.ok = out.line_h > 0;
	return out;
}

// Draws `s` so it spans exactly `box_w` px from x (the monospace layout's width):
// glyphs keep their shapes, the spare width goes evenly between them. `cell_h`
// is the monospace cell height the line height is scaled to.
template<typename plot_F>
static inline void draw(const font_t& f, int x, int y, const char* s, int box_w, int cell_h, plot_F&& plot) {
	if (!f.ok || !*s) return;
	const float sc = (float)cell_h / f.line_h;
	auto glyph = [&](unsigned char c) -> const glyph_t* {
		int i = (int)c - f.first;
		return i >= 0 && i < (int)f.glyphs.size() ? &f.glyphs[i] : nullptr;
	};
	const float space_adv = f.line_h * 0.3f;
	int n = 0;
	float natural = 0;
	for (const char* p = s; *p; ++p, ++n) {
		const glyph_t* g = glyph((unsigned char)*p);
		natural += (g && g->w) ? g->xo + g->w + 1 : space_adv;
	}
	natural *= sc;
	float gap = 0, squeeze = 1;
	if (natural <= box_w) gap = (box_w - natural) / n;
	else squeeze = box_w / natural;
	float cx = x + gap / 2;
	for (const char* p = s; *p; ++p) {
		const glyph_t* g = glyph((unsigned char)*p);
		if (!g || !g->w) { cx += (space_adv * sc) * squeeze + gap; continue; }
		int ow = std::max(1, (int)(g->w * sc + 0.5f)), oh = std::max(1, (int)(g->h * sc + 0.5f));
		int gx0 = (int)(cx + g->xo * sc * squeeze + 0.5f), gy0 = y + (int)(g->yo * sc + 0.5f);
		for (int oy = 0; oy != oh; ++oy) {
			int sy0 = oy * g->h / oh, sy1 = std::max(sy0 + 1, (oy + 1) * g->h / oh);
			for (int ox = 0; ox != ow; ++ox) {
				int sx0 = ox * g->w / ow, sx1 = std::max(sx0 + 1, (ox + 1) * g->w / ow);
				int sum = 0;
				for (int sy = sy0; sy < sy1; ++sy) for (int sx = sx0; sx < sx1; ++sx) sum += g->cov[(size_t)sy * g->w + sx];
				int cov = sum / ((sy1 - sy0) * (sx1 - sx0));
				if (cov >= 24) plot(gx0 + ox, gy0 + oy, cov);
			}
		}
		cx += (g->xo + g->w + 1) * sc * squeeze + gap;
	}
}

} // namespace scr_font
} // namespace bwgame

#endif
