// SC:R HD sprite metadata tables (render-only):
//   images.rel     999 x {u32 flags, u32 target_image}; flag 0x200 = draw with
//                  the target image's HD anim (chains allowed, frame counts match).
//   anim/main.lit  authored per-image/per-frame point lights:
//                  u32 count, u32 0, u32 0, count x u32 offset; at an offset:
//                  u32 frames, frames x {i32 x, i32 y, u32 colour, u32 intensity, u32 radius}.
//                  x/y = anim frame units from the frame crop's top-left (measured: 90% of
//                  keys land on an opaque texel); colour = 0xAABBGGRR; offset 0 = no entry.
// Shared by ui.h and tools/hdrelcheck.cpp (CASC coverage harness).
#ifndef BWGAME_UI_HD_REL_H
#define BWGAME_UI_HD_REL_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace bwgame {
namespace hdrel {

static const uint32_t rel_flag_alias = 0x200;

struct rel_table {
	bool ok = false;
	std::vector<uint32_t> flags;
	std::vector<uint32_t> target;

	static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

	bool parse(const uint8_t* d, size_t n) {
		ok = false;
		flags.clear(); target.clear();
		if (n == 0 || n % 8 || n / 8 > 4096) return false;
		size_t cnt = n / 8;
		flags.resize(cnt); target.resize(cnt);
		for (size_t i = 0; i != cnt; ++i) { flags[i] = rd32(d + i * 8); target[i] = rd32(d + i * 8 + 4); }
		ok = true;
		return true;
	}
	size_t size() const { return flags.size(); }
	bool is_alias(int id) const {
		return ok && id >= 0 && (size_t)id < flags.size() && (flags[id] & rel_flag_alias) && target[id] < flags.size();
	}
	// Image whose main_NNN.anim draws `id`: follows the alias chain. -1 on a cycle.
	int resolve(int id) const {
		if (!ok || id < 0 || (size_t)id >= flags.size()) return id;
		for (int depth = 0; depth < 32; ++depth) {
			if (!is_alias(id)) return id;
			id = (int)target[id];
		}
		return -1;
	}
};

struct light_key {
	int32_t x = 0, y = 0;
	uint32_t rgb = 0, intensity = 0, radius = 0;
	bool active() const { return intensity != 0 && radius != 0; }
	uint32_t rgb888() const { return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff); }
};

struct lit_table {
	bool ok = false;
	std::vector<std::vector<light_key>> images; // index = image id; empty = no entry

	bool parse(const uint8_t* d, size_t n) {
		ok = false;
		images.clear();
		if (n < 12) return false;
		uint32_t cnt = rel_table::rd32(d);
		if (cnt == 0 || cnt > 4096 || rel_table::rd32(d + 4) || rel_table::rd32(d + 8)) return false;
		if (12 + (size_t)cnt * 4 > n) return false;
		images.resize(cnt);
		for (uint32_t i = 0; i != cnt; ++i) {
			uint32_t off = rel_table::rd32(d + 12 + (size_t)i * 4);
			if (off == 0) continue;
			if ((size_t)off + 4 > n) return false;
			uint32_t fc = rel_table::rd32(d + off);
			if (fc > 65535 || (size_t)off + 4 + (size_t)fc * 20 > n) return false;
			auto& v = images[i];
			v.resize(fc);
			const uint8_t* p = d + off + 4;
			for (uint32_t f = 0; f != fc; ++f, p += 20) {
				v[f].x = (int32_t)rel_table::rd32(p);
				v[f].y = (int32_t)rel_table::rd32(p + 4);
				v[f].rgb = rel_table::rd32(p + 8);
				v[f].intensity = rel_table::rd32(p + 12);
				v[f].radius = rel_table::rd32(p + 16);
			}
		}
		ok = true;
		return true;
	}
	const light_key* get(int image_id, size_t frame) const {
		if (!ok || image_id < 0 || (size_t)image_id >= images.size()) return nullptr;
		auto& v = images[image_id];
		if (frame >= v.size() || !v[frame].active()) return nullptr;
		return &v[frame];
	}
	bool has(int image_id) const { return ok && image_id >= 0 && (size_t)image_id < images.size() && !images[image_id].empty(); }
};

} // namespace hdrel
} // namespace bwgame

#endif
