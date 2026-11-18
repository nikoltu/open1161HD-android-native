#ifndef BWGAME_WEBM_ART_H
#define BWGAME_WEBM_ART_H

// SC:R menu art with an alpha channel (glue/campaign/*.webm: VP9 + a VP9 alpha
// stream in the BlockAdditions, AlphaMode=1) -> a premultiplied-on-black strip.

#include "webm.h"
#include "vp9_inter_pixels.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace bwgame {
namespace webm_art {

struct strip_t {
	int w = 0, h = 0, frames = 0, us_per_frame = 0;
	std::vector<uint32_t> px;   // ARGB, frame f at x = f * w, row stride w * frames
};

namespace detail {
// Area-average downscale of one channel-interleaved frame (3 bytes per pixel).
static inline void shrink(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
	std::vector<uint32_t> acc((size_t)dw * dh * 3, 0), cnt((size_t)dw * dh, 0);
	for (int y = 0; y != sh; ++y) {
		int dy = (int)((int64_t)y * dh / sh);
		const uint8_t* s = src + (size_t)y * sw * 3;
		for (int x = 0; x != sw; ++x) {
			size_t d = (size_t)dy * dw + (size_t)((int64_t)x * dw / sw);
			acc[d * 3] += s[x * 3]; acc[d * 3 + 1] += s[x * 3 + 1]; acc[d * 3 + 2] += s[x * 3 + 2];
			++cnt[d];
		}
	}
	for (size_t i = 0; i != cnt.size(); ++i) {
		uint32_t n = cnt[i] ? cnt[i] : 1;
		dst[i * 3] = (uint8_t)(acc[i * 3] / n); dst[i * 3 + 1] = (uint8_t)(acc[i * 3 + 1] / n); dst[i * 3 + 2] = (uint8_t)(acc[i * 3 + 2] / n);
	}
}
} // namespace detail

// Every `step`-th shown frame, the whole frame scaled to at most `max_h` rows, then
// cropped to the union of where any kept frame is visible. ok=false without alpha.
static inline strip_t decode(const uint8_t* data, size_t size, int step, int max_h) {
	strip_t out;
	if (step < 1) step = 1;
	webm::file_t doc = webm::parse(data, size);
	if (!doc.ok || doc.frames.empty() || doc.codec != "V_VP9") return out;
	std::vector<webm::block_t> alpha = doc.frames;
	for (auto& b : alpha) {
		if (!b.alpha_size) return out;
		b.offset = b.alpha_offset;
		b.size = b.alpha_size;
	}
	int sw = 0, sh = 0, dw = 0, dh = 0;
	std::vector<std::vector<uint8_t>> kept;   // scaled RGB, then premultiplied
	int idx = 0, shown = 0;
	std::function<void(const uint8_t*, int, int)> colour = [&](const uint8_t* rgb, int w, int h) {
		if (idx++ % step) return;
		if (!sw) {
			sw = w; sh = h;
			dh = h > max_h ? max_h : h;
			dw = (int)((int64_t)w * dh / h);
			if (dw < 1) dw = 1;
		}
		if (w != sw || h != sh) return;
		kept.emplace_back((size_t)dw * dh * 3);
		detail::shrink(rgb, w, h, kept.back().data(), dw, dh);
	};
	vp9::decode_sequence_hash(data, size, doc.frames, &shown, true, nullptr, nullptr, nullptr, nullptr, false, &colour);
	if (kept.empty()) return out;
	int x0 = dw, y0 = dh, x1 = -1, y1 = -1;
	std::vector<uint8_t> a((size_t)dw * dh * 3);
	idx = 0;
	size_t k = 0;
	std::function<void(const uint8_t*, int, int)> mask = [&](const uint8_t* lum, int w, int h) {
		if (idx++ % step) return;
		if (k >= kept.size() || w != sw || h != sh) return;
		detail::shrink(lum, w, h, a.data(), dw, dh);
		uint8_t* c = kept[k++].data();
		for (int y = 0; y != dh; ++y) for (int x = 0; x != dw; ++x) {
			size_t i = (size_t)y * dw + x;
			unsigned al = a[i * 3];
			c[i * 3] = (uint8_t)(c[i * 3] * al / 255); c[i * 3 + 1] = (uint8_t)(c[i * 3 + 1] * al / 255); c[i * 3 + 2] = (uint8_t)(c[i * 3 + 2] * al / 255);
			if (al > 8) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
		}
	};
	vp9::decode_sequence_hash(data, size, alpha, &shown, true, nullptr, nullptr, nullptr, nullptr, true, &mask);
	if (k != kept.size() || x1 < x0 || y1 < y0) return out;
	int cw = x1 - x0 + 1, ch = y1 - y0 + 1, n = (int)kept.size();
	out.w = cw; out.h = ch; out.frames = n;
	int64_t span = doc.frames.size() > 1 ? doc.frames.back().timecode - doc.frames.front().timecode : 0;
	int64_t us = span > 0 ? span * (int64_t)doc.timecode_scale / 1000 / (int64_t)(doc.frames.size() - 1) : 66667;
	out.us_per_frame = (int)(us * step);
	out.px.assign((size_t)cw * n * ch, 0xff000000u);
	for (int f = 0; f != n; ++f) {
		const uint8_t* c = kept[f].data();
		for (int y = 0; y != ch; ++y) {
			const uint8_t* s = c + ((size_t)(y0 + y) * dw + x0) * 3;
			uint32_t* d = out.px.data() + (size_t)y * cw * n + (size_t)f * cw;
			for (int x = 0; x != cw; ++x) d[x] = 0xff000000u | ((uint32_t)s[x * 3] << 16) | ((uint32_t)s[x * 3 + 1] << 8) | s[x * 3 + 2];
		}
	}
	return out;
}

} // namespace webm_art
} // namespace bwgame

#endif
