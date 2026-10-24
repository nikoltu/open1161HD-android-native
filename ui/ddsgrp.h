// SC:R .dds.grp container (HD icons/wireframes): 12-byte header
// {u32 file_size, u16 frame_count, u16 unk, u32 unk}, then per frame
// {u16 w, u16 h, u32 data_size, DDS blob} back to back. DXT1/DXT5 frames
// decoded via the BC decoders in anim.h. Also hosts a PNG loader (for
// user icon mods) and an area-average scaler shared by the icon paths.
#ifndef BWGAME_UI_DDSGRP_H
#define BWGAME_UI_DDSGRP_H

#include "anim.h"

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace bwgame {
namespace ddsgrp {

struct frame_t {
	int w = 0;
	int h = 0;
	std::vector<uint32_t> rgba; // ARGB
};

// One frame's location inside the file. hw/hh are the container's own u16
// dimensions -- a cheap hint that needs no decoding; the authoritative size is
// the DDS header's, which decode() reports.
struct entry_t {
	int hw = 0;
	int hh = 0;
	size_t off = 0;
	size_t size = 0;
};

// Nothing but the frame index is held; frames are decoded on demand.
//
// Decoding every frame up front cost ~114 MB for wirefram.dds.grp alone on an
// HD2 install, and it was pure waste: both wireframe callers immediately slice
// the frame into cells, scale them into their own cache and never look at the
// full-resolution pixels again. Same for cmdicons: get_icon_override() keeps
// the scaled 36x34 icon, not the HD source. So decode() deliberately returns
// by value and caches NOTHING -- the callers' own caches are the storage.
//
// load_file() does not keep the file bytes either (they are another 30 MB for
// wirefram.dds.grp); it records the path and reads back just the one frame's
// blob when asked. Each frame is decoded once per unit type per box size, so
// this is a handful of short reads over a whole session. parse() -- used when
// the bytes are already in memory -- keeps them, since it has no path to
// re-read from.
struct file_t {
	bool ok = false;
	std::string path;
	std::vector<uint8_t> raw;   // only when constructed from memory
	std::vector<entry_t> entries;

	size_t frame_count() const { return entries.size(); }

	frame_t decode(size_t i) const {
		frame_t f;
		if (i >= entries.size()) return f;
		const entry_t& e = entries[i];
		if (!e.size) return f;
		const uint8_t* blob = nullptr;
		std::vector<uint8_t> tmp;
		if (!raw.empty()) {
			if (e.off + e.size > raw.size()) return f;
			blob = raw.data() + e.off;
		} else {
			FILE* fp = fopen(path.c_str(), "rb");
			if (!fp) return f;
			tmp.resize(e.size);
			bool got = fseek(fp, (long)e.off, SEEK_SET) == 0
				&& fread(tmp.data(), 1, e.size, fp) == e.size;
			fclose(fp);
			if (!got) return f;
			blob = tmp.data();
		}
		int tw = 0, th = 0;
		if (anim::detail::decode_dds(blob, e.size, f.rgba, tw, th)) {
			// open1161HD Faze 4.7c: never hand back a frame whose rgba is shorter
			// than w*h (a truncated/partial DDS decode -- possible under FULL HD
			// memory pressure). Every consumer indexes rgba up to w*h-1, so a short
			// buffer is an out-of-bounds heap read. Treat it as an empty frame.
			if (f.rgba.size() >= (size_t)tw * (size_t)th) {
				f.w = tw;
				f.h = th;
			} else {
				f.rgba.clear();
			}
		}
		return f;
	}
};

// Indexing pass only: walks the frame table, copies the bytes, decodes nothing.
static inline file_t parse(const uint8_t* data, size_t size) {
	using anim::detail::rd16;
	using anim::detail::rd32;
	file_t out;
	if (size < 12) return out;
	int count = rd16(data + 4);
	size_t pos = 12;
	bool complete = true;
	out.entries.reserve(count);
	for (int i = 0; i != count; ++i) {
		if (pos + 8 > size) { complete = false; break; }
		entry_t e;
		e.hw = rd16(data + pos);
		e.hh = rd16(data + pos + 2);
		uint32_t dsz = rd32(data + pos + 4);
		pos += 8;
		if (pos + dsz > size) { complete = false; break; }
		e.off = pos;
		e.size = dsz;
		pos += dsz;
		pos += 4; // per-frame u32 trailer (observed 0; absent-at-EOF tolerated)
		out.entries.push_back(e);
	}
	// Same acceptance rule as the eager parser: a truncated file is not ok.
	if (complete && !out.entries.empty()) {
		out.raw.assign(data, data + size);
		out.ok = true;
	}
	return out;
}

// Index by walking the frame headers with seeks: 12-byte file header, then
// per frame an 8-byte header followed by its blob. Never reads a blob, so a
// 30 MB file costs a few hundred 8-byte reads and no memory spike.
static inline file_t load_file(const std::string& path) {
	using anim::detail::rd16;
	using anim::detail::rd32;
	file_t out;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t hdr[12];
	if (fsz < 12 || fread(hdr, 1, 12, f) != 12) { fclose(f); return out; }
	int count = rd16(hdr + 4);
	size_t pos = 12;
	bool complete = true;
	out.entries.reserve(count);
	for (int i = 0; i != count; ++i) {
		uint8_t fh[8];
		if (pos + 8 > (size_t)fsz || fread(fh, 1, 8, f) != 8) { complete = false; break; }
		entry_t e;
		e.hw = rd16(fh);
		e.hh = rd16(fh + 2);
		uint32_t dsz = rd32(fh + 4);
		pos += 8;
		if (pos + dsz > (size_t)fsz) { complete = false; break; }
		e.off = pos;
		e.size = dsz;
		pos += dsz;
		pos += 4; // per-frame u32 trailer (observed 0; absent-at-EOF tolerated)
		if (fseek(f, (long)pos, SEEK_SET) != 0) { complete = false; break; }
		out.entries.push_back(e);
	}
	fclose(f);
	if (complete && !out.entries.empty()) {
		out.path = path;
		out.ok = true;
	} else {
		out.entries.clear();
	}
	return out;
}

// PNG -> ARGB frame (user icon mods).
static inline frame_t load_png(const std::string& path) {
	frame_t out;
	int w = 0, h = 0, comp = 0;
	unsigned char* p = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!p) return out;
	out.w = w;
	out.h = h;
	out.rgba.resize((size_t)w * h);
	for (size_t i = 0; i != out.rgba.size(); ++i) {
		const unsigned char* px = p + i * 4;
		out.rgba[i] = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
	}
	stbi_image_free(p);
	return out;
}

// Area-average downscale/upscale to dw x dh (alpha-weighted like the anim
// atlas downscale; handles arbitrary ratios, e.g. 512 -> 36).
static inline frame_t scale_box(const frame_t& src, int dw, int dh) {
	frame_t out;
	if (src.w <= 0 || src.h <= 0 || dw <= 0 || dh <= 0) return out;
	// open1161HD Faze 4.7c: guard against an undersized source (a partial DDS
	// decode -- e.g. the scale-4 HD wireframe dropped and re-fetched under FULL HD
	// memory pressure). The inner loop reads src.rgba[sy*src.w + sx] up to
	// src.w*src.h-1; if rgba is short, that is an out-of-bounds heap read (the
	// intermittent 0xb4.. SIGSEGV). Bail to an empty frame instead.
	if (src.rgba.size() < (size_t)src.w * (size_t)src.h) return out;
	out.w = dw;
	out.h = dh;
	out.rgba.resize((size_t)dw * dh);
	for (int y = 0; y != dh; ++y) {
		int sy0 = y * src.h / dh, sy1 = (y + 1) * src.h / dh;
		if (sy1 <= sy0) sy1 = sy0 + 1;
		for (int x = 0; x != dw; ++x) {
			int sx0 = x * src.w / dw, sx1 = (x + 1) * src.w / dw;
			if (sx1 <= sx0) sx1 = sx0 + 1;
			uint64_t a = 0, r = 0, g = 0, b = 0;
			for (int sy = sy0; sy != sy1; ++sy) {
				const uint32_t* row = &src.rgba[(size_t)sy * src.w];
				for (int sx = sx0; sx != sx1; ++sx) {
					uint32_t c = row[sx];
					uint32_t ca = c >> 24;
					a += ca;
					r += ((c >> 16) & 0xff) * ca;
					g += ((c >> 8) & 0xff) * ca;
					b += (c & 0xff) * ca;
				}
			}
			uint64_t n = (uint64_t)(sy1 - sy0) * (sx1 - sx0);
			uint32_t oa = (uint32_t)(a / n);
			out.rgba[(size_t)y * dw + x] = a == 0 ? 0
				: ((oa << 24) | ((uint32_t)(r / a) << 16) | ((uint32_t)(g / a) << 8) | (uint32_t)(b / a));
		}
	}
	return out;
}

} // namespace ddsgrp
} // namespace bwgame

#endif
