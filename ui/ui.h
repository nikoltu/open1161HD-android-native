#include "common.h"
#include "../bwgame.h"
#include "../replay.h"
#include "../savegame.h"   // v0.86: disk save/load serializer (writer_t/reader_t + save/load)

#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"
#include "libsmacker/smacker.h"
#include "font_unispace.h"
#include "loading_screen.h" // standalone loading-screen renderer (host-verified via tools/loadscreen_check.cpp)
#include "anim.h"
#include "ddsgrp.h"
#include "webm.h"
#include "vp9_inter_pixels.h"
#include "diag.h"   // in-engine bug-catcher (DIAG); read-only, runtime-gated

#include <thread>
#include <mutex>
#include <condition_variable>  // perf #2: HD-anim prefetch worker signalling
#include <deque>               // perf #2: HD-anim prefetch request queue
#include <atomic>
#include <memory>
#include <unordered_map>   // hot HD caches (get_hd_anim/get_hd_tile) -- O(1) vs std::map tree
#include <unordered_set>   // DIAG detector de-dup (report a persistent bad state once)
#include <unistd.h>        // sysconf(_SC_PAGESIZE) for the mem_pressure RSS read

namespace bwgame {

struct vr4_entry {
	using bitmap_t = std::conditional<is_native_fast_int<uint64_t>::value, uint64_t, uint32_t>::type;
	std::array<bitmap_t, 64 / sizeof(bitmap_t)> bitmap;
	std::array<bitmap_t, 64 / sizeof(bitmap_t)> inverted_bitmap;
};
struct vx4_entry {
	std::array<uint16_t, 16> images;
};

struct pcx_image {
	size_t width;
	size_t height;
	a_vector<uint8_t> data;
};

struct tileset_image_data {
	a_vector<uint8_t> wpe;
	a_vector<vr4_entry> vr4;
	a_vector<vx4_entry> vx4;
	pcx_image dark_pcx;
	std::array<pcx_image, 7> light_pcx;
	grp_t creep_grp;
	int resource_minimap_color;
	std::array<uint8_t, 256> cloak_fade_selector;
};

struct image_data {
	std::array<std::array<uint8_t, 8>, 16> player_unit_colors;
	std::array<uint8_t, 16> player_minimap_colors;
	std::array<uint8_t, 24> selection_colors;
	std::array<uint8_t, 24> hp_bar_colors;
	std::array<int, 0x100> creep_edge_frame_index{};
};

template<typename data_T>
pcx_image load_pcx_data(const data_T& data) {
	data_loading::data_reader_le r(data.data(), data.data() + data.size());
	auto base_r = r;
	auto id = r.get<uint8_t>();
	if (id != 0x0a) error("pcx: invalid identifier %#x", id);
	r.get<uint8_t>(); // version
	auto encoding = r.get<uint8_t>(); // encoding
	auto bpp = r.get<uint8_t>(); // bpp
	auto offset_x = r.get<uint16_t>();
	auto offset_y = r.get<uint16_t>();
	auto last_x = r.get<uint16_t>();
	auto last_y = r.get<uint16_t>();

	if (encoding != 1) error("pcx: invalid encoding %#x", encoding);
	if (bpp != 8) error("pcx: bpp is %d, expected 8", bpp);

	if (offset_x != 0 || offset_y != 0) error("pcx: offset %d %d, expected 0 0", offset_x, offset_y);

	r.skip(2 + 2 + 48 + 1);

	auto bit_planes = r.get<uint8_t>();
	auto bytes_per_line = r.get<uint16_t>();

	size_t width = last_x + 1;
	size_t height = last_y + 1;

	pcx_image pcx;
	pcx.width = width;
	pcx.height = height;

	pcx.data.resize(width * height);

	r = base_r;
	r.skip(128);

	auto padding = bytes_per_line * bit_planes - width;
	if (padding != 0) error("pcx: padding not supported");

	uint8_t* dst = pcx.data.data();
	uint8_t* dst_end = pcx.data.data() + pcx.data.size();

	while (dst != dst_end) {
		auto v = r.get<uint8_t>();
		if ((v & 0xc0) == 0xc0) {
			v &= 0x3f;
			auto c = r.get<uint8_t>();
			for (; v; --v) {
				if (dst == dst_end) error("pcx: failed to decode");
				*dst++ = c;
			}
		} else {
			*dst = v;
			++dst;
		}
	}

	return pcx;
}

static inline std::unique_ptr<native_window_drawing::surface> flip_image(native_window_drawing::surface* src) {
	auto tmp = native_window_drawing::create_rgba_surface(src->w, src->h);
	src->blit(&*tmp, 0, 0);
	void* ptr = tmp->lock();
	uint32_t* pixels = (uint32_t*)ptr;
	for (size_t y = 0; y != (size_t)tmp->h; ++y) {
		for (size_t x = 0; x != (size_t)tmp->w / 2; ++x) {
			std::swap(pixels[x], pixels[tmp->w - 1 - x]);
		}
		pixels += tmp->pitch / 4;
	}
	tmp->unlock();
	return tmp;
}

template<typename load_data_file_F>
void load_image_data(image_data& img, load_data_file_F&& load_data_file) {

	std::array<int, 0x100> creep_edge_neighbors_index{};
	std::array<int, 128> creep_edge_neighbors_index_n{};

	for (size_t i = 0; i != 0x100; ++i) {
		int v = 0;
		if (i & 2) v |= 0x10;
		if (i & 8) v |= 0x24;
		if (i & 0x10) v |= 9;
		if (i & 0x40) v |= 2;
		if ((i & 0xc0) == 0xc0) v |= 1;
		if ((i & 0x60) == 0x60) v |= 4;
		if ((i & 3) == 3) v |= 0x20;
		if ((i & 6) == 6) v |= 8;
		if ((v & 0x21) == 0x21) v |= 0x40;
		if ((v & 0xc) == 0xc) v |= 0x40;
		creep_edge_neighbors_index[i] = v;
	}

	int n = 0;
	for (int i = 0; i != 128; ++i) {
		auto it = std::find(creep_edge_neighbors_index.begin(), creep_edge_neighbors_index.end(), i);
		if (it == creep_edge_neighbors_index.end()) continue;
		creep_edge_neighbors_index_n[i] = n;
		++n;
	}

	for (size_t i = 0; i != 0x100; ++i) {
		img.creep_edge_frame_index[i] = creep_edge_neighbors_index_n[creep_edge_neighbors_index[i]];
	}

	a_vector<uint8_t> tmp_data;
	auto load_pcx_file = [&](a_string filename) {
		load_data_file(tmp_data, std::move(filename));
		return load_pcx_data(tmp_data);
	};

	auto tunit_pcx = load_pcx_file("game/tunit.pcx");
	if (tunit_pcx.width != 128 || tunit_pcx.height != 1) error("tunit.pcx dimensions are %dx%d (128x1 required)", tunit_pcx.width, tunit_pcx.height);
	for (size_t i = 0; i != 16; ++i) {
		for (size_t i2 = 0; i2 != 8; ++i2) {
			img.player_unit_colors[i][i2] = tunit_pcx.data[i * 8 + i2];
		}
	}
	auto tminimap_pcx = load_pcx_file("game/tminimap.pcx");
	if (tminimap_pcx.width != 16 || tminimap_pcx.height != 1) error("tminimap.pcx dimensions are %dx%d (16x1 required)", tminimap_pcx.width, tminimap_pcx.height);
	for (size_t i = 0; i != 16; ++i) {
		img.player_minimap_colors[i] = tminimap_pcx.data[i];
	}
	auto tselect_pcx = load_pcx_file("game/tselect.pcx");
	if (tselect_pcx.width != 24 || tselect_pcx.height != 1) error("tselect.pcx dimensions are %dx%d (24x1 required)", tselect_pcx.width, tselect_pcx.height);
	for (size_t i = 0; i != 24; ++i) {
		img.selection_colors[i] = tselect_pcx.data[i];
	}
	auto thpbar_pcx = load_pcx_file("game/thpbar.pcx");
	if (thpbar_pcx.width != 19 || thpbar_pcx.height != 1) error("thpbar.pcx dimensions are %dx%d (19x1 required)", thpbar_pcx.width, thpbar_pcx.height);
	for (size_t i = 0; i != 19; ++i) {
		img.hp_bar_colors[i] = thpbar_pcx.data[i];
	}

}

template<typename load_data_file_F>
void load_tileset_image_data(tileset_image_data& img, size_t tileset_index, load_data_file_F&& load_data_file) {
	using namespace data_loading;

	std::array<const char*, 8> tileset_names = {
		"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"
	};

	a_vector<uint8_t> vr4_data;
	a_vector<uint8_t> vx4_data;

	const char* tileset_name = tileset_names.at(tileset_index);

	load_data_file(vr4_data, format("Tileset/%s.vr4", tileset_name));
	load_data_file(vx4_data, format("Tileset/%s.vx4", tileset_name));
	load_data_file(img.wpe, format("Tileset/%s.wpe", tileset_name));

	a_vector<uint8_t> grp_data;
	load_data_file(grp_data, format("Tileset/%s.grp", tileset_name));
	img.creep_grp = read_grp(data_loading::data_reader_le(grp_data.data(), grp_data.data() + grp_data.size()));

	data_reader<true, false> vr4_r(vr4_data.data(), nullptr);
	img.vr4.resize(vr4_data.size() / 64);
	for (size_t i = 0; i != img.vr4.size(); ++i) {
		for (size_t i2 = 0; i2 != 8; ++i2) {
			auto r2 = vr4_r;
			auto v = vr4_r.get<uint64_t, true>();
			auto iv = r2.get<uint64_t, false>();
			size_t n = 8 / sizeof(vr4_entry::bitmap_t);
			for (size_t i3 = 0; i3 != n; ++i3) {
				img.vr4[i].bitmap[i2 * n + i3] = (vr4_entry::bitmap_t)v;
				img.vr4[i].inverted_bitmap[i2 * n + i3] = (vr4_entry::bitmap_t)iv;
				v >>= n == 1 ? 0 : 8 * sizeof(vr4_entry::bitmap_t);
				iv >>= n == 1 ? 0 : 8 * sizeof(vr4_entry::bitmap_t);
			}
		}
	}
	data_reader<true, false> vx4_r(vx4_data.data(), vx4_data.data() + vx4_data.size());
	img.vx4.resize(vx4_data.size() / 32);
	for (size_t i = 0; i != img.vx4.size(); ++i) {
		for (size_t i2 = 0; i2 != 16; ++i2) {
			img.vx4[i].images[i2] = vx4_r.get<uint16_t>();
		}
	}

	a_vector<uint8_t> tmp_data;
	auto load_pcx_file = [&](a_string filename) {
		load_data_file(tmp_data, std::move(filename));
		return load_pcx_data(tmp_data);
	};

	img.dark_pcx = load_pcx_file(format("Tileset/%s/dark.pcx", tileset_name));
	if (img.dark_pcx.width != 256 || img.dark_pcx.height != 32) error("invalid dark.pcx");
	for (size_t x = 0; x != 256; ++x) {
		img.dark_pcx.data[256 * 31 + x] = (uint8_t)x;
	}

	std::array<const char*, 7> light_names = {"ofire", "gfire", "bfire", "bexpl", "trans50", "red", "green"};
	for (size_t i = 0; i != 7; ++i) {
		img.light_pcx[i] = load_pcx_file(format("Tileset/%s/%s.pcx", tileset_name, light_names[i]));
	}

	if (img.wpe.size() != 256 * 4) error("wpe size invalid (%d)", img.wpe.size());

	auto get_nearest_color = [&](int r, int g, int b) {
		size_t best_index = -1;
		int best_score{};
		for (size_t i = 0; i != 256; ++i) {
			int dr = r - img.wpe[4 * i + 0];
			int dg = g - img.wpe[4 * i + 1];
			int db = b - img.wpe[4 * i + 2];
			int score = dr * dr + dg * dg + db * db;
			if (best_index == (size_t)-1 || score < best_score) {
				best_index = i;
				best_score = score;
			}
		}
		return best_index;
	};
	img.resource_minimap_color = get_nearest_color(0, 255, 255);

	for (size_t i = 0; i != 256; ++i) {
		int r = img.wpe[4 * i + 0];
		int g = img.wpe[4 * i + 1];
		int b = img.wpe[4 * i + 2];
		int strength = (r * 28 + g * 77 + b * 151 + 4096) / 8192;
		img.cloak_fade_selector[i] = strength;
	}

}

template<bool bounds_check>
void draw_tile(tileset_image_data& img, size_t megatile_index, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height) {
	auto* images = &img.vx4.at(megatile_index).images[0];
	size_t x = 0;
	size_t y = 0;
	for (size_t image_iy = 0; image_iy != 4; ++image_iy) {
		for (size_t image_ix = 0; image_ix != 4; ++image_ix) {
			auto image_index = *images;
			bool inverted = (image_index & 1) == 1;
			auto* bitmap = inverted ? &img.vr4.at(image_index / 2).inverted_bitmap[0] : &img.vr4.at(image_index / 2).bitmap[0];

			for (size_t iy = 0; iy != 8; ++iy) {
				for (size_t iv = 0; iv != 8 / sizeof(vr4_entry::bitmap_t); ++iv) {
					for (size_t b = 0; b != sizeof(vr4_entry::bitmap_t); ++b) {
						if (!bounds_check || (x >= offset_x && y >= offset_y && x < width && y < height)) {
							*dst = (uint8_t)(*bitmap >> (8 * b));
						}
						++dst;
						++x;
					}
					++bitmap;
				}
				x -= 8;
				++y;
				dst -= 8;
				dst += pitch;
			}
			x += 8;
			y -= 8;
			dst -= pitch * 8;
			dst += 8;
			++images;
		}
		x -= 32;
		y += 8;
		dst += pitch * 8;
		dst -= 32;
	}
}

static inline void draw_tile(tileset_image_data& img, size_t megatile_index, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height) {
	if (offset_x == 0 && offset_y == 0 && width == 32 && height == 32) {
		draw_tile<false>(img, megatile_index, dst, pitch, offset_x, offset_y, width, height);
	} else {
		draw_tile<true>(img, megatile_index, dst, pitch, offset_x, offset_y, width, height);
	}
}

template<bool bounds_check, bool flipped, bool textured, typename remap_F>
void draw_frame(const grp_t::frame_t& frame, const uint8_t* texture, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f) {
	for (size_t y = 0; y != offset_y; ++y) {
		dst += pitch;
		if (textured) texture += frame.size.x;
	}

	for (size_t y = offset_y; y != height; ++y) {

		if (flipped) dst += frame.size.x - 1;
		if (textured && flipped) texture += frame.size.x - 1;

		const uint8_t* d = frame.data_container.data() + frame.line_data_offset.at(y);
		for (size_t x = flipped ? frame.size.x - 1 : 0; x != (flipped ? (size_t)0 - 1 : frame.size.x);) {
			int v = *d++;
			if (v & 0x80) {
				v &= 0x7f;
				x += flipped ? -v : v;
				dst += flipped ? -v : v;
				if (textured) texture += flipped ? -v : v;
			} else if (v & 0x40) {
				v &= 0x3f;
				int c = *d++;
				for (;v; --v) {
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f(textured ? *texture : c, *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			} else {
				for (;v; --v) {
					int c = *d++;
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f(textured ? *texture : c, *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			}
		}

		if (!flipped) dst -= frame.size.x;
		else ++dst;
		dst += pitch;
		if (textured) {
			if (!flipped) texture -= frame.size.x;
			else ++texture;
			texture += frame.size.x;
		}

	}
}

struct no_remap {
	uint8_t operator()(uint8_t new_value, uint8_t old_value) const {
		return new_value;
	}
};

template<typename remap_F = no_remap>
void draw_frame(const grp_t::frame_t& frame, bool flipped, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f = remap_F()) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame<false, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<false, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame<true, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<true, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

template<typename remap_F = no_remap>
void draw_frame_textured(const grp_t::frame_t& frame, const uint8_t* texture, bool flipped, uint8_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f = remap_F()) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame<false, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<false, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame<true, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame<true, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

// v0.22 RGBA sprite pass: GRP frame blitter writing 0xAARRGGBB pixels into the
// rgba surface. Mirrors draw_frame()'s RLE walk; remap_f(palette_index, old
// pixel ref) returns the new pixel, so effect modifiers can blend with the
// destination in true color.
template<bool bounds_check, bool flipped, bool textured, typename remap_F>
void draw_frame_rgba(const grp_t::frame_t& frame, const uint8_t* texture, uint32_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f) {
	for (size_t y = 0; y != offset_y; ++y) {
		dst += pitch;
		if (textured) texture += frame.size.x;
	}

	for (size_t y = offset_y; y != height; ++y) {

		if (flipped) dst += frame.size.x - 1;
		if (textured && flipped) texture += frame.size.x - 1;

		const uint8_t* d = frame.data_container.data() + frame.line_data_offset.at(y);
		for (size_t x = flipped ? frame.size.x - 1 : 0; x != (flipped ? (size_t)0 - 1 : frame.size.x);) {
			int v = *d++;
			if (v & 0x80) {
				v &= 0x7f;
				x += flipped ? -v : v;
				dst += flipped ? -v : v;
				if (textured) texture += flipped ? -v : v;
			} else if (v & 0x40) {
				v &= 0x3f;
				int c = *d++;
				for (;v; --v) {
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f((uint8_t)(textured ? *texture : c), *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			} else {
				for (;v; --v) {
					int c = *d++;
					if (!bounds_check || (x >= offset_x && x < width)) {
						*dst = remap_f((uint8_t)(textured ? *texture : c), *dst);
					}
					dst += flipped ? -1 : 1;
					x += flipped ? -1 : 1;
					if (textured) texture += flipped ? -1 : 1;
				}
			}
		}

		if (!flipped) dst -= frame.size.x;
		else ++dst;
		dst += pitch;
		if (textured) {
			if (!flipped) texture -= frame.size.x;
			else ++texture;
			texture += frame.size.x;
		}

	}
}

template<typename remap_F>
void draw_frame_rgba(const grp_t::frame_t& frame, bool flipped, uint32_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame_rgba<false, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame_rgba<false, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame_rgba<true, true, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame_rgba<true, false, false>(frame, nullptr, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

template<typename remap_F>
void draw_frame_rgba_textured(const grp_t::frame_t& frame, const uint8_t* texture, bool flipped, uint32_t* dst, size_t pitch, size_t offset_x, size_t offset_y, size_t width, size_t height, remap_F&& remap_f) {
	if (offset_x == 0 && offset_y == 0 && width == frame.size.x && height == frame.size.y) {
		if (flipped) draw_frame_rgba<false, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame_rgba<false, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	} else {
		if (flipped) draw_frame_rgba<true, true, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
		else draw_frame_rgba<true, false, true>(frame, texture, dst, pitch, offset_x, offset_y, width, height, std::forward<remap_F>(remap_f));
	}
}

// Tolerant GRP decoder for asset variants that trip the core read_grp() bounds
// checks (e.g. game\icons.grp throws "seek past end"). Mirrors read_grp()'s RLE
// exactly but reads from a raw buffer with clamp-to-zero past the end and a
// runaway guard, so a malformed/variant frame degrades to partial pixels instead
// of aborting the whole load. Output data_container matches what draw_frame()
// consumes (0x80=transparent run, 0x40=repeat one byte, else literal run).
inline grp_t read_grp_tolerant(const uint8_t* buf, size_t len) {
	grp_t grp;
	grp.width = 0;
	grp.height = 0;
	auto u8 = [&](size_t p) -> uint8_t { return p < len ? buf[p] : 0; };
	auto u16 = [&](size_t p) -> size_t { return (size_t)u8(p) | ((size_t)u8(p + 1) << 8); };
	auto u32 = [&](size_t p) -> size_t {
		return (size_t)u8(p) | ((size_t)u8(p + 1) << 8) | ((size_t)u8(p + 2) << 16) | ((size_t)u8(p + 3) << 24);
	};
	if (len < 6) return grp;
	size_t frame_count = u16(0);
	grp.width = u16(2);
	grp.height = u16(4);
	if (frame_count == 0 || frame_count > 4096) return grp;
	grp.frames.resize(frame_count);
	for (size_t i = 0; i != frame_count; ++i) {
		auto& f = grp.frames[i];
		size_t hp = 6 + i * 8;
		f.offset.x = u8(hp + 0);
		f.offset.y = u8(hp + 1);
		f.size.x = u8(hp + 2);
		f.size.y = u8(hp + 3);
		size_t file_offset = u32(hp + 4);
		f.line_data_offset.reserve(f.size.y);
		for (size_t y = 0; y != f.size.y; ++y) {
			size_t line_ptr = file_offset + u16(file_offset + y * 2);
			f.line_data_offset.push_back(f.data_container.size());
			for (size_t x = 0; x != f.size.x;) {
				if (line_ptr >= len) break; // clamp/runaway guard (past-end -> 0 forever)
				uint8_t v = u8(line_ptr++);
				if (v & 0x80) {
					v &= 0x7f;
					if (v > f.size.x - x) v = (uint8_t)(f.size.x - x);
					f.data_container.push_back(0x80 | v);
					x += v;
				} else if (v & 0x40) {
					v &= 0x3f;
					if (v > f.size.x - x) v = (uint8_t)(f.size.x - x);
					f.data_container.push_back(0x40 | v);
					f.data_container.push_back(u8(line_ptr++));
					x += v;
				} else {
					if (v > f.size.x - x) v = (uint8_t)(f.size.x - x);
					f.data_container.push_back(v);
					for (size_t k = 0; k != v; ++k) f.data_container.push_back(u8(line_ptr++));
					x += v;
				}
			}
		}
		f.data_container.shrink_to_fit();
	}
	return grp;
}

struct apm_t {
	a_deque<int> history;
	int current_apm = 0;
	int last_frame_div = 0;
	static const int resolution = 1;
	void add_action(int frame) {
		if (!history.empty() && frame / resolution == last_frame_div) {
			++history.back();
		} else {
			if (history.size() >= 10 * 1000 / 42 / resolution) history.pop_front();
			history.push_back(1);
			last_frame_div = frame / 12;
		}
	}
	void update(int frame) {
		if (history.empty() || frame / resolution != last_frame_div) {
			if (history.size() >= 10 * 1000 / 42 / resolution) history.pop_front();
			history.push_back(0);
			last_frame_div = frame / resolution;
		}
		if (frame % resolution) return;
		if (history.size() == 0) {
			current_apm = 0;
			return;
		}
		int sum = 0;
		for (auto& v : history) sum += v;
		current_apm = (int)(sum * ((int64_t)256 * 60 * 1000 / 42 / resolution) / history.size() / 256);
	}
};

struct ui_util_functions: replay_functions {

	explicit ui_util_functions(state& st, action_state& action_st, replay_state& replay_st) : replay_functions(st, action_st, replay_st) {}

	rect sprite_clickable_bounds(const sprite_t* sprite) const {
		rect r{{(int)game_st.map_width - 1, (int)game_st.map_height - 1}, {0, 0}};
		for (const image_t* image : ptr(sprite->images)) {
			if (!i_flag(image, image_t::flag_clickable)) continue;
			xy pos = get_image_map_position(image);
			auto size = image->grp->frames.at(image->frame_index).size;
			xy to = pos + xy((int)size.x, (int)size.y);
			if (pos.x < r.from.x) r.from.x = pos.x;
			if (pos.y < r.from.y) r.from.y = pos.y;
			if (to.x > r.to.x) r.to.x = to.x;
			if (to.y > r.to.y) r.to.y = to.y;
		}
		return r;
	}

	bool unit_can_be_selected(const unit_t* u) const {
		if (unit_is(u, UnitTypes::Terran_Nuclear_Missile)) return false;
		if (unit_is(u, UnitTypes::Protoss_Scarab)) return false;
		if (unit_is(u, UnitTypes::Spell_Disruption_Web)) return false;
		if (unit_is(u, UnitTypes::Spell_Dark_Swarm)) return false;
		if (unit_is(u, UnitTypes::Special_Upper_Level_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Right_Upper_Level_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Pit_Door)) return false;
		if (unit_is(u, UnitTypes::Special_Right_Pit_Door)) return false;
		return true;
	}

	bool image_has_data_at(const image_t* image, xy pos) const {
		auto& frame = image->grp->frames.at(image->frame_index);
		xy map_pos = get_image_map_position(image);
		int x = pos.x - map_pos.x;
		if (i_flag(image, image_t::flag_horizontally_flipped)) x = image->grp->width - 2 * frame.offset.x - x;
		int y = pos.y - map_pos.y;
		if ((size_t)x >= frame.size.x) return false;
		if ((size_t)y >= frame.size.y) return false;

		const uint8_t* d = frame.data_container.data() + frame.line_data_offset.at(y);
		while (x > 0) {
			int v = *d++;
			if (v & 0x80) {
				v &= 0x7f;
				x -= v;
				if (x <= 0) return false;
			} else if (v & 0x40) {
				v &= 0x3f;
				d++;
				x -= v;
			} else {
				x -= v;
			}
		}
		return true;
	}

	bool unit_has_clickable_image_data_at(const unit_t* u, xy pos) const {
		if (!is_in_bounds(pos, sprite_clickable_bounds(u->sprite))) return false;
		if (ut_flag(u, unit_type_t::flag_100)) {
			for (const image_t* image : ptr(u->sprite->images)) {
				if (!i_flag(image, image_t::flag_clickable)) continue;
				if (image_has_data_at(image, pos)) return true;
			}
			return false;
		} else {
			return image_has_data_at(u->sprite->main_image, pos);
		}
	}

	unit_t* select_get_unit_at(xy pos) const {
		rect area = square_at(pos, 32);
		area.to += xy(game_st.max_unit_width, game_st.max_unit_height);
		unit_t* best_unit = nullptr;
		int best_unit_size{};
		for (unit_t* u : find_units_noexpand(area)) {
			if (!is_in_bounds(pos, sprite_clickable_bounds(u->sprite))) continue;
			u = unit_main_unit(u);
			if (!unit_can_be_selected(u)) continue;
			if (!best_unit) {
				best_unit = u;
				best_unit_size = u->unit_type->placement_size.x * u->unit_type->placement_size.y;
				continue;
			}
			if (sprite_depth_order(u->sprite) >= sprite_depth_order(best_unit->sprite)) {
				if (unit_has_clickable_image_data_at(u, pos) || (u->subunit && unit_has_clickable_image_data_at(u->subunit, pos))) {
					best_unit = u;
					best_unit_size = u->sprite->width * u->sprite->height;
					continue;
				}
			} else {
				if (unit_has_clickable_image_data_at(best_unit, pos)) continue;
				if (best_unit->subunit && unit_has_clickable_image_data_at(best_unit->subunit, pos)) continue;
			}
			if (u->unit_type->placement_size.x * u->unit_type->placement_size.y < best_unit_size) {
				best_unit = u;
				best_unit_size = u->unit_type->placement_size.x * u->unit_type->placement_size.y;
			}
		}
		return best_unit;
	}

	uint32_t sprite_depth_order(const sprite_t* sprite) const {
		uint32_t score = 0;
		score |= sprite->elevation_level;
		score <<= 13;
		score |= sprite->elevation_level <= 4 ? sprite->position.y : 0;
		score <<= 1;
		score |= s_flag(sprite, sprite_t::flag_turret) ? 1 : 0;
		return score;
	}

};

// Exact floor(x/255) for x in [0,65025] via reciprocal multiply -- replaces the
// multi-cycle integer divide in the per-pixel HD sprite blend. Verified bit-exact
// against x/255 over the whole [0,65535] range (all blend inputs are products or
// weighted sums of two 0..255 values, so <= 255*255 = 65025).
static inline uint32_t div255(uint32_t x) { return (x * 0x8081u) >> 23; }

struct ui_functions: ui_util_functions {
	image_data img;
	tileset_image_data tileset_img;
	native_window::window wnd;
	bool create_window = true;
	bool draw_ui_elements = true;

	bool exit_on_close = true;
	bool leave_through_host = false;   // the host's main loop picks up request_exit (saves the replay, then returns)
	bool window_closed = false;

	xy screen_pos;

	size_t screen_width;
	size_t screen_height;

	game_player player;
	replay_state current_replay_state;
	action_state current_action_state;
	std::array<apm_t, 12> apm;
	ui_functions(game_player player) : ui_util_functions(player.st(), current_action_state, current_replay_state), player(std::move(player)) {
	}

	// Join the HD-anim prefetch worker before any member it touches (the queues,
	// load_hd_bytes, global_st) is destroyed. Signal quit under the lock, wake it,
	// then join. No-op unless the worker was ever started (SD tier / host tools).
	~ui_functions() {
		if (hd_pf_started) {
			{
				std::lock_guard<std::mutex> lk(hd_pf_mtx);
				hd_pf_quit = true;
			}
			hd_pf_cv.notify_all();
			if (hd_pf_thread.joinable()) hd_pf_thread.join();
		}
		if (music_pf_started) {
			{
				std::lock_guard<std::mutex> lk(music_pf_mtx);
				music_pf_quit = true;
			}
			music_pf_cv.notify_all();
			if (music_pf_thread.joinable()) music_pf_thread.join();
		}
	}

	std::function<void(a_vector<uint8_t>&, a_string)> load_data_file;

	sound_types_t sound_types;
	a_vector<a_string> sound_filenames;

	const sound_type_t* get_sound_type(Sounds id) const {
		if ((size_t)id >= (size_t)Sounds::None) error("invalid sound id %d", (size_t)id);
		return &sound_types.vec[(size_t)id];
	}

	a_vector<bool> has_loaded_sound;
	a_vector<std::unique_ptr<native_sound::sound>> loaded_sounds;
	a_vector<std::chrono::high_resolution_clock::time_point> last_played_sound;

	int global_volume = 50;
	// Faze 7+: per-category submixes, each a 0..100 factor applied on top of the
	// master (global_volume). SFX = world/positional sound; voice = non-positional
	// unit responses ("yes sir", ready lines); ui = HUD alerts + mission voice wavs.
	// The category is chosen by the play site, so no sfxdata.dat classification is
	// needed (world sounds always carry a position; voice responses never do).
	int sfx_volume = 100;
	int voice_volume = 100;
	int ui_volume = 100;
	// Unit voice-response frequency (0..100): probability a selection/order
	// acknowledgement actually plays. 100 = original BW behaviour (always).
	int voice_freq = 100;

	struct sound_channel {
		bool playing = false;
		const sound_type_t* sound_type = nullptr;
		int priority = 0;
		int flags = 0;
		const unit_type_t* unit_type = nullptr;
		int volume = 0;
		int cat = 100; // category submix factor in effect for the current sound
	};
	a_vector<sound_channel> sound_channels;

	void set_volume(int volume) {
		if (volume < 0) volume = 0;
		else if (volume > 100) volume = 100;
		global_volume = volume;
		for (auto& c : sound_channels) {
			if (c.playing) {
				native_sound::set_volume(&c - sound_channels.data(), (128 - 4) * (c.volume * global_volume / 100) / 100 * c.cat / 100);
			}
		}
	}

	sound_channel* get_sound_channel(int priority) {
		sound_channel* r = nullptr;
		for (auto& c : sound_channels) {
			if (c.playing) {
				if (!native_sound::is_playing(&c - sound_channels.data())) {
					c.playing = false;
					r = &c;
				}
			} else r = &c;
		}
		if (r) return r;
		int best_prio = priority;
		for (auto& c : sound_channels) {
			if (c.flags & 0x20) continue;
			if (c.priority < best_prio) {
				best_prio = c.priority;
				r = &c;
			}
		}
		return r;
	}

	virtual void play_sound(int id, xy position, const unit_t* source_unit, bool add_race_index) override {
		if (global_volume == 0) return;
		// v0.83.001: add_race_index means "advance to this race's line", not
		// "+1". sfxdata.dat lays these out in race triples in the engine's own
		// order -- measured with `aidump sfx 122 14`:
		//   127 Zerg\Advisor\ZAdUpd04   128 Terran\...\TAdUpd04   129 Protoss\...
		// The old `id += 1` therefore played the TERRAN line to everyone. This
		// is the only call site that passes the flag (bwgame.h order_NukeLaunch),
		// which is why it went unnoticed.
		if (add_race_index) {
			int p = game_local_player;
			race_t r = (p >= 0 && p < 12) ? st.players[p].race : race_t::terran;
			id += (int)r;
		}
		if ((size_t)id >= has_loaded_sound.size()) return;
		if (!has_loaded_sound[id]) {
			has_loaded_sound[id] = true;
			// v0.83.002: guarded for the same reason the briefing WAV now is --
			// a sound that fails to load must cost silence, not the game. This
			// call sits inside the frame loop, so an escaping exception ends the
			// mission. (A mod shipping a broken sound file is enough.)
			try {
				a_vector<uint8_t> data;
				load_data_file(data, "sound/" + sound_filenames[id]);
				loaded_sounds[id] = native_sound::load_wav(data.data(), data.size());
			} catch (const std::exception& e) {
				ui::log("sound %d ('%s') failed to load (%s)\n", id, sound_filenames[id].c_str(), e.what());
			}
		}
		auto& s = loaded_sounds[id];
		if (!s) return;

		auto now = clock.now();
		if (now - last_played_sound[id] <= std::chrono::milliseconds(80)) return;
		last_played_sound[id] = now;

		const sound_type_t* sound_type = get_sound_type((Sounds)id);

		// Non-positional sounds (unit voice responses, ready sounds) play at
		// full volume - min_volume is only an attenuation floor for world
		// sounds, and is 0 for voice entries in sfxdata.dat (this silenced
		// every unit response after the positional-audio rework).
		int volume = position == xy() ? 99 : sound_type->min_volume;

		if (position != xy()) {
			int distance = 0;
			if (position.x < screen_pos.x) distance += screen_pos.x - position.x;
			else if (position.x > screen_pos.x + (int)screen_width) distance += position.x - (screen_pos.x + (int)screen_width);
			if (position.y < screen_pos.y) distance += screen_pos.y - position.y;
			else if (position.y > screen_pos.y + (int)screen_height) distance += position.y - (screen_pos.y + (int)screen_height);

			int distance_volume = 99 - 99 * distance / 512;

			if (distance_volume > volume) volume = distance_volume;
		}

		if (volume > 10) {
			// Positional stereo: pan by horizontal distance from the screen center
			// (original BW pan curve, in 32px tiles). Negative = left.
			int pan = 0;
			if (position != xy()) {
				int pan_x = (position.x - (screen_pos.x + (int)screen_width / 2)) / 32;
				bool left = pan_x < 0;
				if (left) pan_x = -pan_x;
				if (pan_x <= 2) pan = 0;
				else if (pan_x <= 5) pan = 52;
				else if (pan_x <= 10) pan = 127;
				else if (pan_x <= 20) pan = 191;
				else if (pan_x <= 40) pan = 230;
				else pan = 255;
				if (left) pan = -pan;
			}

			const unit_type_t* unit_type = source_unit ? source_unit->unit_type : nullptr;

			if (sound_type->flags & 0x10) {
				for (auto& c : sound_channels) {
					if (c.playing && c.sound_type == sound_type) {
						if (native_sound::is_playing(&c - sound_channels.data())) return;
						c.playing = false;
					}
				}
			} else if (sound_type->flags & 2 && unit_type) {
				for (auto& c : sound_channels) {
					if (c.playing && c.unit_type == unit_type && c.flags & 2) {
						if (native_sound::is_playing(&c - sound_channels.data())) return;
						c.playing = false;
					}
				}
			}

			// Category submix: non-positional plays are unit voice responses /
			// ready lines; anything with a world position is a sound effect.
			int cat = (position == xy()) ? voice_volume : sfx_volume;
			auto* c = get_sound_channel(sound_type->priority);
			if (c) {
				native_sound::play(c - sound_channels.data(), &*s, (128 - 4) * (volume * global_volume / 100) / 100 * cat / 100, pan);
				c->playing = true;
				c->sound_type = sound_type;
				c->flags = sound_type->flags;
				c->priority = sound_type->priority;
				c->unit_type = unit_type;
				c->volume = volume;
				c->cat = cat;
			}
		}
	}

	// --- Unit voice responses ("what" on select, "yes" on order) ------------
	// UI feedback only, so a separate non-deterministic RNG is used — never the
	// sim's lcg_rand, which would desync a LAN game.
	uint32_t ui_sound_rng = 0x9e3779b9u;
	int ui_rand(int n) { ui_sound_rng = ui_sound_rng * 1103515245u + 12345u; return n <= 0 ? 0 : (int)((ui_sound_rng >> 16) % (uint32_t)n); }

	void play_unit_response(const unit_t* u, int first, int last) {
		if (!u) return;
		// v0.83.002: this guard is why critters stayed silent. v0.83.001 taught
		// the SELECTION site to accept a neutral unit (first_neutral_selected_unit)
		// and then handed it to this function, which dropped anything not owned
		// by the local player -- so the fix could never have worked, and the
		// user's "still no squeak" was right. The same failure as in v0.82: fix the
		// caller, forget the callee.
		//
		// Neutral (owner 11) and critters speak; other players' units do not,
		// because a voice line would leak an enemy's position.
		bool own = u->owner == game_local_player;
		bool neutral = u->owner == 11 || unit_is_critter(u->unit_type);
		if (!own && !neutral) return;
		if (st.trigger_speech_muted & (1u << u->owner)) return; // trigger Mute Unit Speech
		if (first <= 0 || last < first) return;
		// Voice-response frequency: throttle routine acknowledgements without
		// touching the portrait talk state (so the head still animates on select).
		if (voice_freq < 100 && ui_rand(100) >= voice_freq) { portrait_talk(u); return; }
		play_sound(first + ui_rand(last - first + 1), xy(), u, false);
		portrait_talk(u);
	}
	// Selecting the same unit over and over cycles through its "pissed" lines
	// (the classic BW easter egg) instead of the plain "what" response.
	int what_repeat_unit_id = -1;
	int what_repeat_count = 0;
	int pissed_cycle = 0;
	std::chrono::high_resolution_clock::time_point last_what_time;
	void play_unit_what(const unit_t* u) {
		if (!u) return;
		int id = (int)get_unit_id(u).raw_value;
		auto now = clock.now();
		if (id == what_repeat_unit_id && now - last_what_time <= std::chrono::seconds(8)) ++what_repeat_count;
		else what_repeat_count = 0;
		what_repeat_unit_id = id;
		last_what_time = now;
		int first_pissed = u->unit_type->first_pissed_sound;
		int last_pissed = u->unit_type->last_pissed_sound;
		if (what_repeat_count >= 3 && first_pissed > 0 && last_pissed >= first_pissed && u->owner == game_local_player) {
			int n = last_pissed - first_pissed + 1;
			play_sound(first_pissed + (pissed_cycle++ % n), xy(), u, false);
			portrait_talk(u);
			return;
		}
		play_unit_response(u, u->unit_type->first_what_sound, u->unit_type->last_what_sound);
	}
	void play_unit_yes(const unit_t* u) { if (u) play_unit_response(u, u->unit_type->first_yes_sound, u->unit_type->last_yes_sound); }

	// --- Ready sounds ("Battlecruiser operational") ----------------------
	// No engine hook fires when training completes, so poll own units and
	// play the unit's ready_sound on the incomplete -> complete transition.
	a_unordered_set<uint32_t> ready_pending;
	void poll_ready_sounds() {
		if (!game_input || game_local_player < 0) return;
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player) continue;
			uint32_t id = get_unit_id(u).raw_value;
			if (!u_completed(u)) ready_pending.insert(id);
			else {
				auto it = ready_pending.find(id);
				if (it != ready_pending.end()) {
					ready_pending.erase(it);
					if (u->unit_type->ready_sound > 0 && !ut_building(u)) play_sound(u->unit_type->ready_sound, xy(), u, false);
				}
			}
		}
	}

	// --- Music ---------------------------------------------------------
	// In-game tracks streamed from the MPQs (music/<race>N.wav). The playlist
	// is built once from whichever files actually exist (probed via
	// data_file_exists so vanilla/BW track counts both work), starts at a
	// random track and then rotates. Streaming is handled by native_sound;
	// here we only feed it the next file when the current one ends.
	std::function<bool(a_string)> data_file_exists;
	int music_volume = 50;
	a_vector<a_string> music_playlist;
	size_t music_next_index = 0;
	bool music_initialized = false;

	// --- Music preload (fixes the track-switch stutter) ------------------
	// poll_music used to load_data_file the NEXT track synchronously the moment
	// the current one ended -- a multi-MB WAV decompressed from the MPQ on the
	// game thread, which stalled the frame (user-reported stutter at every track
	// change). A background worker now fetches the next track's bytes ahead of
	// time; at the switch poll_music takes the ready buffer and play_music is
	// instant. Same MPQ lock as every other read (load_data_file), so it never
	// races the main thread's sound reads. Pure audio/IO -- no sim, no gate.
	std::thread music_pf_thread;
	std::mutex music_pf_mtx;
	std::condition_variable music_pf_cv;
	a_vector<uint8_t> music_pf_data;     // preloaded bytes for music_pf_name
	a_string music_pf_name;              // which track music_pf_data holds ("" = none)
	a_string music_pf_want;              // track the worker should load next ("" = idle)
	bool music_pf_ready = false;
	bool music_pf_quit = false;
	bool music_pf_started = false;

	void music_prefetch_start() {
		if (music_pf_started) return;
		music_pf_started = true;
		music_pf_thread = std::thread([this] { music_prefetch_loop(); });
	}
	void music_prefetch_loop() {
		for (;;) {
			a_string want;
			{
				std::unique_lock<std::mutex> lk(music_pf_mtx);
				music_pf_cv.wait(lk, [this] { return music_pf_quit || !music_pf_want.empty(); });
				if (music_pf_quit) return;
				want = music_pf_want;
				music_pf_want.clear();
			}
			a_vector<uint8_t> data;
			try { load_data_file(data, want); }
			catch (...) { data.clear(); }
			{
				std::lock_guard<std::mutex> lk(music_pf_mtx);
				music_pf_data.swap(data);
				music_pf_name.swap(want);
				music_pf_ready = !music_pf_data.empty();
			}
		}
	}
	// Ask the worker to fetch `name` (the next track) unless it already holds it.
	void request_music_preload(a_string name) {
		if (name.empty()) return;
		music_prefetch_start();
		{
			std::lock_guard<std::mutex> lk(music_pf_mtx);
			if (music_pf_ready && music_pf_name == name) return; // already staged
			if (music_pf_want == name) return;                   // already queued
			music_pf_want = std::move(name);
		}
		music_pf_cv.notify_one();
	}

	void set_music_volume(int volume) {
		if (volume < 0) volume = 0;
		else if (volume > 100) volume = 100;
		music_volume = volume;
		if (volume == 0) native_sound::stop_music();
		else native_sound::set_music_volume((128 - 4) * volume / 100);
	}

	void init_music() {
		music_initialized = true;
		if (!data_file_exists) return;
		const char* race_names[3] = {"zerg", "terran", "protoss"};
		const char* race_name = nullptr;
		if (game_local_player >= 0 && game_local_player < 12) {
			race_t r = st.players[game_local_player].race;
			if ((size_t)r < 3) race_name = race_names[(size_t)r];
		}
		auto probe_race = [&](const char* name) {
			for (int i = 1; i <= 9; ++i) {
				char buf[32];
				snprintf(buf, sizeof(buf), "music/%s%d.wav", name, i);
				if (data_file_exists(buf)) music_playlist.push_back(buf);
			}
		};
		if (race_name) probe_race(race_name);
		else for (auto* name : race_names) probe_race(name); // replay/observer: mix all
		if (!music_playlist.empty()) music_next_index = (size_t)ui_rand((int)music_playlist.size());
		ui::log("music: %d tracks (%s)\n", (int)music_playlist.size(), race_name ? race_name : "all races");
	}

	void poll_music() {
		// Faze 4.7 (user request): in-game music belongs to GAMEPLAY, not to the
		// briefing (which has its own voice-overs) or the loading/lobby screens.
		if (briefing_active || lobby_mode || loading_mode) return;
		if (music_volume == 0) return;
		if (!music_initialized) init_music();
		if (music_playlist.empty()) return;
		if (native_sound::music_playing()) return;
		size_t index = music_next_index;
		a_string fname = music_playlist[index];
		a_vector<uint8_t> data;
		// Take the background-preloaded bytes if they are for THIS track -- no
		// main-thread MPQ decompress, so the switch doesn't stutter. Otherwise
		// (first track, or the preload didn't finish in time) load synchronously.
		bool used_preload = false;
		{
			std::lock_guard<std::mutex> lk(music_pf_mtx);
			if (music_pf_ready && music_pf_name == fname) {
				data.swap(music_pf_data);
				music_pf_ready = false;
				music_pf_name.clear();
				used_preload = true;
			}
		}
		if (!used_preload) {
			try {
				load_data_file(data, fname);
			} catch (const std::exception& e) {
				ui::log("music: load error for %s: %s\n", fname.c_str(), e.what());
				data.clear(); // unsupported compression etc. -> drop the track below
			} catch (...) {
				data.clear();
			}
		}
		music_next_index = (index + 1) % music_playlist.size();
		bool ok = !data.empty() && native_sound::play_music(data.data(), data.size(), (128 - 4) * music_volume / 100);
		ui::log("music: %s %s (%d bytes%s)\n", ok ? "playing" : "DROPPED", fname.c_str(), (int)data.size(), used_preload ? ", preloaded" : "");
		if (!ok) {
			// Undecodable track: drop it so we don't retry it every rotation
			// (and don't spin every frame if all tracks turn out bad).
			music_playlist.erase(music_playlist.begin() + index);
			if (music_playlist.empty()) music_next_index = 0;
			else music_next_index = index % music_playlist.size();
		} else if (!music_playlist.empty()) {
			// Stage the NEXT track in the background so its switch is seamless too.
			request_music_preload(music_playlist[music_next_index]);
		}
	}

	// --- SMK talking-head portraits --------------------------------------
	// Authentic unit portraits are Smacker videos (portrait/*.smk in the
	// MPQs), decoded with the vendored libsmacker. portdata.dat maps a unit's
	// portrait id to an idle ("fidget") and a talking video; the idle one
	// loops while a single unit is selected and the talking one plays once
	// whenever that unit voices a response.
	// portdata.dat: 220 records ({u32 tbl_index} x220, {u8 smk_change} x220,
	// {u8 unknown} x220). Records 0-109 are the idle ("fidget") videos and
	// record pid+110 is the talking video for portrait pid.
	a_vector<uint32_t> portrait_smk_index;
	string_table_data portraits_tbl;
	bool portraits_loaded = false;

	smk portrait_smk = nullptr;
	a_vector<uint8_t> portrait_smk_data; // must outlive portrait_smk (memory mode)
	unsigned long portrait_video_w = 0;
	unsigned long portrait_video_h = 0;
	unsigned long portrait_frame_count = 0;
	double portrait_usf = 66667.0; // microseconds per frame
	int portrait_current_id = -1;
	bool portrait_is_talking = false;
	bool portrait_load_failed = false;
	std::chrono::high_resolution_clock::time_point portrait_frame_time;

	// --- HD .webm portraits (Faze 3.5b) ----------------------------------
	// SC:R replaced the SMK heads with VP9 .webm (CASC). Decode is ~25 ms/frame
	// on the tablet (measured) - far too slow for the render thread - so a
	// detached worker decodes the whole idle clip to RGB off-thread and
	// publishes it through a shared slot; the render thread plays whatever is
	// ready and falls back to the SMK head until then (and if the webm is
	// missing). The worker captures only the shared slot + a private copy of the
	// bytes (never `this`), so it is safe even if the ui is torn down mid-decode;
	// spawned detached, so no join/terminate and no destructor needed. Pure
	// presentation; no determinism impact.
	struct hd_portrait_clip { int w = 0, h = 0; std::vector<std::vector<uint8_t>> frames; };
	struct hd_portrait_slot_t {
		std::mutex mtx;
		std::shared_ptr<hd_portrait_clip> clip; // published by worker, read by render
		std::atomic<bool> cancel{false};
	};
	std::shared_ptr<hd_portrait_slot_t> hd_portrait_slot; // current job (main thread ref)
	std::chrono::high_resolution_clock::time_point hd_portrait_play_t0;
	// Decoded-clip cache (main thread only): re-selecting a portrait must be
	// instant HD, not a 250 ms SMK (SD) flash + re-decode. Keyed by id*2+talking.
	std::map<uint64_t, std::shared_ptr<hd_portrait_clip>> hd_portrait_cache;
	size_t hd_portrait_cache_bytes = 0;
	uint64_t hd_portrait_cur_key = 0;
	bool hd_portrait_have_key = false;
	// True while an HD clip for the current portrait is expected (a webm was
	// found) but not yet decoded: the render then leaves the slot on its dark
	// background instead of flashing the SD SMK head. False => no HD webm for
	// this portrait, so the SMK head is the intended fallback.
	bool hd_portrait_expected = false;

	// Faze 4.7: SC:R static "channel-change" transition. When the portrait switches
	// to a DIFFERENT unit, snow (portrait/static/statout.webm then statin.webm,
	// 6 frames each @ 10 fps, no audio) plays over the slot -- which also hides the
	// ~250 ms bg decode of the new unit's clip. Generic clips: decoded once, kept.
	std::shared_ptr<hd_portrait_slot_t> static_out_slot, static_in_slot;
	std::shared_ptr<hd_portrait_clip> static_out_clip, static_in_clip;
	bool static_clips_kicked = false;
	bool portrait_transition = false;
	std::chrono::high_resolution_clock::time_point portrait_transition_t0;
	// Faze 4.7: procedural CRT look on the portrait slot (SC:R renders one; there is
	// no asset for it). Subtle scanline darkening + a slow rolling highlight band.
	bool portrait_crt = true;

	static size_t hd_clip_bytes(const hd_portrait_clip& c) {
		size_t b = 0; for (auto& f : c.frames) b += f.size(); return b;
	}

	// Faze 4.7: kick a one-off background decode of a portrait-sized webm into a
	// slot (mirrors set_portrait's worker; captures only the slot + a byte copy).
	void kick_portrait_clip(const a_string& name, std::shared_ptr<hd_portrait_slot_t>& slot) {
		if (!load_hd_bytes) return;
		std::vector<uint8_t> bytes;
		if (!load_hd_bytes(name, bytes) || bytes.empty()) return;
		slot = std::make_shared<hd_portrait_slot_t>();
		auto sl = slot;
		std::thread([sl, bytes = std::move(bytes)]() {
			webm::file_t doc = webm::parse(bytes.data(), bytes.size());
			if (!doc.ok || doc.frames.empty()) return;
			auto clip = std::make_shared<hd_portrait_clip>();
			int shown = 0, w = 0, h = 0;
			vp9::decode_sequence_hash(bytes.data(), bytes.size(), doc.frames, &shown,
				/*as_rgb=*/true, &clip->frames, &w, &h, &sl->cancel);
			if (sl->cancel.load() || clip->frames.empty() || w <= 0 || h <= 0) return;
			clip->w = w; clip->h = h;
			std::lock_guard<std::mutex> lk(sl->mtx); sl->clip = std::move(clip);
		}).detach();
	}

	// One-shot: decode the two generic static-transition clips (reused every switch).
	void ensure_static_clips() {
		if (static_clips_kicked || !hd_casc || !load_hd_bytes) return;
		static_clips_kicked = true;
		kick_portrait_clip(hd_casc_prefix + "portrait/static/statout.webm", static_out_slot);
		kick_portrait_clip(hd_casc_prefix + "portrait/static/statin.webm", static_in_slot);
	}

	a_string hd_portrait_webm_path(const a_string& tbl) const {
		// "tmarine\\TMaFid0" -> "<prefix>portrait/tmarine/tmafid00.webm"
		a_string s = tbl;
		for (auto& c : s) { if (c == '\\') c = '/'; else c = (char)tolower((unsigned char)c); }
		return hd_casc_prefix + "portrait/" + s + "0.webm";
	}

	void load_portraits() {
		portraits_loaded = true;
		if (!data_file_exists || !data_file_exists("arr/portdata.dat")) return;
		try {
			a_vector<uint8_t> dat;
			load_data_file(dat, "arr/portdata.dat");
			load_data_file(portraits_tbl.data, "arr/portdata.tbl");
			size_t n = dat.size() / 6; // {u32 tbl_index} xN, {u8 smk_change} xN, {u8 unknown} xN
			auto rd32 = [&](size_t off) {
				return (uint32_t)dat[off] | ((uint32_t)dat[off + 1] << 8) | ((uint32_t)dat[off + 2] << 16) | ((uint32_t)dat[off + 3] << 24);
			};
			portrait_smk_index.resize(n);
			for (size_t i = 0; i != n; ++i) portrait_smk_index[i] = rd32(i * 4);
			ui::log("portraits: %d records\n", (int)n);
		} catch (...) {
			portrait_smk_index.clear();
		}
	}

	void close_portrait() {
		if (portrait_smk) {
			smk_close(portrait_smk);
			portrait_smk = nullptr;
		}
		portrait_smk_data.clear();
		// Signal any in-flight HD decode to bail and drop our reference to its
		// slot; the detached worker (if still running) publishes to a slot no one
		// reads and then exits. The decoded-clip cache persists across portraits.
		if (hd_portrait_slot) hd_portrait_slot->cancel.store(true);
		hd_portrait_slot.reset();
		hd_portrait_expected = false;
		hd_portrait_have_key = false;
		// Faze 4.7 (measured): a Hide Portrait (briefing type 6) calls close_portrait,
		// and the FOLLOWING transmission must then default to the Adjutant. That only
		// works if "no portrait" is actually recorded -- so clear the current speaker
		// here. set_portrait() re-sets it immediately on its own close->assign path,
		// so gameplay/selection is unaffected.
		portrait_current_id = -1;
		portrait_is_talking = false;
	}

	size_t portrait_record_count() { return portrait_smk_index.size() / 2; }

	// Faze 4.6: one-shot HD portrait coverage audit against the (validated) CASC.
	// Walks the whole portdata roster (idle + talk records) and reports how many
	// resolve to an existing HD webm, so "full coverage" is MEASURED against the
	// real install rather than assumed from one sampled unit. Cheap: hd_file_exists
	// is a name lookup, no byte read. Runs once; no-op without CASC HD.
	bool hd_portrait_coverage_done = false;
	void hd_portrait_coverage_report() {
		if (hd_portrait_coverage_done) return;
		hd_portrait_coverage_done = true;
		if (!hd_casc || !hd_file_exists || portrait_smk_index.empty()) return;
		int found = 0, total = 0, miss = 0;
		a_string misses;
		for (size_t rec = 0; rec < portrait_smk_index.size(); ++rec) {
			size_t index = portrait_smk_index[rec];
			a_string fn = portraits_tbl[index];
			if (fn.empty()) continue;
			++total;
			if (hd_file_exists(hd_portrait_webm_path(fn))) ++found;
			else { if (miss < 14) { if (!misses.empty()) misses += ' '; misses += fn; } ++miss; }
		}
		ui::log("hdport coverage: %d/%d webms resolved (%d missing)%s%s\n",
			found, total, miss, miss ? "; e.g. " : "", misses.c_str());
	}

	void set_portrait(int id, bool talking) {
		close_portrait();
		portrait_current_id = id;
		portrait_is_talking = talking;
		portrait_load_failed = false;
		size_t half = portrait_record_count();
		if (id < 0 || (size_t)id >= half) return;
		size_t record = talking ? (size_t)id + half : (size_t)id;
		size_t index = portrait_smk_index[record];
		a_string fn = portraits_tbl[index];
		if (fn.empty()) { portrait_load_failed = true; return; }
		// Faze 3.5b: kick the HD .webm portrait decode off-thread. `fn` is already
		// the idle or talking variant name (record chosen by `talking` above), so
		// this path covers both - the talking clip loops for as long as the SMK
		// logic keeps talking, then poll_portrait's set_portrait(id,false) re-kicks
		// idle. SMK below still loads and plays instantly; the render swaps to the
		// HD head once the worker publishes (~250 ms later) and stays on SMK if the
		// webm is absent. close_portrait() (above) already dropped any prior slot.
		if (load_hd_bytes) {
			uint64_t key = (uint64_t)id * 2 + (talking ? 1 : 0);
			hd_portrait_cur_key = key;
			hd_portrait_have_key = true;
			hd_portrait_play_t0 = clock.now();
			auto ci = hd_portrait_cache.find(key);
			if (ci != hd_portrait_cache.end() && ci->second) {
				// Already decoded: publish instantly - no re-decode, no SD flash.
				auto slot = std::make_shared<hd_portrait_slot_t>();
				{ std::lock_guard<std::mutex> lk(slot->mtx); slot->clip = ci->second; }
				hd_portrait_slot = slot;
				hd_portrait_expected = true;
				ui::log("hdport: cache-hit id=%d talk=%d\n", id, (int)talking);
			} else {
				a_string name = hd_portrait_webm_path(fn);
				std::vector<uint8_t> bytes;
				// DIAG ASSET_SLOW_LOAD (PERF_HITCH pin): time the main-thread portrait webm
				// CASC fetch -- the other synchronous asset load that can block a frame.
				int64_t _pt0 = bwgame::diag::enabled()
					? std::chrono::duration_cast<std::chrono::microseconds>(prof_clock::now().time_since_epoch()).count() : 0;
				hd_portrait_expected = load_hd_bytes(name, bytes) && !bytes.empty();
				if (bwgame::diag::enabled() && hd_portrait_expected) {
					int64_t dt = std::chrono::duration_cast<std::chrono::microseconds>(
						prof_clock::now().time_since_epoch()).count() - _pt0;
					if (dt > 120000 && (uint32_t)st.current_frame > 300 && diag_once(15, (uint32_t)id))
						bwgame::diag::emit((long)st.current_frame, "asset", bwgame::diag::Sev::warn, "ASSET_SLOW_LOAD",
							"synchronous HD portrait fetch blocked the frame", "\"portrait\":" + std::to_string(id) +
							",\"us\":" + std::to_string(dt));
				}
				ui::log("hdport: %s id=%d talk=%d\n", hd_portrait_expected ? "decode" : "no-webm(SD)", id, (int)talking);
				if (hd_portrait_expected) {
					auto slot = std::make_shared<hd_portrait_slot_t>();
					hd_portrait_slot = slot;
					std::thread([slot, bytes = std::move(bytes)]() {
						webm::file_t doc = webm::parse(bytes.data(), bytes.size());
						if (!doc.ok || doc.frames.empty()) return;
						auto clip = std::make_shared<hd_portrait_clip>();
						int shown = 0, w = 0, h = 0;
						vp9::decode_sequence_hash(bytes.data(), bytes.size(), doc.frames, &shown,
							/*as_rgb=*/true, &clip->frames, &w, &h, &slot->cancel);
						if (slot->cancel.load() || clip->frames.empty() || w <= 0 || h <= 0) return;
						clip->w = w; clip->h = h;
						std::lock_guard<std::mutex> lk(slot->mtx);
						slot->clip = std::move(clip);
					}).detach();
				}
			}
		}
		// tbl strings look like "tmarine\TMaFid0"; the files on disk carry a
		// variant digit: portrait\tmarine\TMaFid00.smk (variant 0 always exists)
		a_string path = "portrait\\" + fn + "0.smk";
		if (data_file_exists && !data_file_exists(path)) {
			path = "portrait\\" + fn + ".smk"; // fallback: no variant digit
		}
		try {
			load_data_file(portrait_smk_data, path);
		} catch (...) {
			ui::log("portrait: failed to load '%s'\n", path.c_str());
			portrait_load_failed = true;
			return;
		}
		portrait_smk = smk_open_memory(portrait_smk_data.data(), (unsigned long)portrait_smk_data.size());
		if (!portrait_smk) { portrait_load_failed = true; return; }
		unsigned long cur_frame = 0;
		smk_info_all(portrait_smk, &cur_frame, &portrait_frame_count, &portrait_usf);
		unsigned char y_scale = 0;
		smk_info_video(portrait_smk, &portrait_video_w, &portrait_video_h, &y_scale);
		if (portrait_usf < 1000.0) portrait_usf = 66667.0;
		smk_enable_video(portrait_smk, 1);
		smk_first(portrait_smk);
		portrait_frame_time = clock.now();
	}

	// The unit whose portrait should be on screen: the single selected unit
	// (any owner, like BW), or the first own unit of a multi-selection.
	unit_t* portrait_unit() {
		// SCBW2 group-focus: a grid-tapped unit drives the portrait while the group
		// stays selected; else the first selected unit.
		unit_t* f = get_unit(focused_unit_id);
		if (f && current_selection_is_selected(f)) return f;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u) return u;
		}
		return nullptr;
	}

	// Move the current bg-decoded HD portrait clip into the cache (no selection
	// logic), so it survives re-selection. Returns true if a clip was harvested.
	// Used by poll_portrait and by hd_prewarm (which must NOT run the selection
	// override poll_portrait does).
	bool hd_portrait_harvest() {
		if (!(hd_portrait_have_key && hd_portrait_slot
		    && hd_portrait_cache.find(hd_portrait_cur_key) == hd_portrait_cache.end()))
			return false;
		std::shared_ptr<hd_portrait_clip> clip;
		{ std::lock_guard<std::mutex> lk(hd_portrait_slot->mtx); clip = hd_portrait_slot->clip; }
		if (!clip) return false;
		hd_portrait_cache[hd_portrait_cur_key] = clip;
		hd_portrait_cache_bytes += hd_clip_bytes(*clip);
		// Faze 4.7: 48 MB was too small once the pre-warm + a 4-speaker briefing
		// (4 idle + 4 talking clips) shared it -- early speakers got evicted, so a
		// re-speaker's talking clip was gone (re-decode -> looked idle) and Adjutant
		// froze. 160 MB holds a full briefing + pre-warm + gameplay heads at once;
		// the low-memory handler still drops the whole cache under real pressure.
		while (hd_portrait_cache_bytes > (160u << 20) && hd_portrait_cache.size() > 1) {
			auto it = hd_portrait_cache.begin();
			if (it->first == hd_portrait_cur_key) { ++it; if (it == hd_portrait_cache.end()) break; }
			if (it->second) hd_portrait_cache_bytes -= hd_clip_bytes(*it->second);
			hd_portrait_cache.erase(it);
		}
		return true;
	}

	void poll_portrait() {
		if (!portraits_loaded) load_portraits();
		if (portrait_smk_index.empty()) return;
		hd_portrait_coverage_report(); // Faze 4.6: one-shot roster coverage audit
		ensure_static_clips();         // Faze 4.7: pre-decode the static-transition clips
		if (!briefing_active) {
			int want = -1;
			bool want_talk = false;
			// A trigger Transmission/Talking Portrait overrides the selection
			// portrait until its duration elapses.
			if (trigger_portrait_id >= 0 && (int)st.current_frame < trigger_portrait_until) {
				want = trigger_portrait_id;
				want_talk = true;
			} else {
				trigger_portrait_id = -1;
				unit_t* u = portrait_unit();
				if (u) {
					int p = u->unit_type->portrait;
					if (p >= 0 && (size_t)p < portrait_record_count()) want = p;
				}
			}
			if (want != portrait_current_id) {
				// Faze 4.7: SC:R plays a static "snow" burst when the portrait jumps
				// to a DIFFERENT unit; it also masks the new clip's ~250 ms bg decode.
				// (idle<->talk of the SAME unit goes via portrait_talk(), not here.)
				if (hd_casc && want >= 0 && (static_out_clip || static_in_clip)) {
					portrait_transition = true;
					portrait_transition_t0 = clock.now();
				}
				set_portrait(want, want_talk);
			}
		}
		// Faze 4.7: harvest the generic static clips once the bg decode publishes.
		if (static_out_slot && !static_out_clip) { std::lock_guard<std::mutex> lk(static_out_slot->mtx); static_out_clip = static_out_slot->clip; }
		if (static_in_slot && !static_in_clip) { std::lock_guard<std::mutex> lk(static_in_slot->mtx); static_in_clip = static_in_slot->clip; }
		// Harvest a freshly decoded HD clip into the cache so re-selecting this
		// portrait is instant HD (no re-decode, no SD flash). Main thread only.
		hd_portrait_harvest();
		if (!portrait_smk) return;
		auto now = clock.now();
		auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - portrait_frame_time).count();
		if (elapsed < (int64_t)portrait_usf) return;
		portrait_frame_time = now;
		// char is unsigned on arm64, so force signed for the -1 error code
		int r = (signed char)smk_next(portrait_smk);
		if (r == SMK_ERROR || r == SMK_DONE || r == SMK_LAST) {
			if (portrait_is_talking) {
				// v0.83.025 (hardware feedback): in a briefing the spoken line
				// lasts as long as briefing_waiting is set (the Transmission's own
				// duration). The talking clip is shorter, so playing it once left
				// the head frozen for the rest of the line. Loop it for the whole
				// line, then drop to idle once the wait ends.
				if (briefing_active && briefing_waiting) smk_first(portrait_smk);
				else set_portrait(portrait_current_id, false); // back to idle loop
			}
			else if (r == SMK_ERROR) close_portrait();
			else smk_first(portrait_smk); // loop the idle video
		}
	}

	// Called from the voice-response path: flip the on-screen portrait to the
	// talking video while the unit speaks.
	void portrait_talk(const unit_t* u) {
		if (!u || portrait_smk_index.empty()) return;
		int p = u->unit_type->portrait;
		if (p != portrait_current_id) return;
		if (p < 0 || (size_t)p >= portrait_record_count()) return;
		if (portrait_is_talking) return;
		set_portrait(p, true);
	}

	// Scale the current SMK frame (palette-indexed) into the given rect,
	// nearest-neighbor, preserving the video's aspect ratio.
	// Blit the current HD portrait frame (RGB, 10 fps loop) fit-to-rect, nearest
	// scale - mirrors the SMK path's geometry.
	void draw_hd_portrait_frame(const hd_portrait_clip& c, uint32_t* data, size_t pitch, rect r) {
		int n = (int)c.frames.size();
		if (n == 0 || c.w <= 0 || c.h <= 0) return;
		auto now = clock.now();
		int64_t el = std::chrono::duration_cast<std::chrono::microseconds>(now - hd_portrait_play_t0).count();
		if (el < 0) el = 0;
		int idx = (int)((el / 100000) % n); // 100 ms/frame (10 fps)
		draw_clip_frame(c, idx, data, pitch, r);
	}

	// Blit one RGB clip frame fit-to-rect (nearest, aspect-preserved). Shared by
	// the HD portrait head and the Faze 4.7 static-transition frames.
	void draw_clip_frame(const hd_portrait_clip& c, int idx, uint32_t* data, size_t pitch, rect r) {
		int n = (int)c.frames.size();
		if (n == 0 || c.w <= 0 || c.h <= 0) return;
		if (idx < 0) idx = 0; if (idx >= n) idx = n - 1;
		const std::vector<uint8_t>& fr = c.frames[idx];
		if (fr.size() < (size_t)c.w * c.h * 3) return;
		int rw = r.to.x - r.from.x, rh = r.to.y - r.from.y;
		if (rw <= 0 || rh <= 0) return;
		int dw = rw, dh = (int)(rw * c.h / c.w);
		if (dh > rh) { dh = rh; dw = (int)(rh * c.w / c.h); }
		int x0 = r.from.x + (rw - dw) / 2, y0 = r.from.y + (rh - dh) / 2;
		for (int y = 0; y != dh; ++y) {
			int sy = (int)((int64_t)y * c.h / dh);
			int dy = y0 + y;
			if (dy < 0 || dy >= (int)screen_height) continue;
			uint32_t* row = data + dy * pitch;
			const uint8_t* src = fr.data() + (size_t)sy * c.w * 3;
			for (int x = 0; x != dw; ++x) {
				int dx = x0 + x;
				if (dx < 0 || dx >= (int)screen_width) continue;
				const uint8_t* p = src + (size_t)((int64_t)x * c.w / dw) * 3;
				row[dx] = 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
			}
		}
	}

	void draw_portrait_video(uint32_t* data, size_t pitch, rect r) {
		// Faze 4.7: a static "snow" burst overrides the head briefly on a unit
		// change (statout then statin, 6 frames each @ 10 fps), then the normal
		// head resumes. The CRT overlay is applied over whatever was drawn.
		bool drew_static = false;
		if (portrait_transition) {
			int64_t el = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - portrait_transition_t0).count();
			const int per = 600; // ms per clip (6 frames * 100 ms)
			if (el >= 2 * per) portrait_transition = false;
			else {
				hd_portrait_clip* sc = (el < per) ? static_in_clip.get() : static_out_clip.get();
				int64_t local = (el < per) ? el : (el - per);
				if (sc && !sc->frames.empty()) {
					draw_clip_frame(*sc, (int)((local / 100) % (int64_t)sc->frames.size()), data, pitch, r);
					drew_static = true;
				}
			}
		}
		if (!drew_static) draw_portrait_content(data, pitch, r);
		if (portrait_crt) draw_portrait_crt(data, pitch, r);
	}

	// Procedural CRT look for the portrait slot (Faze 4.7): scanline darkening plus
	// a slow rolling highlight band. UI-only (no determinism impact).
	void draw_portrait_crt(uint32_t* data, size_t pitch, rect r) {
		int x0 = r.from.x < 0 ? 0 : r.from.x, y0 = r.from.y < 0 ? 0 : r.from.y;
		int x1 = r.to.x > (int)screen_width ? (int)screen_width : r.to.x;
		int y1 = r.to.y > (int)screen_height ? (int)screen_height : r.to.y;
		if (x1 <= x0 || y1 <= y0) return;
		int H = y1 - y0;
		int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
		int band_h = H / 8; if (band_h < 4) band_h = 4;
		int band_y = (int)((ms / 24) % (int64_t)(H + band_h)) - band_h; // scrolls down
		int sl = hud_us(); if (sl < 1) sl = 1; // scanline pitch tracks the buffer scale
		for (int y = y0; y < y1; ++y) {
			uint32_t* row = data + (size_t)y * pitch;
			bool scan = ((((y - y0) / sl) % 3) == 0);        // darken 1 of every 3 lines
			int rel = (y - y0) - band_y, glow = 0;           // rolling highlight
			if (rel >= 0 && rel < band_h) {
				int d = rel - band_h / 2; if (d < 0) d = -d;
				glow = (band_h / 2 - d) * 22 / (band_h / 2 > 0 ? band_h / 2 : 1);
			}
			if (!scan && glow == 0) continue;
			for (int x = x0; x < x1; ++x) {
				uint32_t c = row[x];
				int rr = (c >> 16) & 0xff, gg = (c >> 8) & 0xff, bb = c & 0xff;
				if (scan) { rr = rr * 80 / 100; gg = gg * 80 / 100; bb = bb * 80 / 100; }
				if (glow) { rr += glow; gg += glow; bb += glow; if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255; }
				row[x] = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
			}
		}
	}

	void draw_portrait_content(uint32_t* data, size_t pitch, rect r) {
		// Faze 3.5b: prefer the HD .webm head once the bg decoder has published it.
		if (hd_portrait_slot) {
			std::shared_ptr<hd_portrait_clip> clip;
			{ std::lock_guard<std::mutex> lk(hd_portrait_slot->mtx); clip = hd_portrait_slot->clip; }
			if (clip && !clip->frames.empty()) { draw_hd_portrait_frame(*clip, data, pitch, r); return; }
		}
		// An HD head is coming for this portrait but has not decoded yet: leave the
		// slot on its dark background instead of flashing the SD SMK head. Only
		// fall through to SMK when there is no HD webm for this portrait.
		if (hd_portrait_expected) return;
		if (!portrait_smk) return;
		const unsigned char* pal = smk_get_palette(portrait_smk);
		const unsigned char* vid = smk_get_video(portrait_smk);
		if (!pal || !vid || !portrait_video_w || !portrait_video_h) return;
		int rw = r.to.x - r.from.x;
		int rh = r.to.y - r.from.y;
		if (rw <= 0 || rh <= 0) return;
		// fit-to-rect with aspect
		int dw = rw, dh = (int)(rw * portrait_video_h / portrait_video_w);
		if (dh > rh) { dh = rh; dw = (int)(rh * portrait_video_w / portrait_video_h); }
		int x0 = r.from.x + (rw - dw) / 2;
		int y0 = r.from.y + (rh - dh) / 2;
		for (int y = 0; y != dh; ++y) {
			int sy = (int)((int64_t)y * portrait_video_h / dh);
			int dy = y0 + y;
			if (dy < 0 || dy >= (int)screen_height) continue;
			uint32_t* row = data + dy * pitch;
			const unsigned char* src = vid + (size_t)sy * portrait_video_w;
			for (int x = 0; x != dw; ++x) {
				int dx = x0 + x;
				if (dx < 0 || dx >= (int)screen_width) continue;
				int si = src[(int)((int64_t)x * portrait_video_w / dw)];
				row[dx] = 0xff000000u | ((uint32_t)pal[si * 3] << 16) | ((uint32_t)pal[si * 3 + 1] << 8) | pal[si * 3 + 2];
			}
		}
	}

	// --- Trigger presentation (v0.20) --------------------------------------
	// The sim raises these hooks for every player's triggers; the UI keeps
	// only what concerns the local player (or everything in a replay).

	struct trigger_text_t {
		a_string text;
		int frame;
		// Lifetime in frames, carried per line: a wrapped transmission must
		// expire as one sentence, not line by line.
		int lifetime = 0;
	};
	a_vector<trigger_text_t> trigger_texts;
	int trigger_portrait_id = -1;
	int trigger_portrait_until = -1000000;

	// Map MPQ for map-embedded assets (trigger WAVs in .scm/.scx). Set by the
	// Android glue; opened lazily. Campaign missions instead set
	// map_data_prefix, resolved through the normal (mod-aware) MPQ chain.
	a_string map_mpq_path;
	a_string map_data_prefix;
	std::unique_ptr<data_loading::mpq_file<>> map_mpq;
	bool map_mpq_failed = false;

	bool trigger_concerns_local(int owner) const {
		if (!game_input) return true; // replay/observer: show everything
		return owner == game_local_player;
	}

	static a_string trigger_sanitize(const a_string& s) {
		a_string r;
		for (char c : s) {
			if ((unsigned char)c >= 0x20) r += c;
			else if (c == '\n' || c == '\r' || c == 0x12 || c == 0x13) r += ' ';
		}
		return r;
	}

	// v0.83.004: wrap to the screen. Campaign transmissions are whole spoken
	// sentences and were drawn from x to wherever they ended -- off the right
	// edge, so the mission dialogue read as if it had been cut mid-word. Splits
	// on spaces; a single unbroken word is cut, since something must give.
	a_vector<a_string> wrap_hud_text(const a_string& s) const {
		const int char_w = ui::unispace_w + 1;
		int avail = (int)screen_width - 32;
		size_t max_chars = avail > char_w * 8 ? (size_t)(avail / char_w) : (size_t)8;
		a_vector<a_string> out;
		size_t at = 0;
		while (s.size() - at > max_chars) {
			size_t cut = at + max_chars, space = cut;
			while (space > at && s[space] != ' ') --space;
			if (space == at) space = cut;
			out.push_back(s.substr(at, space - at));
			at = space < s.size() && s[space] == ' ' ? space + 1 : space;
		}
		out.push_back(s.substr(at));
		return out;
	}

	// v0.83.005: the map's own line breaks are LINE BREAKS, not spaces.
	// Measured (`aidump strings campaign/terran/terran01/... 'multiple units'`)
	// the tutorial's tip is stored as:
	//     "          TIP\r\n               You can select multiple units by
	//      clicking \r\n               and dragging a box around them.\r\n..."
	// -- CRLF plus a fifteen-space indent that lines the text up on a 640-wide
	// screen. trigger_sanitize turned each control byte into a space, so every
	// break became seventeen spaces in the middle of a sentence: the huge gaps
	// and the mid-word breaks in the user's screenshot were the map's layout
	// pushed through a flattener, not our wrapping. Now: break where the map
	// breaks, drop its indentation (our margin is not 640 pixels wide), and
	// wrap only what is still too long.
	// `wrap` = false leaves each of the map's lines whole, for a caller that wraps
	// to its own width (the briefing transcript: wrapping here to the screen and
	// there to the panel broke lines twice, leaving one- and two-word orphans).
	a_vector<a_string> split_and_wrap(const a_string& raw, bool wrap = true) const {
		a_vector<a_string> out;
		a_string cur;
		auto flush = [&]() {
			size_t a = 0, b = cur.size();
			while (a < b && cur[a] == ' ') ++a;
			while (b > a && cur[b - 1] == ' ') --b;
			a_string line = cur.substr(a, b - a);
			cur.clear();
			if (line.empty()) return;
			// Runs of spaces inside a line are column alignment for a screen we
			// are not; collapse them so words do not drift apart.
			a_string tidy;
			int spaces = 0;
			for (char c : line) {
				if (c == ' ') { if (++spaces > 1) continue; }
				else spaces = 0;
				tidy += c;
			}
			if (!wrap) { out.push_back(tidy); return; }
			for (auto& w : wrap_hud_text(tidy)) out.push_back(w);
		};
		for (char c : raw) {
			if (c == '\n') { flush(); continue; }
			if (c == '\r') continue;
			if ((unsigned char)c < 0x20) { cur += ' '; continue; }
			cur += c;
		}
		flush();
		return out;
	}

	virtual void on_trigger_display_text(int owner, const a_string& text) override {
		if (!trigger_concerns_local(owner)) return;
		auto lines = split_and_wrap(text);
		if (lines.empty()) return;
		// One transmission = one entry per line, all sharing a lifetime scaled
		// to the whole message, so a wrapped sentence never fades in pieces.
		int now = (int)st.current_frame;
		size_t total = 0;
		for (auto& l : lines) total += l.size();
		int life = trigger_text_lifetime(a_string(total, 'x'));
		for (auto& line : lines) trigger_texts.push_back({line, now, life});
		while (trigger_texts.size() > 10) trigger_texts.erase(trigger_texts.begin());
	}

	// v0.83.004: is a transmission still going? Used to hold the end of a
	// mission: the original lets the last line finish speaking before the
	// score screen, and cutting a character off mid-sentence is the one thing
	// that makes a campaign ending feel broken. Voice first, then text -- the
	// WAV is the authority, the text is just what is left on screen.
	bool transmission_active() {
		if (native_sound::is_playing((int)sound_channels.size())) return true;
		return !trigger_texts.empty();
	}

	// A transient HUD notice (reuses the trigger-text list + its fade/timeout).
	// Deduped so a held button / repeated failure doesn't spam the same line.
	int last_hud_msg_frame = -100000;
	a_string last_hud_msg;
	void show_hud_message(const char* text) {
		if (!text || !*text) return;
		int now = (int)st.current_frame;
		if (last_hud_msg == text && now - last_hud_msg_frame < 24) return; // ~1s dedupe
		last_hud_msg = text; last_hud_msg_frame = now;
		a_string t = trigger_sanitize(a_string(text));
		trigger_texts.push_back({t, now, trigger_text_lifetime(t)});
		while (trigger_texts.size() > 8) trigger_texts.erase(trigger_texts.begin());
	}

	// v0.28 action-error feedback: OpenBW leaves the "not enough ..." errors as a
	// todo (bwgame.h has_available_*), so surface them UI-side when the local
	// player taps an action they can't afford. Messages match the original game.
	const char* supply_error_for_race(race_t r) {
		if (r == race_t::protoss) return "You must construct additional pylons.";
		if (r == race_t::zerg) return "You must spawn more overlords.";
		return "You must construct additional supply depots.";
	}
	// BW resource/warning sounds are the per-race spoken "advisor" voice lines
	// (sound/<race>/advisor/<x>aderr0N.wav): 00 minerals, 01 gas, 02 supply,
	// 06 under attack. The line spoken follows the local player's race, exactly
	// like the original game. Falls back to Terran if the race is unresolved.
	static constexpr int ADV_MINERALS = 0, ADV_GAS = 1, ADV_SUPPLY = 2;
	// v0.83.002: "under attack" used to be ADV_UNDER_ATTACK = 6, i.e. aderr06 --
	// a GUESS, and a wrong one: the user heard "not enough energy". There are
	// two advisor families, and only one of them is errors:
	//   aderr NN  -- refusals   (00 minerals, 01 gas, 02 supply, 06 energy)
	//   adupd NN  -- updates    (04 = nuclear launch, verified: bwgame.h:6308
	//                            plays sfx 127 = ZAdUpd04 with add_race_index)
	// An attack notice is an update, so it cannot live in the aderr family at
	// all. Which update index carries "forces" versus "base" is inferred from
	// the family layout (00/01 are the only pair present for all three races)
	// and is the one part of this still owed an ear.
	static constexpr int ADV_UPD_FORCES_ATTACK = 0, ADV_UPD_BASE_ATTACK = 1;
	a_string advisor_path(const char* family, int idx) {
		int p = game_local_player;
		race_t r = (p >= 0 && p < 12) ? st.players[p].race : race_t::terran;
		const char* folder; char letter;
		if (r == race_t::zerg) { folder = "zerg"; letter = 'z'; }
		else if (r == race_t::protoss) { folder = "protoss"; letter = 'p'; }
		else { folder = "terran"; letter = 't'; }
		char buf[64];
		snprintf(buf, sizeof(buf), "sound/%s/advisor/%c%s%02d.wav", folder, letter, family, idx);
		return a_string(buf);
	}
	a_string advisor_wav(int idx) { return advisor_path("aderr", idx); }
	a_string advisor_update_wav(int idx) { return advisor_path("adupd", idx); }

	bool check_afford_unit(const unit_type_t* ut) {
		int p = game_local_player;
		if (p < 0 || !ut) return true;
		if (st.current_minerals[p] < ut->mineral_cost) { hud_error("Not enough minerals.", advisor_wav(ADV_MINERALS)); return false; }
		if (st.current_gas[p] < ut->gas_cost) { hud_error("Not enough vespene gas.", advisor_wav(ADV_GAS)); return false; }
		if (!has_available_supply_for(p, ut)) { hud_error(supply_error_for_race(st.players[p].race), advisor_wav(ADV_SUPPLY)); return false; }
		return true;
	}
	bool check_afford_tech(const tech_type_t* t) {
		int p = game_local_player;
		if (p < 0 || !t) return true;
		if (st.current_minerals[p] < t->mineral_cost) { hud_error("Not enough minerals.", advisor_wav(ADV_MINERALS)); return false; }
		if (st.current_gas[p] < t->gas_cost) { hud_error("Not enough vespene gas.", advisor_wav(ADV_GAS)); return false; }
		return true;
	}
	bool check_afford_upgrade(const upgrade_type_t* up) {
		int p = game_local_player;
		if (p < 0 || !up) return true;
		if (st.current_minerals[p] < upgrade_mineral_cost(p, up)) { hud_error("Not enough minerals.", advisor_wav(ADV_MINERALS)); return false; }
		if (st.current_gas[p] < upgrade_gas_cost(p, up)) { hud_error("Not enough vespene gas.", advisor_wav(ADV_GAS)); return false; }
		return true;
	}

	bool load_trigger_wav_data(const a_string& name, a_vector<uint8_t>& data) {
		if (name.empty()) return false;
		a_string fn = name;
		for (auto& c : fn) if (c == '\\') c = '/';
		// Every read here is guarded. A briefing WAV that exists but fails to
		// decode used to throw straight out of briefing_step(), through
		// m.update(), into android_main's catch -- and the whole mission
		// refused to start ("nepavyko paleisti: data_reader: attempt to read
		// past end", Backwater Station). A sound that will not load is worth
		// silence, never a dead mission.
		if (!map_data_prefix.empty() && data_file_exists && data_file_exists(map_data_prefix + fn)) {
			try {
				load_data_file(data, map_data_prefix + fn);
				return true;
			} catch (const std::exception& e) {
				ui::log("trigger wav: '%s%s' failed to load (%s)\n", map_data_prefix.c_str(), fn.c_str(), e.what());
				return false;
			}
		}
		if (!map_mpq && !map_mpq_failed && !map_mpq_path.empty()) {
			try {
				map_mpq = std::make_unique<data_loading::mpq_file<>>(map_mpq_path);
			} catch (...) {
				map_mpq_failed = true;
			}
		}
		if (map_mpq) {
			a_string mpq_name = name;
			for (auto& c : mpq_name) if (c == '/') c = '\\';
			try {
				if (map_mpq->mpq.file_exists(mpq_name)) {
					(*map_mpq)(data, mpq_name);
					return true;
				}
			} catch (...) {}
		}
		if (data_file_exists && data_file_exists(fn)) {
			try {
				load_data_file(data, fn);
				return true;
			} catch (const std::exception& e) {
				ui::log("trigger wav: '%s' failed to load (%s)\n", fn.c_str(), e.what());
				return false;
			}
		}
		return false;
	}

	std::unique_ptr<native_sound::sound> trigger_wav_sound;
	std::unique_ptr<native_sound::sound> ui_wav_sound;

	// Plays a named WAV from the MPQ chain on the dedicated voice channel
	// (used for UI feedback like resource-shortage warnings).
	void play_ui_wav(const char* filename) {
		if (global_volume == 0) return;
		a_vector<uint8_t> data;
		if (!load_trigger_wav_data(a_string(filename), data)) return;
		ui_wav_sound = native_sound::load_wav(data.data(), data.size());
		if (ui_wav_sound)
			native_sound::play((int)sound_channels.size(), &*ui_wav_sound, (128 - 4) * global_volume / 100 * ui_volume / 100, 0);
	}

	// A HUD error notice with its characteristic BW sound. The sound plays only
	// when the message is fresh (same dedup window as show_hud_message) so a
	// held button doesn't machine-gun the voice line.
	void hud_error(const char* text, const a_string& wav) {
		int now = (int)st.current_frame;
		bool fresh = !(last_hud_msg == text && now - last_hud_msg_frame < 24);
		show_hud_message(text);
		if (fresh && !wav.empty()) play_ui_wav(wav.c_str());
	}

	virtual void on_trigger_play_wav(int owner, const a_string& filename) override {
		if (!trigger_concerns_local(owner)) return;
		if (global_volume == 0) return;
		a_vector<uint8_t> data;
		if (!load_trigger_wav_data(filename, data)) return;
		trigger_wav_sound = native_sound::load_wav(data.data(), data.size());
		if (trigger_wav_sound) {
			// Dedicated channel (one past the SFX pool) so unit noise never
			// evicts a mission voice line. Mission voice lines follow the UI submix.
			native_sound::play((int)sound_channels.size(), &*trigger_wav_sound, (128 - 4) * global_volume / 100 * ui_volume / 100, 0);
		}
	}

	virtual void on_trigger_center_view(int owner, xy pos) override {
		if (!game_input || owner != game_local_player) return;
		screen_pos = xy(pos.x - (int)screen_width / 2, pos.y - (int)playfield_view_height() / 2);
	}

	// --- v0.26 Cinematics (smk\*.smk from the CD archives) ----------------
	// Full-screen modal playback with audio: the SMK's audio track is
	// unpacked into one WAV up front (pass 1) and played on the mission
	// voice channel while pass 2 renders frames at the native frame rate.
	// Any tap skips. Returns false if the file is missing (silently - not
	// every install has the CD archives).
	// Faze 4.7 Variant B: full-screen HD WebM cinematic via hardware decode. Maps
	// the classic smk\<name>.smk path to <prefix>smk/<name>.webm in CASC, decodes
	// on the platform codec, and presents through the same letterbox loop as SMK.
	// Video only for now (music is silenced). Returns false (untouched) if there is
	// no HW decoder, no CASC, or no matching webm -- the caller then tries SMK.
	bool play_hd_cinematic(const a_string& path_in) {
		if (!hd_cine_open || !hd_casc || !load_hd_bytes) {
			ui::log("cine: guard open=%d casc=%d load=%d\n", (int)(bool)hd_cine_open, (int)hd_casc, (int)(bool)load_hd_bytes);
			return false;
		}
		a_string name = path_in;
		for (auto& c : name) { if (c == '\\') c = '/'; else c = (char)tolower((unsigned char)c); }
		if (name.size() >= 4 && name.substr(name.size() - 4) == ".smk") name = name.substr(0, name.size() - 4);
		a_string base = name; // "smk/<x>"
		name = hd_casc_prefix + name + ".webm";
		std::vector<uint8_t> bytes;
		bool got = load_hd_bytes(name, bytes);
		// Cinematics are full-screen; the logo/some clips exist only at full res,
		// so fall back to the unprefixed smk/ path when the tier-prefixed one is
		// absent (HW decodes any resolution regardless of the graphics tier).
		if ((!got || bytes.empty()) && !hd_casc_prefix.empty()) {
			a_string full = base + ".webm";
			got = load_hd_bytes(full, bytes);
			if (got && !bytes.empty()) name = full;
		}
		ui::log("cine: load %s -> %s (%d bytes)\n", name.c_str(), (got && !bytes.empty()) ? "ok" : "FAIL", (int)bytes.size());
		if (!got || bytes.empty()) return false;
		// Faze 4.7 Variant A: hand the WebM to the OS video player (VideoView) when
		// available -- it decodes VP9 + Vorbis and handles device-specific (UBWC/
		// tiled) output internally, so it is robust across all SoCs and gives audio
		// + A/V sync for free. Blocks until the clip ends or is skipped. We silence
		// the game music while it owns the screen (the clip carries its own audio).
		if (hd_cine_play_java) {
			int saved_music_volume = music_volume;
			set_music_volume(0);
			bool ok = hd_cine_play_java(bytes);
			set_music_volume(saved_music_volume);
			ui::log("cine: java player %s -> %s\n", name.c_str(), ok ? "ok" : "fallback");
			if (ok) return true;
		}
		int w = 0, h = 0; double usf = 41667.0;
		void* dec = hd_cine_open(bytes, w, h, usf);
		if (!dec) { ui::log("cine: no HW decoder for %s (fallback SMK)\n", name.c_str()); return false; }
		if (usf < 1000.0) usf = 41667.0;
		ui::log("cine: HW playing %s %dx%d\n", name.c_str(), w, h);
		int saved_music_volume = music_volume;
		set_music_volume(0);
		auto frame_time = clock.now();
		bool skipped = false;
		std::vector<uint8_t> rgb;
		int nframes = 0; const char* reason = "eof";
		while (!skipped && !window_closed) {
			if (!hd_cine_next(dec, rgb)) { reason = "eof"; break; }
			if (w <= 0 || h <= 0 || rgb.size() < (size_t)w * h * 3) { reason = "size"; break; }
			++nframes;
			uint32_t* dst = (uint32_t*)rgba_surface->lock();
			if (!dst) break;
			size_t pitch = rgba_surface->pitch / 4;
			int dw = (int)screen_width, dh = (int)((int64_t)screen_width * h / w);
			if (dh > (int)screen_height) { dh = (int)screen_height; dw = (int)((int64_t)screen_height * w / h); }
			int x0 = ((int)screen_width - dw) / 2, y0 = ((int)screen_height - dh) / 2;
			std::fill_n(dst, pitch * screen_height, 0xff000000u);
			for (int y = 0; y != dh; ++y) {
				const uint8_t* src = rgb.data() + (size_t)((int64_t)y * h / dh) * w * 3;
				uint32_t* row = dst + (size_t)(y0 + y) * pitch + x0;
				for (int x = 0; x != dw; ++x) {
					const uint8_t* p = src + (size_t)((int64_t)x * w / dw) * 3;
					row[x] = 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
				}
			}
			rgba_surface->unlock();
			if (wnd) {
				wnd.update_surface();
				native_window::event_t e;
				while (wnd.peek_message(e)) {
					if (e.type == native_window::event_t::type_mouse_button_down) skipped = true;
					if (e.type == native_window::event_t::type_quit) { skipped = true; window_closed = true; }
				}
			}
			frame_time += std::chrono::microseconds((int64_t)usf);
			auto now = clock.now();
			if (frame_time > now) std::this_thread::sleep_for(frame_time - now);
		}
		hd_cine_close(dec);
		set_music_volume(saved_music_volume);
		ui::log("cine: presented %d frames (exit: %s%s)\n", nframes, skipped ? "skipped" : reason, window_closed ? "+closed" : "");
		// No frames => the HW path could not produce readable video (e.g. opaque
		// decoder output); report failure so the caller falls back to SMK/static.
		return nframes > 0;
	}

	bool play_cinematic(const a_string& path_in) {
		if (!rgba_surface || !wnd) return false; // needs the window up
		// Faze 4.7 Variant B: prefer the HD WebM cinematic (hardware decode).
		if (play_hd_cinematic(path_in)) return true;
		a_string mpq_path = path_in;
		for (auto& c : mpq_path) if (c == '/') c = '\\'; // MPQ open wants '\'
		if (!(data_file_exists && data_file_exists(mpq_path))) return false;
		a_vector<uint8_t> data;
		try {
			load_data_file(data, mpq_path);
		} catch (...) {
			return false;
		}
		smk s = smk_open_memory(data.data(), (unsigned long)data.size());
		if (!s) return false;
		unsigned long cur = 0, frames = 0;
		double usf = 66667.0;
		smk_info_all(s, &cur, &frames, &usf);
		if (usf < 1000.0) usf = 66667.0;
		unsigned long vw = 0, vh = 0;
		unsigned char ys = 0;
		smk_info_video(s, &vw, &vh, &ys);
		if (!vw || !vh || !frames) { smk_close(s); return false; }
		// pass 1: audio -> WAV
		unsigned char a_tracks = 0, a_channels[7] = {}, a_depth[7] = {};
		unsigned long a_rate[7] = {};
		smk_info_audio(s, &a_tracks, a_channels, a_depth, a_rate);
		std::unique_ptr<native_sound::sound> cine_sound;
		if (a_tracks & 1) {
			smk_enable_audio(s, 0, 1);
			a_vector<uint8_t> pcm;
			if ((signed char)smk_first(s) >= 0) {
				do {
					unsigned long n = smk_get_audio_size(s, 0);
					const unsigned char* p = smk_get_audio(s, 0);
					if (n && p) pcm.insert(pcm.end(), p, p + n);
				} while ((signed char)smk_next(s) == SMK_MORE);
			}
			if (!pcm.empty()) {
				int ch = a_channels[0] ? a_channels[0] : 1;
				int depth = a_depth[0] ? a_depth[0] : 16;
				int rate = (int)(a_rate[0] ? a_rate[0] : 22050);
				a_vector<uint8_t> wav(44 + pcm.size());
				uint8_t* h = wav.data();
				auto w32 = [&](size_t off, uint32_t v) { memcpy(h + off, &v, 4); };
				auto w16 = [&](size_t off, uint16_t v) { memcpy(h + off, &v, 2); };
				memcpy(h, "RIFF", 4);
				w32(4, (uint32_t)(36 + pcm.size()));
				memcpy(h + 8, "WAVEfmt ", 8);
				w32(16, 16);
				w16(20, 1);
				w16(22, (uint16_t)ch);
				w32(24, (uint32_t)rate);
				w32(28, (uint32_t)(rate * ch * depth / 8));
				w16(32, (uint16_t)(ch * depth / 8));
				w16(34, (uint16_t)depth);
				memcpy(h + 36, "data", 4);
				w32(40, (uint32_t)pcm.size());
				memcpy(wav.data() + 44, pcm.data(), pcm.size());
				cine_sound = native_sound::load_wav(wav.data(), wav.size());
			}
			smk_enable_audio(s, 0, 0);
		}
		// pass 2: video
		smk_enable_video(s, 1);
		if ((signed char)smk_first(s) < 0) { smk_close(s); return false; }
		int saved_music_volume = music_volume;
		set_music_volume(0); // silence music under the cinematic
		if (cine_sound) native_sound::play((int)sound_channels.size(), &*cine_sound, (128 - 4) * global_volume / 100, 0);
		auto frame_time = clock.now();
		bool skipped = false;
		for (unsigned long fi = 0; fi != frames && !skipped && !window_closed; ++fi) {
			const unsigned char* pal = smk_get_palette(s);
			const unsigned char* vid = smk_get_video(s);
			uint32_t* dst = (uint32_t*)rgba_surface->lock();
			if (!dst) break;
			size_t pitch = rgba_surface->pitch / 4;
			// letterbox fit
			int dw = (int)screen_width, dh = (int)(screen_width * vh / vw);
			if (dh > (int)screen_height) { dh = (int)screen_height; dw = (int)(screen_height * vw / vh); }
			int x0 = ((int)screen_width - dw) / 2, y0 = ((int)screen_height - dh) / 2;
			std::fill_n(dst, pitch * screen_height, 0xff000000u);
			if (pal && vid) {
				for (int y = 0; y != dh; ++y) {
					const unsigned char* src = vid + (size_t)((int64_t)y * vh / dh) * vw;
					uint32_t* row = dst + (size_t)(y0 + y) * pitch + x0;
					for (int x = 0; x != dw; ++x) {
						int si = src[(int64_t)x * vw / dw];
						row[x] = 0xff000000u | ((uint32_t)pal[si * 3] << 16) | ((uint32_t)pal[si * 3 + 1] << 8) | pal[si * 3 + 2];
					}
				}
			}
			rgba_surface->unlock();
			if (wnd) {
				wnd.update_surface();
				native_window::event_t e;
				while (wnd.peek_message(e)) {
					if (e.type == native_window::event_t::type_mouse_button_down) skipped = true;
					if (e.type == native_window::event_t::type_quit) { skipped = true; window_closed = true; }
				}
			}
			frame_time += std::chrono::microseconds((int64_t)usf);
			auto now = clock.now();
			if (frame_time > now) std::this_thread::sleep_for(frame_time - now);
			if ((signed char)smk_next(s) != SMK_MORE) break;
		}
		smk_close(s);
		if (cine_sound) native_sound::stop((int)sound_channels.size());
		set_music_volume(saved_music_volume);
		return true;
	}

	// v0.34: an in-engine after-action score screen, replacing the jarring
	// SDL system message box. Renders a Victory/Defeat banner plus a per-player
	// stats table, and blocks (its own present+input loop) until the player
	// taps to continue. Mirrors play_cinematic's surface/present pattern.
	void present_score_screen(bool won, int local_player) {
		if (!rgba_surface || !wnd) return;
		static const char* rn[] = {"Zerg", "Terran", "Protoss"};
		int sw = (int)screen_width, sh = (int)screen_height;
		bool done = false;
		int shown_frames = 0;
		while (!done && !window_closed) {
			uint32_t* dst = (uint32_t*)rgba_surface->lock();
			if (!dst) break;
			size_t pitch = rgba_surface->pitch / 4;
			std::fill_n(dst, pitch * sh, 0xff0a0a12u);
			// Banner.
			const char* title = won ? L("VICTORY!") : L("DEFEAT!");
			uint32_t tcol = won ? 0xffffd040u : 0xffff5050u;
			int tscale = 6;
			int tw = (int)strlen(title) * (ui::unispace_w + 1) * tscale;
			draw_text_rgba(dst, pitch, sw / 2 - tw / 2, sh / 12, title, tcol, tscale);
			// Local player summary.
			int y = sh / 12 + ui::unispace_h * tscale + sh / 20;
			int p = local_player;
			if (p >= 0 && p < 12) {
				int seconds = (int)(st.current_frame * 42 / 1000);
				int64_t kills = 0, losses = 0;
				for (size_t i = 0; i != 228; ++i) {
					kills += st.unit_kills[p][(UnitTypes)i];
					losses += st.unit_deaths[p][(UnitTypes)i];
				}
				char buf[160];
				snprintf(buf, sizeof(buf), "Laikas %d:%02d   Rezultatas %d",
					seconds / 60, seconds % 60, (int)st.kills_score[p] + (int)st.razings_score[p]);
				draw_text_rgba(dst, pitch, sw / 8, y, buf, 0xffe8e8e8u, 3); y += ui::unispace_h * 3 + 10;
				snprintf(buf, sizeof(buf), "Sunaikinta unitu %lld   Prarasta %lld   Sugriauta pastatu %d",
					(long long)kills, (long long)losses, (int)st.razings_score[p]);
				draw_text_rgba(dst, pitch, sw / 8, y, buf, 0xffc8c8c8u, 2); y += ui::unispace_h * 2 + 8;
				snprintf(buf, sizeof(buf), "Pagaminta unitu %d   pastatu %d   Surinkta %d min / %d duju",
					(int)st.total_non_buildings_ever_completed[p], (int)st.total_buildings_ever_completed[p],
					(int)st.total_minerals_gathered[p], (int)st.total_gas_gathered[p]);
				draw_text_rgba(dst, pitch, sw / 8, y, buf, 0xffc8c8c8u, 2); y += ui::unispace_h * 2 + sh / 24;
			}
			// Per-player comparison table.
			draw_text_rgba(dst, pitch, sw / 8, y, "--- Zaidejai ---", 0xff90c8e0u, 2);
			y += ui::unispace_h * 2 + 10;
			for (int i = 0; i < 12; ++i) {
				auto c = st.players[i].controller;
				if (c != player_t::controller_occupied && c != player_t::controller_computer &&
				    c != player_t::controller_computer_game) continue;
				size_t rc = (size_t)st.players[i].race; if (rc > 2) rc = 1;
				char pl[192];
				snprintf(pl, sizeof(pl), "P%d %s%s   rezultatas %d   sugriauta %d   res %d/%d",
					i, rn[rc], i == p ? " (tu)" : "", (int)st.kills_score[i], (int)st.razings_score[i],
					(int)st.total_minerals_gathered[i], (int)st.total_gas_gathered[i]);
				draw_text_rgba(dst, pitch, sw / 8, y, pl, i == p ? 0xffffe090u : 0xffb0b0b0u, 2);
				y += ui::unispace_h * 2 + 6;
			}
			// Continue hint (blinking).
			if ((shown_frames / 20) & 1) {
				const char* hint = "Palieskite ekrana, kad testumete";
				int hw = (int)strlen(hint) * (ui::unispace_w + 1) * 2;
				draw_text_rgba(dst, pitch, sw / 2 - hw / 2, sh - sh / 10, hint, 0xffa0d0a0u, 2);
			}
			rgba_surface->unlock();
			wnd.update_surface();
			native_window::event_t e;
			while (wnd.peek_message(e)) {
				// Ignore the first ~0.5s of input so a late in-game tap doesn't skip it.
				if (shown_frames > 12 && (e.type == native_window::event_t::type_mouse_button_down ||
				    e.type == native_window::event_t::type_key_down)) done = true;
				if (e.type == native_window::event_t::type_quit) { done = true; window_closed = true; }
			}
			++shown_frames;
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
	}

	// --- v0.26 Mission briefing (CHK MBRF) --------------------------------
	// The Android glue parses the local player's MBRF triggers into a flat
	// action list (strings already resolved); the game stays frozen while
	// briefing_active and this runner plays the sequence: texts accumulate
	// like a transcript, portraits use the SMK system, WAVs the trigger
	// voice channel. Tap = skip current wait, START button = begin mission.
	struct briefing_action_t {
		int type;      // MBRF action type (1 wait, 2 wav, 3 text, 4 objectives, 5/6/7 portrait)
		int time_ms;
		int unit;      // portrait unit type
		int slot = 0;  // portrait WINDOW slot (0-3): which of the four cells this
		               // action addresses. Show places a portrait in a slot; Speak/
		               // Transmission make the portrait ALREADY in that slot talk.
		a_string text; // resolved string (text/objectives)
		a_string wav;  // resolved wav path
	};
	a_vector<briefing_action_t> briefing_actions;
	bool briefing_active = false;
	// Faze 6: hold the briefing state machine (no stepping, no audio, no wall-clock
	// wait accrual) until the initial prewarm/loading finishes. The briefing timeline
	// is steady_clock-based, and hd_prewarm blocks the loop ~several seconds; without
	// this the one briefing_step in the first frame's draw would start the opening
	// dialogue, and the blocked wall-clock would then expire every timed wait at once
	// -> the briefing "fast-forwards" and appears to already be playing under the
	// loading screen. android_main sets this true at briefing setup and clears it
	// right after the first hd_prewarm returns, so the briefing plays start-to-finish
	// AFTER loading. Universal across all campaigns (same code path).
	bool briefing_hold = false;
	// Faze 6: the active speaker talks only while their WAV actually plays, then
	// returns to idle -- as in StarCraft. The briefing wait (time_ms) is usually
	// LONGER than the spoken line, so keying "talking" off the wait left a head
	// mouthing silence for many seconds ("never stops talking"). This is the WAV's
	// end time; draw_briefing_rgba flips the live head back to idle once reached.
	std::chrono::steady_clock::time_point briefing_talk_until{};
	// Duration of a RIFF/WAVE clip in ms (data bytes / byte-rate); 0 if unparsable.
	static int64_t wav_duration_ms(const uint8_t* d, size_t n) {
		if (n < 44 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return 0;
		uint32_t byte_rate = 0, data_size = 0;
		size_t p = 12;
		while (p + 8 <= n) {
			uint32_t csz; std::memcpy(&csz, d + p + 4, 4);
			if (std::memcmp(d + p, "fmt ", 4) == 0 && p + 8 + 16 <= n) {
				std::memcpy(&byte_rate, d + p + 8 + 8, 4); // avg bytes/sec at fmt+16
			} else if (std::memcmp(d + p, "data", 4) == 0) {
				data_size = csz;
				if (p + 8 + (size_t)data_size > n) data_size = (uint32_t)(n - (p + 8));
				break;
			}
			p += 8 + csz + (csz & 1);
		}
		if (!byte_rate) return 0;
		return (int64_t)data_size * 1000 / byte_rate;
	}
	size_t briefing_pos = 0;
	bool briefing_waiting = false;
	std::chrono::steady_clock::time_point briefing_wait_until;
	a_string briefing_objectives;
	a_vector<a_string> briefing_lines;

	// Faze 4.7 (user request): the briefing shows up to four speakers, each in its
	// OWN cell. A distinct speaker (by portrait id) claims the next free cell the
	// first time it appears and keeps it for the whole briefing; the currently
	// speaking one shows the live head, the others a captured still (their last
	// frame). Example (Revolution): Raynor c1, Kerrigan c2, Mengsk c3, Adjutant c4.
	struct briefing_cell_t { int portrait_id = -1; a_vector<uint32_t> still; int w = 0, h = 0; };
	briefing_cell_t briefing_cells[4];
	int briefing_cell_count = 0;
	// Faze 6b: the MBRF `slot` (0-3) IS the window index -- ShowPortrait places a
	// portrait in a slot, and Transmission/DisplaySpeaking name the slot whose
	// portrait should talk. Cells are indexed BY SLOT (not first-appearance); this is
	// the slot currently speaking (its cell shows the live head). The old model drove
	// the live head off "the last portrait shown", lighting the wrong speaker whenever
	// a briefing re-addresses an earlier slot out of order (every terran02/05/06/07
	// briefing does) -- the "Adjutant speaks for Mengsk" bug.
	int briefing_speaking_slot = -1;
	void briefing_reset_cells() { for (auto& c : briefing_cells) c = briefing_cell_t(); briefing_cell_count = 0; briefing_speaking_slot = -1; }
	// ShowPortrait: place `unit`'s portrait into window `slot` (does not speak).
	void briefing_slot_set(int slot, int unit_id) {
		if (slot < 0 || slot > 3) return;
		if (!portraits_loaded) load_portraits();
		if (unit_id <= 0 || unit_id >= 228) return;
		auto* ut = get_unit_type((UnitTypes)unit_id);
		if (!ut) return;
		int p = ut->portrait;
		if (p < 0 || (size_t)p >= portrait_record_count()) return;
		briefing_cells[slot].portrait_id = p;
		if (slot + 1 > briefing_cell_count) briefing_cell_count = slot + 1;
		set_portrait(p, false); // warm the idle head for this slot
	}
	// Transmission / DisplaySpeaking: make the portrait already in `slot` talk. The
	// action's unit is a placeholder (Terran Marine, 0) in every measured campaign, so
	// the speaker is the slot's portrait; a non-zero unit overrides it, and an empty
	// slot falls back to the Adjutant (Command Center).
	void briefing_slot_talk(int slot, int unit_id) {
		if (slot < 0 || slot > 3) slot = briefing_speaking_slot >= 0 ? briefing_speaking_slot : 0;
		int p = -1;
		if (unit_id > 0 && unit_id < 228) { auto* ut = get_unit_type((UnitTypes)unit_id); if (ut) p = ut->portrait; }
		if (p < 0) p = briefing_cells[slot].portrait_id;
		if (p < 0) { auto* ut = get_unit_type(UnitTypes::Terran_Command_Center); if (ut) p = ut->portrait; }
		if (p < 0 || (size_t)p >= portrait_record_count()) return;
		briefing_cells[slot].portrait_id = p;
		if (slot + 1 > briefing_cell_count) briefing_cell_count = slot + 1;
		briefing_speaking_slot = slot;
		set_portrait(p, true);
	}
	// Snapshot the just-drawn head region into the cell's still (freshest frame).
	void briefing_capture_still(briefing_cell_t& c, uint32_t* data, size_t pitch, rect r) {
		int w = r.to.x - r.from.x, h = r.to.y - r.from.y;
		if (w <= 0 || h <= 0) return;
		c.w = w; c.h = h; c.still.assign((size_t)w * h, 0);
		for (int y = 0; y < h; ++y) {
			int sy = r.from.y + y; if (sy < 0 || sy >= (int)screen_height) continue;
			const uint32_t* srow = data + (size_t)sy * pitch;
			for (int x = 0; x < w; ++x) {
				int sx = r.from.x + x; if (sx < 0 || sx >= (int)screen_width) continue;
				c.still[(size_t)y * w + x] = srow[sx];
			}
		}
	}
	void briefing_blit_still(const briefing_cell_t& c, uint32_t* data, size_t pitch, rect r) {
		if (c.still.empty() || c.w <= 0 || c.h <= 0) return;
		int w = r.to.x - r.from.x, h = r.to.y - r.from.y;
		if (w <= 0 || h <= 0) return;
		for (int y = 0; y < h; ++y) {
			int dy = r.from.y + y; if (dy < 0 || dy >= (int)screen_height) continue;
			const uint32_t* srow = &c.still[(size_t)(y * c.h / h) * c.w];
			uint32_t* drow = data + (size_t)dy * pitch;
			for (int x = 0; x < w; ++x) {
				int dx = r.from.x + x; if (dx < 0 || dx >= (int)screen_width) continue;
				drow[dx] = srow[x * c.w / w];
			}
		}
	}


	void briefing_step() {
		auto now = std::chrono::steady_clock::now();
		if (briefing_waiting) {
			if (now < briefing_wait_until) return;
			briefing_waiting = false;
		}
		while (briefing_pos < briefing_actions.size()) {
			auto& a = briefing_actions[briefing_pos++];
			switch (a.type) {
			case 1: // Wait
				briefing_wait_until = now + std::chrono::milliseconds(a.time_ms);
				briefing_waiting = true;
				return;
			case 2: { // Play WAV
				if (global_volume == 0) break;
				a_vector<uint8_t> d;
				if (!load_trigger_wav_data(a.wav, d)) break;
				// Faze 6: the head talks for as long as this line plays.
				int64_t dur = wav_duration_ms(d.data(), d.size());
				briefing_talk_until = now + std::chrono::milliseconds(dur > 0 ? dur : a.time_ms);
				trigger_wav_sound = native_sound::load_wav(d.data(), d.size());
				if (trigger_wav_sound) native_sound::play((int)sound_channels.size(), &*trigger_wav_sound, (128 - 4) * global_volume / 100, 0);
				break;
			}
			case 3: { // Text message
				// v0.83.005: same treatment as the in-game text -- the map's
				// CRLFs are breaks, its indentation belongs to a 640-wide
				// screen, not to ours. See split_and_wrap().
				for (auto& line : split_and_wrap(a.text, false)) { // the transcript wraps to its panel
					briefing_lines.push_back(line);
					while (briefing_lines.size() > 12) briefing_lines.erase(briefing_lines.begin());
				}
				break;
			}
			case 4: // Mission objectives
				briefing_objectives = a.text;
				break;
			case 5: // Show portrait: place it in its window slot (does not speak)
				briefing_slot_set(a.slot, a.unit);
				break;
			// Faze 4.7 FIX: MBRF action ids were swapped -- 6 is HIDE PORTRAIT and 7 is
			// DISPLAY SPEAKING PORTRAIT (staredit order), not the reverse. The old
			// mapping made every "start speaking" (7) hide the head (so a re-speaker
			// showed idle/blank) and every "hide" (6) make the current head talk (so a
			// finished speaker stuck mid-sentence) -- exactly the Desperate Alliance /
			// Revolution symptoms.
			case 6: // Hide portrait: clear that slot; stop the head only if it spoke
				if (a.slot >= 0 && a.slot <= 3) {
					briefing_cells[a.slot] = briefing_cell_t();
					if (a.slot == briefing_speaking_slot) { briefing_speaking_slot = -1; close_portrait(); }
				} else close_portrait();
				break;
			case 7: // Display speaking portrait: make the slot's portrait talk
				briefing_slot_talk(a.slot, a.unit);
				// No WAV on this action -> talk for the displayed duration.
				briefing_talk_until = now + std::chrono::milliseconds(a.time_ms > 0 ? a.time_ms : 1500);
				if (a.time_ms > 0) {
					briefing_wait_until = now + std::chrono::milliseconds(a.time_ms);
					briefing_waiting = true;
					return;
				}
				break;
			case 8: {
				// Transmission: the combined action SC campaigns actually use
				// for dialogue -- talking portrait + WAV + text + wait, in one.
				// Types 1-7 were handled and this was not, which is why
				// briefings showed their objectives but no conversation at all.
				//
				// v0.83.002: the unit field of a Transmission is ZERO in every
				// campaign briefing measured (`aidump mbrf campaign/terran/
				// terran02`): the speaker is set by the preceding "Display
				// portrait" (type 5) action -- 106 Command Center = Adjutant,
				// 19 Jim Raynor, 23 Edmund Duke. Passing that zero through
				// showed unit type 0, TERRAN MARINE, which is why every
				// character in every briefing wore a marine's face. So only a
				// non-zero unit replaces the speaker; otherwise the one already
				// on screen starts talking, as in case 6.
				// Faze 4.7 (measured, terran03 MBRF): a Transmission with unit=0 and
				// NO portrait shown -- which happens after a Hide Portrait (type 6) --
				// defaults to the ADJUTANT (Command Center, unit 106) in StarCraft, not
				// to "no speaker". Without this the Adjutant's between-character lines
				// ("Transmission ended...", "Ending transmission...") played with no
				// talking head.
				briefing_slot_talk(a.slot, a.unit); // speaker = the slot's portrait
				// Faze 6: talk for the WAV's real length (or the wait if there is no
				// WAV / it can't be measured), then idle -- not for the whole time_ms.
				{
					int64_t dur = 0;
					if (global_volume != 0 && !a.wav.empty()) {
						a_vector<uint8_t> d;
						if (load_trigger_wav_data(a.wav, d)) {
							dur = wav_duration_ms(d.data(), d.size());
							trigger_wav_sound = native_sound::load_wav(d.data(), d.size());
							if (trigger_wav_sound)
								native_sound::play((int)sound_channels.size(), &*trigger_wav_sound,
									(128 - 4) * global_volume / 100, 0);
						}
					}
					briefing_talk_until = now + std::chrono::milliseconds(dur > 0 ? dur : a.time_ms);
				}
				// v0.83.005: same treatment as the in-game text -- the map's
				// CRLFs are breaks, its indentation belongs to a 640-wide
				// screen, not to ours. See split_and_wrap().
				for (auto& line : split_and_wrap(a.text, false)) { // the transcript wraps to its panel
					briefing_lines.push_back(line);
					while (briefing_lines.size() > 12) briefing_lines.erase(briefing_lines.begin());
				}
				if (a.time_ms > 0) {
					briefing_wait_until = now + std::chrono::milliseconds(a.time_ms);
					briefing_waiting = true;
					return;
				}
				break;
			}
			default:
				break;
			}
		}
	}

	// v0.71 item 3: the briefing follows the original's Ready Room structure --
	// portrait frames down one side, the transmission transcript beside them,
	// the objectives box in the bottom-left corner, Start at the bottom-right --
	// but drawn natively and responsively instead of blitting the 640x480
	// glue\Ready<race> art, which would either be soft or letterboxed on a
	// tablet. It is drawn in the launcher's
	// design language (loading_screen::pal), like the loading screen before it --
	// no per-race accent any more.
	int briefing_margin() const { return (int)screen_width / 24; }

	rect briefing_start_button_rect() {
		int us = hud_us(); // Faze 4.7: scale to the FULL HD buffer (draw + hit-test share this)
		int m = briefing_margin();
		int w = (ui::unispace_w * 22 + 24) * us, h = ui::unispace_h * 3 * us;
		return rect{{(int)screen_width - w - m, (int)screen_height - h - m},
		            {(int)screen_width - m, (int)screen_height - m}};
	}
	rect briefing_skip_button_rect() {
		int us = hud_us();
		rect s = briefing_start_button_rect();
		int w = (ui::unispace_w * 12 + 24) * us;
		return rect{{s.from.x - w - 12 * us, s.from.y}, {s.from.x - 12 * us, s.to.y}};
	}
	// Faze 4.7 (user request): the Boot Camp (Terran tutorial) briefing gets a
	// distinctive SKIP TUTORIAL button, left of REPLAY. Only present for the
	// tutorial mission. Advances the campaign without playing the tutorial.
	bool briefing_is_tutorial = false;   // set by android_main for campaign/terran/tutorial
	bool briefing_skip_tutorial = false; // android_main polls -> advance + exit
	rect briefing_skip_tutorial_button_rect() {
		if (!briefing_is_tutorial) return rect{};
		int us = hud_us();
		rect s = briefing_skip_button_rect(); // REPLAY
		int w = (ui::unispace_w * 14 + 24) * us;
		return rect{{s.from.x - w - 12 * us, s.from.y}, {s.from.x - 12 * us, s.to.y}};
	}
	// Faze 4.7 (user request): the second briefing button is REPLAY, not Skip --
	// it restarts the briefing from the top (rewind the action cursor and clear the
	// accumulated transcript/objectives; they refill as the sequence replays).
	void briefing_replay() {
		briefing_pos = 0;
		briefing_lines.clear();
		briefing_objectives.clear();
		briefing_waiting = false;
		briefing_skip_all = false;
		briefing_reset_cells(); // speakers re-claim cells in order as the replay runs
		close_portrait();
	}

	void briefing_end() {
		briefing_active = false;
		briefing_waiting = false;
		close_portrait();
		briefing_lines.clear();
		briefing_chrome.release(); // a full-screen buffer: not needed in the mission
	}

	void briefing_click(int x, int y) {
		auto r = briefing_start_button_rect();
		if (x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y) {
			briefing_end();
			return;
		}
		// Faze 4.7: the second button is now REPLAY -- restart the briefing from the
		// top instead of skipping to the end.
		auto s = briefing_skip_button_rect();
		if (x >= s.from.x && x < s.to.x && y >= s.from.y && y < s.to.y) {
			briefing_replay();
			return;
		}
		// Faze 4.7: SKIP TUTORIAL (Boot Camp only) -- hand off to android_main, which
		// advances the campaign to the first mission without playing the tutorial.
		if (briefing_is_tutorial) {
			auto t = briefing_skip_tutorial_button_rect();
			if (x >= t.from.x && x < t.to.x && y >= t.from.y && y < t.to.y) {
				briefing_skip_tutorial = true;
				briefing_end();
				return;
			}
		}
		briefing_waiting = false; // tap = fast-forward the current wait
	}
	bool briefing_skip_all = false;

	// Static chrome of the briefing (backdrop, panels, labels, buttons), rendered
	// once and re-blitted: the panels are alpha-blended over the full-screen
	// backdrop, far too much per-pixel work to redo every frame. Keyed by everything
	// that shapes it; released when the briefing ends (it is a full-screen buffer).
	loading_screen::cache_t briefing_chrome;

	void draw_briefing_rgba(uint32_t* data, size_t pitch) {
		// Faze 6: while held (initial loading not finished), draw the briefing chrome
		// but do NOT advance the state machine -- no dialogue, no audio, no wall-clock
		// wait accrual -- so the briefing starts cleanly from the top once loading ends.
		if (briefing_skip_all) briefing_waiting = false;
		if (!briefing_hold) briefing_step();
		// Faze 6: once the current line's WAV has finished, drop the live head back to
		// idle (SC talks only while speaking). Fires once -- set_portrait clears
		// portrait_is_talking, so the guard stops re-triggering until the next line.
		if (!briefing_hold && portrait_is_talking && portrait_current_id >= 0
		    && std::chrono::steady_clock::now() >= briefing_talk_until) {
			set_portrait(portrait_current_id, false);
		}
		// Faze 4.7 redesign: scale to the FULL HD buffer; a row of 4 portrait cells
		// across the top (the original shows several speakers), the transcript
		// dropped below them, and the Objectives box pinned to the bottom edge.
		// Unified look: the launcher's
		// backdrop, panels, green section labels and buttons (loading_screen::pal) --
		// the layout and every hit rect are unchanged.
		namespace ls = loading_screen;
		int us = hud_us();
		bool _prev = hud_scale_text; hud_scale_text = true;
		struct g_t { bool* f; bool p; ~g_t() { *f = p; } } _g{&hud_scale_text, _prev};
		int sw = (int)screen_width, sh = (int)screen_height;
		int m = briefing_margin();
		int cw = (ui::unispace_w + 1) * us, lh = ui::unispace_h * us;
		const int pad = 10 * us;   // panel inner padding (vertical)
		const int tin = 12 * us;   // text inset from a panel's side edges
		const int ft = 2 * us;     // portrait cell frame thickness
		// The race-coloured band across the top is gone (the launcher's look is not
		// race tinted); its height stays as the top margin so the row keeps its place.
		int band_h = lh / 2 + 3 * us;

		// --- portrait row: 4 cells across the top -----------------------
		// The original Ready Room shows up to four speaking characters. The cell of
		// the slot that is speaking carries the live head and the green "selected"
		// frame; the other assigned cells loop their idle clip in a steel frame.
		int pgap = m / 3;
		int pcell = (sw - 2 * m - 3 * pgap) / 4;
		int prow_max = sh / 4; if (pcell > prow_max) pcell = prow_max; // never dominate the screen
		int prow_y = band_h + m / 2;
		// Centre the row (it can be narrower than the screen once pcell is clamped).
		int prow_total = 4 * pcell + 3 * pgap;
		int prow_x0 = (sw - prow_total) / 2;
		int prow_bottom = prow_y + pcell;

		// --- buttons + objectives box geometry (bottom edge) --------------
		rect startb = briefing_start_button_rect();
		rect skipb = briefing_skip_button_rect();
		rect tutb = briefing_skip_tutorial_button_rect(); // empty rect unless tutorial
		int buttons_left = briefing_is_tutorial ? tutb.from.x : skipb.from.x;
		a_vector<a_string> obj_lines;
		int obj_w = buttons_left - 12 * us - m - m; // left margin .. just left of the buttons
		if (obj_w < sw / 4) obj_w = sw / 4;
		int obj_h = 0, obj_y = 0, obj_bottom = sh - m; // share the buttons' baseline
		if (!briefing_objectives.empty()) {
			a_string cur;
			int maxw = (obj_w - 2 * tin) / cw; if (maxw < 8) maxw = 8;
			for (char c : briefing_objectives + a_string("\n")) {
				if (c == '\n' || (int)cur.size() >= maxw) {
					obj_lines.push_back(cur);
					cur.clear();
					if (c != '\n' && (unsigned char)c >= 0x20) cur += c;
				} else if ((unsigned char)c >= 0x20) cur += c;
			}
			obj_h = ((int)obj_lines.size() + 1) * (lh + 5 * us) + 2 * pad;
			obj_y = obj_bottom - obj_h;
		}
		rect ob = {{m, obj_y}, {m + obj_w, obj_y + obj_h}};

		// --- transcript panel geometry (full width, below the portrait row) ---
		// Its bottom is whichever is higher of the objectives box top and the button
		// row top, so the transcript never runs under either.
		rect tp = {{m, prow_bottom + m / 2}, {sw - m, (obj_h ? obj_y : startb.from.y) - 8 * us}};
		int head_y = tp.from.y + pad;
		int rule_y = head_y + lh + pad * 7 / 10;
		int text_y = rule_y + us + pad;

		// --- static chrome: backdrop, cells, panels, labels, buttons (cached) ---
		{
			ls::style_t bs;
			ui_backdrop_apply(bs);
			std::string mission = loading_info.kind == ls::info_t::campaign ? ls::upper(loading_info.title) : std::string();
			std::string heading = ls::upper(L("MISSION BRIEFING")), obj_label = ls::upper(L("OBJECTIVES"));
			std::string start_label = L("START MISSION");
			uint64_t sig = 1469598103934665603ull;
			auto mixi = [&](int64_t v) { for (int k = 0; k != 8; ++k) { sig ^= (uint8_t)(v >> (k * 8)); sig *= 1099511628211ull; } };
			auto mixs = [&](const std::string& s) { for (char q : s) mixi((uint8_t)q); mixi(-1); };
			mixi(sw); mixi(sh); mixi(us); mixi(m); mixi(pcell); mixi(prow_x0); mixi(prow_y);
			mixi(tp.from.y); mixi(tp.to.y); mixi(obj_h); mixi(obj_w); mixi(obj_y);
			mixi(startb.from.x); mixi(skipb.from.x); mixi(briefing_is_tutorial ? tutb.from.x : -1);
			mixi(bs.backdrop ? (int64_t)bs.backdrop->size() : -1); mixi(bs.backdrop_w); mixi(bs.backdrop_h);
			mixs(mission); mixs(heading); mixs(obj_label); mixs(start_label);
			if (!sig) sig = 1;
			auto& ch = briefing_chrome;
			if (ch.sig != sig || ch.w != sw || ch.h != sh || ch.px.size() != (size_t)sw * sh) {
				ch.px.assign((size_t)sw * sh, 0);
				ls::canvas cc; cc.data = ch.px.data(); cc.pitch = (size_t)sw; cc.w = sw; cc.h = sh;
				ls::draw_backdrop(cc, bs);
				for (int i = 0; i < 4; ++i) {
					// An empty cell: an opaque dark plate (the stills captured from it must not
					// carry the backdrop) in a dim steel frame.
					int cx = prow_x0 + i * (pcell + pgap);
					cc.frame(cx - us, prow_y - us, cx + pcell + us, prow_y + pcell + us, us, ls::pal::rim, 255);
					cc.fill(cx, prow_y, cx + pcell, prow_y + pcell, ls::pal::panel, 255);
					cc.frame(cx, prow_y, cx + pcell, prow_y + pcell, us, ls::mix(ls::pal::steel, 0x000000, 0.45f), 255);
				}
				ls::draw_panel(cc, tp.from.x, tp.from.y, tp.to.x, tp.to.y, true);
				ls::draw_text(cc, (float)(tp.from.x + tin), (float)head_y, heading, ls::pal::label, (float)lh, 0.12f);
				if (!mission.empty()) {
					// The mission's title, right-aligned on the heading row (when both fit).
					float hw = ls::text_width(heading, (float)lh, 0.12f), mw = ls::text_width(mission, (float)lh, 0.06f);
					if (hw + mw + 4 * tin <= (float)(tp.to.x - tp.from.x))
						ls::draw_text(cc, (float)(tp.to.x - tin) - mw, (float)head_y, mission, ls::pal::title, (float)lh, 0.06f);
				}
				ls::draw_hairline(cc, tp.from.x + tin, tp.to.x - tin, rule_y, us);
				if (obj_h) {
					ls::draw_panel(cc, ob.from.x, ob.from.y, ob.to.x, ob.to.y, true);
					ls::draw_text(cc, (float)(ob.from.x + tin), (float)(ob.from.y + pad), obj_label, ls::pal::label, (float)lh, 0.12f);
				}
				ls::draw_button(cc, skipb.from.x, skipb.from.y, skipb.to.x, skipb.to.y, "REPLAY", 0, (float)lh);
				if (briefing_is_tutorial) ls::draw_button(cc, tutb.from.x, tutb.from.y, tutb.to.x, tutb.to.y, "SKIP TUTORIAL", 3, (float)lh);
				ls::draw_button(cc, startb.from.x, startb.from.y, startb.to.x, startb.to.y, start_label, 1, (float)lh);
				ch.w = sw; ch.h = sh; ch.sig = sig;
			}
			for (int y = 0; y < sh; ++y)
				memcpy(data + (size_t)y * pitch, ch.px.data() + (size_t)y * sw, (size_t)sw * 4);
		}

		// --- portrait heads + the state frame of each assigned cell -----------
		{ // Faze 4.7 trace: what state is each speaker cell in (diagnose freezing)?
			static int _bdbg = 0;
			if ((++_bdbg % 90) == 0) {
				ui::log("brief: cur=%d talk=%d ncells=%d\n", portrait_current_id, (int)portrait_is_talking, briefing_cell_count);
				for (int c = 0; c < briefing_cell_count; ++c) {
					int sid = briefing_cells[c].portrait_id;
					ui::log("  cell%d sid=%d idleHD=%d talkHD=%d active=%d\n", c, sid,
						(int)(hd_portrait_cache.find((uint64_t)sid * 2) != hd_portrait_cache.end()),
						(int)(hd_portrait_cache.find((uint64_t)sid * 2 + 1) != hd_portrait_cache.end()),
						(int)(sid == portrait_current_id));
				}
			}
		}
		ls::canvas dc; dc.data = data; dc.pitch = pitch; dc.w = sw; dc.h = sh;
		for (int i = 0; i < 4; ++i) {
			int cx = prow_x0 + i * (pcell + pgap);
			rect pr = {{cx, prow_y}, {cx + pcell, prow_y + pcell}};
			rect inner = {{pr.from.x + ft, pr.from.y + ft}, {pr.to.x - ft, pr.to.y - ft}};
			bool assigned = (i < briefing_cell_count && briefing_cells[i].portrait_id >= 0);
			if (!assigned) continue; // the empty plate is part of the cached chrome
			// The active speaker is the cell whose SLOT is currently speaking (the MBRF
			// slot the last Transmission/DisplaySpeaking addressed), not "the cell whose
			// portrait matches the live head" -- two slots can hold the same portrait,
			// and the live head follows whoever spoke last regardless of slot.
			bool active = (i == briefing_speaking_slot);
			if (active) {
				draw_portrait_video(data, pitch, inner);
				briefing_capture_still(briefing_cells[i], data, pitch, inner); // still = fallback
			} else {
				// Inactive speaker: loop the IDLE clip only (never the talking clip --
				// that made a finished speaker look stuck mid-sentence). The idle clip
				// (cache key id*2+0) is warmed for every briefing speaker in hd_prewarm;
				// if it is still missing (SMK-only portrait), show the captured still.
				// Each cell runs on its own phase so they don't blink in lockstep.
				int sid = briefing_cells[i].portrait_id;
				std::shared_ptr<hd_portrait_clip> idle;
				auto ci = hd_portrait_cache.find((uint64_t)sid * 2);
				if (ci != hd_portrait_cache.end() && ci->second && !ci->second->frames.empty()) idle = ci->second;
				if (idle) {
					int n = (int)idle->frames.size();
					int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
					int idx = (int)(((ms / 100) + (int64_t)i * 5) % n);
					draw_clip_frame(*idle, idx, data, pitch, inner);
				} else {
					briefing_blit_still(briefing_cells[i], data, pitch, inner);
				}
				if (portrait_crt) draw_portrait_crt(data, pitch, inner);
			}
			// Speaking = the launcher's "selected" green edge (plus a soft outer ring);
			// a speaker waiting their turn = steel.
			dc.frame(pr.from.x, pr.from.y, pr.to.x, pr.to.y, ft, active ? ls::pal::label : ls::pal::steel, 255);
			if (active) dc.frame(pr.from.x - ft, pr.from.y - ft, pr.to.x + ft, pr.to.y + ft, ft, ls::pal::label, 70);
		}

		// --- transcript (inside its panel) --------------------------------
		{
			int tx = tp.from.x + tin;
			int y = text_y;
			int maxw = (tp.to.x - tp.from.x - 2 * tin) / cw; if (maxw < 8) maxw = 8;
			// Wrap everything first so the tail can be shown: a briefing keeps
			// appending lines, and the newest ones are the ones being spoken.
			a_vector<a_string> tlines;
			for (auto& l : briefing_lines) {
				size_t i = 0;
				while (i < l.size()) {
					size_t take = l.size() - i;
					if ((int)take > maxw) {
						size_t brk = 0;
						for (size_t k = 0; k < (size_t)maxw && i + k < l.size(); ++k)
							if (l[i + k] == ' ') brk = k;
						take = brk ? brk : (size_t)maxw;
					}
					tlines.push_back(l.substr(i, take));
					i += take;
					while (i < l.size() && l[i] == ' ') ++i;
				}
			}
			int bottom = tp.to.y - pad;
			int tlh = lh + 6 * us;
			int fits = (bottom - y) / tlh;
			if (fits < 1) fits = 1;
			size_t first = tlines.size() > (size_t)fits ? tlines.size() - fits : 0;
			for (size_t k = first; k != tlines.size(); ++k) {
				draw_text_rgba(data, pitch, tx, y, tlines[k].c_str(), 0xff000000u | ls::pal::text);
				y += tlh;
			}
		}

		// --- objectives (inside its panel; the label is part of the chrome) ---
		if (obj_h) {
			int y = obj_y + pad + lh + 5 * us;
			for (auto& l : obj_lines) {
				if (!l.empty()) draw_text_rgba(data, pitch, ob.from.x + tin, y, l.c_str(), 0xff000000u | ls::pal::text);
				y += lh + 5 * us;
			}
		}
	}

	virtual void on_trigger_minimap_ping(int owner, xy pos) override {
		if (!trigger_concerns_local(owner)) return;
		ping_pos = pos;
		ping_frame = (int)st.current_frame;
	}

	virtual void on_trigger_portrait(int owner, int unit_id, int duration_ms) override {
		if (!trigger_concerns_local(owner)) return;
		if (unit_id < 0 || unit_id >= 228) return;
		int p = get_unit_type((UnitTypes)unit_id)->portrait;
		if (p < 0 || (size_t)p >= portrait_record_count()) return;
		if (duration_ms <= 0) duration_ms = 3000;
		trigger_portrait_id = p;
		trigger_portrait_until = (int)st.current_frame + duration_ms / 42;
		set_portrait(p, true);
	}

	a_vector<uint8_t> creep_random_tile_indices = a_vector<uint8_t>(256 * 256);
	void init() {
		uint32_t rand_state = (uint32_t)clock.now().time_since_epoch().count();
		auto rand = [&]() {
			rand_state = rand_state * 22695477 + 1;
			return (rand_state >> 16) & 0x7fff;
		};
		for (auto& v : creep_random_tile_indices) {
			if (rand() % 100 < 4) v = 6 + rand() % 7;
			else v = rand() % 6;
		}

		a_vector<uint8_t> data;
		load_data_file(data, "arr/sfxdata.dat");
		sound_types = data_loading::load_sfxdata_dat(data);

		sound_filenames.resize(sound_types.vec.size());
		has_loaded_sound.resize(sound_filenames.size());
		loaded_sounds.resize(has_loaded_sound.size());
		last_played_sound.resize(loaded_sounds.size());

		string_table_data tbl;
		load_data_file(tbl.data, "arr/sfxdata.tbl");
		for (size_t i = 0; i != sound_types.vec.size(); ++i) {
			size_t index = sound_types.vec[i].filename_index;
			sound_filenames[i] = tbl[index];
		}
		native_sound::frequency = 44100;
		native_sound::channels = 9; // 8 SFX + 1 dedicated trigger-voice channel
		native_sound::init();

		sound_channels.resize(8);

		load_data_file(images_tbl.data, "arr/images.tbl");

		load_all_image_data(load_data_file);
	}

	virtual void on_action(int owner, int action) override {
		apm.at(owner).add_action(st.current_frame);
	}

	// Viewer-side "units lost" tally for the replay observer strip (the engine
	// records kills by killer+victim-type, not losses per owner). Not sim state;
	// android_main saves/restores it alongside apm in its rewind snapshots so a
	// backward seek stays consistent.
	std::array<int, 12> units_lost{};
	virtual void on_unit_destroy(unit_t* u) override {
		if (u && u->owner >= 0 && u->owner < 12) ++units_lost[(size_t)u->owner];
	}
	// Total kills (all victim types) for a player, from st.unit_kills.
	int observer_kills(int p) const {
		int k = 0;
		for (auto& v : st.unit_kills.at(p)) k += v;
		return k;
	}
	// Lifetime average income per minute (minerals + gas gathered / elapsed).
	int observer_income_per_min(int p) const {
		int64_t total = (int64_t)st.total_minerals_gathered.at(p) + st.total_gas_gathered.at(p);
		int frames = (int)st.current_frame; if (frames < 1) frames = 1;
		return (int)(total * (60 * 1000 / 42) / frames);
	}

	size_t view_width;
	size_t view_height;
	fp16 view_scale;

	rect_t<xy_t<size_t>> screen_tile_bounds() {
		size_t from_tile_y = screen_pos.y / 32u;
		if (from_tile_y >= game_st.map_tile_height) from_tile_y = 0;
		size_t to_tile_y = (screen_pos.y + view_height + 31) / 32u;
		if (to_tile_y > game_st.map_tile_height) to_tile_y = game_st.map_tile_height;
		size_t from_tile_x = screen_pos.x / 32u;
		if (from_tile_x >= game_st.map_tile_width) from_tile_x = 0;
		size_t to_tile_x = (screen_pos.x + view_width + 31) / 32u;
		if (to_tile_x > game_st.map_tile_width) to_tile_x = game_st.map_tile_width;

		return {{from_tile_x, from_tile_y}, {to_tile_x, to_tile_y}};
	}


	void draw_tiles(uint8_t* data, size_t data_pitch) {

		auto screen_tile = screen_tile_bounds();

		size_t tile_index = screen_tile.from.y * game_st.map_tile_width + screen_tile.from.x;
		auto* megatile_index = &st.tiles_mega_tile_index[tile_index];
		auto* tile = &st.tiles[tile_index];
		size_t width = screen_tile.to.x - screen_tile.from.x;

		xy dirs[9] = {{1, 1}, {0, 1}, {-1, 1}, {1, 0}, {-1, 0}, {1, -1}, {0, -1}, {-1, -1}, {0, 0}};

		for (size_t tile_y = screen_tile.from.y; tile_y != screen_tile.to.y; ++tile_y) {
			for (size_t tile_x = screen_tile.from.x; tile_x != screen_tile.to.x; ++tile_x) {

				int screen_x = tile_x * 32 - screen_pos.x;
				int screen_y = tile_y * 32 - screen_pos.y;

				size_t offset_x = 0;
				size_t offset_y = 0;
				if (screen_x < 0) {
					offset_x = -screen_x;
				}
				if (screen_y < 0) {
					offset_y = -screen_y;
				}

				uint8_t* dst = data + screen_y * data_pitch + screen_x;

				size_t width = 32;
				size_t height = 32;

				width = std::min(width, screen_width - screen_x);
				height = std::min(height, screen_height - screen_y);

				size_t index = *megatile_index;
				if (tile->flags & tile_t::flag_has_creep) {
					index = game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[tile_x + tile_y * game_st.map_tile_width]];
				}
				draw_tile(tileset_img, index, dst, data_pitch, offset_x, offset_y, width, height);

				if (~tile->flags & tile_t::flag_has_creep) {
					size_t creep_index = 0;
					for (size_t i = 0; i != 9; ++i) {
						int add_x = dirs[i].x;
						int add_y = dirs[i].y;
						if (tile_x + add_x >= game_st.map_tile_width) continue;
						if (tile_y + add_y >= game_st.map_tile_height) continue;
						if (st.tiles[tile_x + add_x + (tile_y + add_y) * game_st.map_tile_width].flags & tile_t::flag_has_creep) creep_index |= 1 << i;
					}
					size_t creep_frame = img.creep_edge_frame_index[creep_index];

					if (creep_frame) {

						auto& frame = tileset_img.creep_grp.frames.at(creep_frame - 1);

						screen_x += frame.offset.x;
						screen_y += frame.offset.y;

						size_t width = frame.size.x;
						size_t height = frame.size.y;

						if (screen_x < (int)screen_width && screen_y < (int)screen_height) {
							if (screen_x + (int)width > 0 && screen_y + (int)height > 0) {

								size_t offset_x = 0;
								size_t offset_y = 0;
								if (screen_x < 0) {
									offset_x = -screen_x;
								}
								if (screen_y < 0) {
									offset_y = -screen_y;
								}

								uint8_t* dst = data + screen_y * data_pitch + screen_x;

								width = std::min(width, screen_width - screen_x);
								height = std::min(height, screen_height - screen_y);

								draw_frame(frame, false, dst, data_pitch, offset_x, offset_y, width, height);
							}
						}
					}
				}

				++megatile_index;
				++tile;
			}
			megatile_index -= width;
			megatile_index += game_st.map_tile_width;
			tile -= width;
			tile += game_st.map_tile_width;
		}
	}

	a_vector<uint8_t> temporary_warp_texture_buffer;

	void draw_image(const image_t* image, uint8_t* data, size_t data_pitch, size_t color_index) {

		if (is_new_image(image)) {
			image_draw_queue.push_back(image);
			return;
		}

		xy map_pos = get_image_map_position(image) + interp_off_for(image->sprite);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;

		auto& frame = image->grp->frames.at(image->frame_index);

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		auto draw_alpha = [&](size_t index, auto remap_f) {
			auto& data = tileset_img.light_pcx.at(index).data;
			uint8_t* ptr = data.data();
			size_t size = data.size() / 256;
			auto glow = [ptr, size, remap_f](uint8_t new_value, uint8_t old_value) {
				new_value = remap_f(new_value, old_value);
				--new_value;
				if (new_value >= size) return (uint8_t)0;
				return ptr[256u * new_value + old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, glow);
		};

		if (image->modifier == 0 || image->modifier == 1) {
			uint8_t* ptr = img.player_unit_colors.at(color_index).data();
			auto player_color = [ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return ptr[new_value - 8];
				return new_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, player_color);
		} else if (image->modifier == 2 || image->modifier == 4) {
			uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
			draw_alpha(4, [color_ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			});
			uint8_t* selector = tileset_img.cloak_fade_selector.data();
			int value = image->modifier_data1;
			auto cloaking = [color_ptr, selector, value](uint8_t new_value, uint8_t old_value) {
				if (selector[new_value] <= value) return old_value;
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, cloaking);
		} else if (image->modifier == 3) {
			uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
			draw_alpha(4, [color_ptr](uint8_t new_value, uint8_t) {
				if (new_value >= 8 && new_value < 16) return color_ptr[new_value - 8];
				return new_value;
			});
		} else if (image->modifier == 8) {
			size_t data_size = data_pitch * screen_height;
			auto distortion = [data_size, dst](uint8_t new_value, uint8_t& old_value) {
				size_t offset = &old_value - dst;
				if (offset >= new_value && data_size - offset > new_value) return *(&old_value + new_value);
				return old_value;
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, distortion);
		} else if (image->modifier == 10) {
			uint8_t* ptr = &tileset_img.dark_pcx.data[256 * 18];
			auto shadow = [ptr](uint8_t, uint8_t old_value) {
				return ptr[old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, shadow);
		} else if (image->modifier == 9) {
			draw_alpha(image->image_type->color_shift - 1, no_remap());
		} else if (image->modifier == 12) {
			if (temporary_warp_texture_buffer.size() < frame.size.x * frame.size.y) temporary_warp_texture_buffer.resize(frame.size.x * frame.size.y);
			auto& texture_frame = global_st.image_grp[(size_t)ImageTypes::IMAGEID_Warp_Texture]->frames.at(image->modifier_data1);
			draw_frame(texture_frame, false, temporary_warp_texture_buffer.data(), frame.size.x, 0, 0, frame.size.x, frame.size.y);
			draw_frame_textured(frame, temporary_warp_texture_buffer.data(), i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height);
		} else if (image->modifier == 17) {
			auto& data = tileset_img.light_pcx.at(0).data;
			uint8_t* ptr = &data.at(256u * (image->modifier_data1 - 1));
			size_t size = data.data() + data.size() - ptr;
			auto glow = [ptr, size](uint8_t, uint8_t old_value) {
				if (old_value >= size) return (uint8_t)0;
				return ptr[old_value];
			};
			draw_frame(frame, i_flag(image, image_t::flag_horizontally_flipped), dst, data_pitch, offset_x, offset_y, width, height, glow);
		} else error("don't know how to draw image modifier %d", image->modifier);

	}

	a_vector<const unit_t*> current_selection_sprites_set = a_vector<const unit_t*>(2500);
	a_vector<const sprite_t*> current_selection_sprites;

	void draw_selection_circle(const sprite_t* sprite, const unit_t* u, uint8_t* data, size_t data_pitch) {
		auto* image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		xy map_pos = sprite->position + xy(0, sprite->sprite_type->selection_circle_vpos) + interp_off_for(sprite);

		auto* grp = global_st.image_grp[(size_t)image_type->id];
		auto& frame = grp->frames.at(0);

		map_pos.x += int(frame.offset.x - grp->width / 2);
		map_pos.y += int(frame.offset.y - grp->height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		size_t color_index = st.players[sprite->owner].color;
		uint8_t color = img.player_unit_colors.at(color_index)[0];
		if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
			color = tileset_img.resource_minimap_color;
		}
		auto player_color = [color](uint8_t new_value, uint8_t) {
			if (new_value >= 0 && new_value < 8) return color;
			return new_value;
		};
		draw_frame(frame, false, dst, data_pitch, offset_x, offset_y, width, height, player_color);

	}

	void draw_health_bars(const sprite_t* sprite, const unit_t* u, uint8_t* data, size_t data_pitch) {

		auto* selection_circle_image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		auto* selection_circle_grp = global_st.image_grp[(size_t)selection_circle_image_type->id];
		auto& selection_circle_frame = selection_circle_grp->frames.at(0);

		int offsety = sprite->sprite_type->selection_circle_vpos + selection_circle_frame.size.y / 2 + 8;

		bool has_shield = u->unit_type->has_shield;
		bool has_energy = ut_has_energy(u) || u_hallucination(u) || unit_is(u, UnitTypes::Zerg_Broodling);

		int width = sprite->sprite_type->health_bar_size;
		width -= (width - 1) % 3;
		if (width < 19) width = 19;
		int orig_width = width;
		int height = 5;
		if (has_shield) height += 2;
		if (has_energy) height += 6;

		xy map_pos = sprite->position + xy(0, offsety) + interp_off_for(sprite);

		map_pos.x += int(0 - width / 2);
		map_pos.y += int(0 - height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + width <= 0 || screen_y + height <= 0) return;

		auto filled_width = [&](int percent) {
			int r = percent * width / 100;
			if (r < 3) r = 3;
			else if (r % 3) {
				if (r % 3 > 1) r += 3 - (r % 3);
				else r -= r % 3;
			}
			return r;
		};

		int hp_percent = unit_hp_percent(u);
		int dw = filled_width(hp_percent);

		int shield_dw = 0;
		if (has_shield) {
			int shield_percent = (int)u->shield_points.integer_part() * 100 / std::max(u->unit_type->shield_points, 1);
			shield_dw = filled_width(shield_percent);
		}

		int energy_dw = 0;
		if (has_energy) {
			int energy_percent;
			if (ut_has_energy(u)) energy_percent = (int)u->energy.integer_part() * 100 / std::max((int)unit_max_energy(u).integer_part(), 1);
			else energy_percent = (int)u->remove_timer / std::max((int)default_remove_timer(u), 1);
			energy_dw = filled_width(energy_percent);
		}

		const int no_shield_colors_66[] = {18, 0, 1, 2, 18};
		const int no_shield_colors_33[] = {18, 3, 4, 5, 18};
		const int no_shield_colors_0[] = {18, 6, 7, 8, 18};
		const int no_shield_colors_bg[] = {18, 15, 16, 17, 18};

		const int with_shield_colors_66[] = {18, 0, 0, 1, 1, 2, 18};
		const int with_shield_colors_33[] = {18, 3, 3, 4, 4, 5, 18};
		const int with_shield_colors_0[] = {18, 6, 6, 7, 7, 8, 18};
		const int with_shield_colors_bg[] = {18, 15, 15, 16, 16, 17, 18};

		const int* colors_66 = has_shield ? with_shield_colors_66 : no_shield_colors_66;
		const int* colors_33 = has_shield ? with_shield_colors_33 : no_shield_colors_33;
		const int* colors_0 = has_shield ? with_shield_colors_0 : no_shield_colors_0;
		const int* colors_bg = has_shield ? with_shield_colors_bg : no_shield_colors_bg;

		int offset_x = 0;
		int offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
			dw = std::max(dw + screen_x, 0);
			shield_dw = std::max(shield_dw + screen_x, 0);
			energy_dw = std::max(energy_dw + screen_x, 0);
			width = std::max(width + screen_x, 0);
			screen_x = 0;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
			height += screen_y;
			screen_y = 0;
		}

		uint8_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, (int)screen_width - screen_x);
		height = std::min(height, (int)screen_height - screen_y);

		if (dw > width) dw = width;
		if (shield_dw > width) shield_dw = width;
		if (energy_dw > width) energy_dw = width;

		int hp_height = std::min(std::max((has_shield ? 7 : 5) - offset_y, 0), height);

		for (int i = offset_y; i < offset_y + hp_height; ++i) {
			int ci = hp_percent >= 66 ? colors_66[i] : hp_percent >= 33 ? colors_33[i] : colors_0[i];
			int c = img.hp_bar_colors.at(ci);

			if (dw > 0) memset(dst, c, dw);
			if (width - dw > 0) {
				c = img.hp_bar_colors.at(colors_bg[i]);
				memset(dst + dw, c, width - dw);
			}
			dst += data_pitch;
		}

		if (has_shield) {
			const int shield_colors[] = {18, 10, 11, 18};
			const int shield_colors_bg[] = {18, 16, 17, 18};

			dst = data + screen_y * data_pitch + screen_x;

			for (int i = offset_y; i < std::min(4, height); ++i) {
				int c = img.hp_bar_colors.at(shield_colors[i]);

				if (shield_dw > 0) memset(dst, c, shield_dw);
				if (width - shield_dw > 0) {
					c = img.hp_bar_colors.at(shield_colors_bg[i]);
					memset(dst + shield_dw, c, width - shield_dw);
				}
				dst += data_pitch;
			}
		}

		int energy_offset = std::max((has_shield ? 8 : 6) - offset_y, 0);
		int energy_begin = std::max(offset_y - (has_shield ? 8 : 6), 0);
		int energy_end = std::min(5, offset_y + height - (has_shield ? 8 : 6));

		if (has_energy ) {
			dst = data + (screen_y + energy_offset) * data_pitch + screen_x;
			const int energy_colors[] = {18, 12, 13, 14, 18};
			for (int i = energy_begin; i < energy_end; ++i) {
				int c = img.hp_bar_colors.at(energy_colors[i]);

				if (energy_dw > 0) memset(dst, c, energy_dw);
				if (width - energy_dw > 0) {
					c = img.hp_bar_colors.at(no_shield_colors_bg[i]);
					memset(dst + energy_dw, c, width - energy_dw);
				}
				dst += data_pitch;
			}
		}

		dst = data + screen_y * data_pitch + screen_x;
		if (offset_x % 3) dst += 3 - offset_x % 3;

		int c = img.hp_bar_colors.at(18);
		for (int x = 0; x < orig_width; x += 3) {
			if (x < offset_x || x >= offset_x + width) continue;
			for (int y = 0; y != hp_height; ++y) {
				*dst = c;
				dst += data_pitch;
			}
			if (has_energy && energy_end > energy_begin) {
				if (energy_offset) dst += data_pitch;
				for (int i = energy_begin; i != energy_end; ++i) {
					*dst = c;
					dst += data_pitch;
				}
				if (energy_offset) dst -= data_pitch;
				dst -= (energy_end - energy_begin) * data_pitch;
			}
			dst -= hp_height * data_pitch;
			dst += 3;
		}

	}

	// --- v0.22 RGBA sprite pass -----------------------------------------
	// When rgba_sprites is on, the world sprites skip the 8-bit indexed pass
	// and are drawn in true color onto the rgba surface (after the palette
	// blit, before fog/HUD). Entries keep the sprite sort order. This is the
	// foundation for HD assets: the blitter takes any GRP resolution.
	bool rgba_sprites = false;

	struct rgba_sprite_entry {
		const image_t* image;    // image draw when set
		const sprite_t* sprite;
		const unit_t* circle_u;  // selection circle when set
		const unit_t* bars_u;    // health bars when set
	};
	a_vector<rgba_sprite_entry> rgba_sprite_queue;

	// Lightweight per-phase frame profiler; averages logged every 256 frames.
	using prof_clock = std::chrono::steady_clock;
	prof_clock::time_point prof_t0;
	std::array<int64_t, 8> prof_us{};
	int prof_frames = 0;
	void prof_add(int i) {
		auto now = prof_clock::now();
		prof_us[i] += std::chrono::duration_cast<std::chrono::microseconds>(now - prof_t0).count();
		prof_t0 = now;
	}
	void prof_frame_done() {
		if (++prof_frames == 256) {
			int64_t total = prof_us[0]+prof_us[1]+prof_us[2]+prof_us[3]+prof_us[4]+prof_us[5];
			ui::log("prof us/frame: tiles %d sprites %d blit %d hd+queue %d fog %d hud %d | render %d fps %d\n",
			    (int)(prof_us[0] / 256), (int)(prof_us[1] / 256), (int)(prof_us[2] / 256),
			    (int)(prof_us[3] / 256), (int)(prof_us[4] / 256), (int)(prof_us[5] / 256),
			    (int)(total / 256), fps_display);
			prof_us = {};
			prof_frames = 0;
		}
	}

	// --- v0.23 HD sprites (SC:R .anim via mods) -------------------------
	// When hd_anim_dir is set (an active mod ships anim/main_NNN.anim),
	// images with an HD sprite whose frame count matches the GRP are drawn
	// from the decoded atlas instead of the paletted GRP. Loaded lazily per
	// image id; a failed load is cached so we only try once.
	a_string hd_anim_dir;

	// --- v0.83 HD cache bounds (P2) --------------------------------------
	// Every cache below used to grow without limit and was never released,
	// including on SDL_APP_LOWMEMORY. Measured on the user's HD2 pack: the
	// mean .anim costs 580 kB resident, the largest 11.9 MB, and all 868 would
	// be ~490 MB. So each cache now carries a byte total and drops its
	// least-recently-used entries past a budget.
	//
	// Eviction never touches an entry used during the CURRENT frame: the
	// getters hand out raw pointers and the caller draws with them, so
	// anything this frame has already asked for must stay alive. Everything
	// older is fair game, however recently it was touched.
	//
	// The first attempt pinned "the last 32 lookups" instead, and a forced-
	// eviction test caught it: with few but large entries every one of them
	// falls inside 32 lookups, so the trim ran and dropped nothing -- the
	// budget would have been silently unenforced in exactly the case it
	// exists for. Failed loads (bytes == 0) stay pinned as well; dropping one
	// only buys a repeated open() that fails again.
	uint64_t hd_cache_tick = 0;

	int hd_cache_frame() const { return (int)st.current_frame; }

	template<typename Map>
	void trim_hd_cache(Map& m, size_t& bytes, size_t budget, const char* name, bool& logged) {
		if (bytes <= budget) return;
		int now = hd_cache_frame();
		size_t before = bytes;
		size_t dropped = 0;
		while (bytes > budget) {
			auto victim = m.end();
			for (auto it = m.begin(); it != m.end(); ++it) {
				if (it->second.bytes == 0 || it->second.frame == now) continue;
				if (victim == m.end() || it->second.used < victim->second.used) victim = it;
			}
			if (victim == m.end()) break;   // all live this frame; overshoot
			bytes -= victim->second.bytes;
			m.erase(victim);
			++dropped;
		}
		if (!logged) {
			// Once per cache per session: enough for a device log to show the
			// budget was reached and that entries actually went away.
			logged = true;
			ui::log("hd cache '%s' hit its %d kB budget at frame %d: dropped %d of %d entries, %d -> %d kB\n",
				name, (int)(budget >> 10), now, (int)dropped, (int)(m.size() + dropped),
				(int)(before >> 10), (int)(bytes >> 10));
		}
	}

	struct hd_anim_entry_t {
		anim::sprite_t sp;
		size_t bytes = 0;
		uint64_t used = 0;
		int frame = -1;
	};
	std::unordered_map<int, hd_anim_entry_t> hd_anims;   // A4: O(1) (per drawn image/frame)
	size_t hd_anims_bytes = 0;
	bool hd_anims_trim_logged = false;

	// --- Async HD-anim prefetch (perf #2) --------------------------------
	// First appearance of an image_id used to load+parse its SC:R HD anim --
	// and for "hero/alt" image ids, scan MANY sibling anims -- SYNCHRONOUSLY on
	// the render thread. That is the ~1s stall the DIAG PERF_HITCH/ASSET_SLOW_LOAD
	// detectors flag (e.g. lazy CASC decode at f~7048). Now a background worker
	// does the CASC read + parse: the render path enqueues the image_id, draws
	// the SD sprite for a few frames, and picks up the HD result once the worker
	// finishes (drained into hd_anims each frame). CASC byte reads are serialised
	// inside the load_hd_bytes callback (android_main), so the worker never races
	// the main thread's tile/portrait/cinematic reads; the parse runs lock-free
	// off-thread. Inert on host tools and SD tier (hd_anim_dir empty && !hd_casc):
	// the worker is never started, so determinism/gate paths are untouched.
	struct hd_pf_result_t {
		int image_id = -1;
		hd_anim_entry_t entry;
		int64_t load_us = 0;
		int via_sibling = -1;
		bool missing = false;
	};
	std::thread hd_pf_thread;
	std::mutex hd_pf_mtx;                        // guards hd_pf_queue + hd_pf_done
	std::condition_variable hd_pf_cv;
	std::deque<std::pair<int, int>> hd_pf_queue; // (image_id, grp_index), producer=render thread
	std::vector<hd_pf_result_t> hd_pf_done;      // finished, awaiting main-thread drain
	std::unordered_set<int> hd_pf_inflight;      // queued or decoding (render-thread-only; dedupe)
	bool hd_pf_quit = false;
	bool hd_pf_started = false;

	void hd_prefetch_start() {
		if (hd_pf_started) return;
		hd_pf_started = true;
		hd_pf_thread = std::thread([this] { hd_prefetch_loop(); });
	}
	void hd_prefetch_loop() {
		for (;;) {
			std::pair<int, int> req;
			{
				std::unique_lock<std::mutex> lk(hd_pf_mtx);
				hd_pf_cv.wait(lk, [this] { return hd_pf_quit || !hd_pf_queue.empty(); });
				if (hd_pf_quit) return;
				req = hd_pf_queue.front();
				hd_pf_queue.pop_front();
			}
			int image_id = req.first, grp_index = req.second;
			hd_pf_result_t r;
			r.image_id = image_id;
			int64_t t0 = std::chrono::duration_cast<std::chrono::microseconds>(prof_clock::now().time_since_epoch()).count();
			// SC:R names HD anims anim/main_{image_id}.anim. Hero/alt ids (e.g.
			// Terran Command Center image 275) have no own anim and resolve the
			// canonical sibling sharing the same grp_filename_index (275 -> 63).
			anim::sprite_t sp = load_hd_anim_num(image_id);
			int via_sibling = -1;
			if (!sp.ok && grp_index >= 0) {
				auto& iv = global_st.image_types.vec;
				for (size_t sid = 0; sid < iv.size(); ++sid) {
					if ((int)sid == image_id || (int)iv[sid].grp_filename_index != grp_index) continue;
					anim::sprite_t sib = load_hd_anim_num((int)sid);
					if (sib.ok) { sp = std::move(sib); via_sibling = (int)sid; break; }
				}
			}
			r.load_us = std::chrono::duration_cast<std::chrono::microseconds>(prof_clock::now().time_since_epoch()).count() - t0;
			r.via_sibling = via_sibling;
			r.missing = !sp.ok && grp_index >= 0;
			r.entry.sp = std::move(sp);
			if (r.entry.sp.ok) {
				r.entry.bytes = r.entry.sp.diffuse.size() * 4 + r.entry.sp.teamcolor.size()
					+ r.entry.sp.emissive.size() * 4 + r.entry.sp.frames.size() * sizeof(anim::frame_t);
			}
			{
				std::lock_guard<std::mutex> lk(hd_pf_mtx);
				hd_pf_done.push_back(std::move(r));
			}
		}
	}
	// Move finished worker results into the live cache (main thread, per frame).
	// diag emit + logging happen here (bwgame::diag / ui::log are not guaranteed
	// thread-safe), so the worker only ever touches CASC + the parse + the queues.
	void hd_prefetch_drain() {
		if (!hd_pf_started) return;
		std::vector<hd_pf_result_t> done;
		{
			std::lock_guard<std::mutex> lk(hd_pf_mtx);
			if (hd_pf_done.empty()) return;
			done.swap(hd_pf_done);
		}
		for (auto& r : done) {
			if (bwgame::diag::enabled()) {
				if (r.missing)
					bwgame::diag::emit((long)st.current_frame, "asset", bwgame::diag::Sev::warn, "ASSET_MISSING",
						"no HD sprite anim -- SD fallback", "\"image\":" + std::to_string(r.image_id));
				else if (r.load_us > 120000 && (uint32_t)st.current_frame > 300)
					bwgame::diag::emit((long)st.current_frame, "asset", bwgame::diag::Sev::info, "ASSET_SLOW_LOAD",
						"HD sprite decode >120ms (off-thread -- no frame block)", "\"image\":" + std::to_string(r.image_id) +
						",\"us\":" + std::to_string(r.load_us) + ",\"via_sibling\":" + std::to_string(r.via_sibling));
			}
			if (hd_anim_diag) ui::log("hdanim(pf) id=%d %s via_sibling=%d bytes=%d\n",
				r.image_id, r.entry.sp.ok ? "OK" : "MISS->SD", r.via_sibling, (int)r.entry.bytes);
			size_t bytes = r.entry.bytes;
			int id = r.image_id;
			auto res = hd_anims.emplace(id, std::move(r.entry));
			if (res.second) {
				hd_anims_bytes += bytes;
				res.first->second.used = ++hd_cache_tick;
				res.first->second.frame = hd_cache_frame();
			}
			hd_pf_inflight.erase(id);
		}
		trim_hd_cache(hd_anims, hd_anims_bytes, (size_t)64 << 20, "anims", hd_anims_trim_logged);
	}

	// HD UI assets (v0.23): <mod>/ui/cmdicons.dds.grp (SC:R HD icons) and
	// <mod>/icons/NNN.png (user icon packs) override the paletted cmdicons.
	// Scaled to the legacy icon box once and cached.
	a_string hd_ui_dir;
	a_string icons_png_dir;
	ddsgrp::file_t hd_cmdicons_dds;
	bool hd_cmdicons_dds_tried = false;
	// One scaled frame plus its accounting, for the byte-bounded caches.
	struct cached_frame_t {
		ddsgrp::frame_t f;
		size_t bytes = 0;
		uint64_t used = 0;
		int frame = -1;
	};
	std::map<int, cached_frame_t> icon_override_cache;
	size_t icon_override_bytes = 0;
	bool icon_override_trim_logged = false;

	// --- v0.23 HD terrain -----------------------------------------------
	// <mod>/tileset/<name>.dds.vr4 is the same dds.grp-style container with
	// one 64x64 (HD2) DXT1 frame per MEGATILE (vx4 order). Kept compressed
	// in memory; frames decode+downscale to 32x32 logical on demand into a
	// bounded cache (whole-cache flush when it grows past ~16MB).
	a_string hd_tileset_dir;
	struct hd_tiles_t {
		bool ok = false;
		std::vector<uint8_t> raw;
		struct ent { uint32_t off, size; };
		std::vector<ent> ents;
		// Faze 7 perf: recency-tracked so the decoded-tile cache uses the shared LRU
		// (trim_hd_cache) instead of a full cache.clear() at the cap -- the full clear
		// re-decoded the whole visible tile set on one frame (a visible hitch on large
		// maps). Each entry carries its byte size + last-use tick/frame like the others.
		struct tile_c { std::vector<uint32_t> rgba; size_t bytes = 0; uint64_t used = 0; int frame = -1; };
		std::unordered_map<size_t, tile_c> cache;   // A4: O(1) (per visible tile/frame)
	};
	hd_tiles_t hd_tiles;
	size_t hd_tiles_cache_bytes = 0;
	bool hd_tiles_trim_logged = false;
	// Faze 7: composite the SC:R emissive (self-glow) anim layer additively. On by
	// default; a cheap CPU add per glowing pixel. Settings-controllable.
	bool hd_emissive = true;
	// Faze 7: optional GPU bloom strength (0 = off). Applied to the present window.
	int hd_bloom = 0;
	// SP shader pipeline: GLES2 present stage. hd_post_gl toggles it; hd_post_effect picks
	// the effect (-1 AUTO = sharpen on Adreno / identity elsewhere, 0 identity, 1 sharpen);
	// hd_post_strength (0..100) scales it. Applied to the window each frame.
	bool hd_post_gl = false;
	int hd_post_effect = -1;
	int hd_post_strength = 35;
	// SP Phase 3: real GPU bloom through the GL post chain (0..100; 0 = off). Replaces the
	// SDL_ComposeCustomBlendMode bloom when the GL present stage is active.
	int hd_gl_bloom = 0;
	// SE color-grade: subtle cinematic tone (0..100; 0 = neutral). Applied in the final GL pass.
	int hd_grade = 0;
	// Faze 7: unit-shadow softness, 0..100 (settings slider). 0 = classic hard 50%
	// black; higher lightens the shadow so terrain shows through more.
	int hd_shadow_soft = 0;
	// SHADOWS (SM/SI) Phase 1: directional sun shadow (software MVP). Re-shears the baked
	// modifier==10 silhouette in place along a fixed global sun vector so shadows lean like
	// a cast shadow instead of the flat baked drop. Presentation-only (no sim touch); the
	// in-place transform keeps the sprite draw order intact (no new depth-sort entries).
	int hd_shadow_dir = 0;    // 0 = off (baked shadow only), 1 = directional sun shadow
	int hd_shadow_len = 40;   // lean length 0..100 -> px horizontal shift per row of height
	// Sun azimuth sign: +1 casts to the right/SE (matches BW's upper-left baked light),
	// -1 to the left. Kept as a field so the settings/evening tuning can flip it.
	int hd_shadow_sign = 1;
	std::vector<uint32_t> shadow_shear_scratch; // silhouette coverage mask for the sheared blit
	std::vector<uint8_t> shadow_dst_mask;       // dest-space coverage (darken each pixel once)
	// SHADOWS Phase 2: dynamic point lights (software MVP). Sourced read-only from the SC:R
	// emissive layer the HD sprite composite already decodes (weapon charges / psi / warp /
	// explosions glow) -- a sprite's aggregated emissive energy becomes a colored light that
	// additively brightens the world around it (before fog). Presentation-only, default OFF.
	int hd_point_lights = 0;    // 0 = off, 1 = on
	int hd_light_strength = 100; // 0..200 % scale on light intensity/radius
	struct light_source_t { int x, y, radius, intensity; uint32_t rgb; };
	std::vector<light_source_t> frame_lights; // rebuilt each rendered frame; read-only from st
	// Faze 7+ interface toggles (Settings > Sąsaja). Pure presentation.
	bool hud_show_fps = true;      // top-left fps readout
	bool hud_show_apm = false;     // top-centre APM readout (off by default)
	bool hud_health_always = false;// draw unit health bars without hovering/selecting
	int camera_speed = 100;        // edge/key-scroll speed as a % of the classic curve
	// Shadow pixel darken. keep = fraction of the underlying brightness retained,
	// in /256 units: 128 (=50%) at softness 0, up to ~224 (=87%) at softness 100.
	uint32_t shadow_darken(uint32_t old) const {
		if (hd_shadow_soft <= 0) return 0xff000000u | ((old >> 1) & 0x7f7f7f);
		int s = hd_shadow_soft > 100 ? 100 : hd_shadow_soft;
		uint32_t keep = 128u + (uint32_t)(96 * s / 100); // 128..224
		uint32_t r = (((old >> 16) & 0xff) * keep) >> 8;
		uint32_t g = (((old >> 8) & 0xff) * keep) >> 8;
		uint32_t b = ((old & 0xff) * keep) >> 8;
		return 0xff000000u | (r << 16) | (g << 8) | b;
	}

	// SHADOWS Phase 1: draw a modifier==10 silhouette as a directional cast shadow instead
	// of the flat baked drop. The silhouette is rasterized to a coverage mask, then blitted
	// with a per-row horizontal shear anchored at the bottom (ground-contact) row: rows
	// higher up (larger h) are shifted along the sun vector, so the shadow leans like a
	// projected one. screen_x/screen_y are LOGICAL map px (like the caller); ws scales to
	// physical, matching draw_image_rgba's ws paths. Reuses the RLE decoder + shadow_darken,
	// so it is byte-for-byte the baked shadow's darkness, only relocated. Hard-edged (no blur
	// -- the MVP per SHADOWS-review). Draw order is unchanged: this replaces the in-place
	// shadow blit at the same point in the sprite loop, injecting no new depth-sort entries.
	void draw_shadow_sheared(const grp_t::frame_t& frame, bool flipped, int screen_x, int screen_y,
	                         int ws, uint32_t* data, size_t data_pitch) {
		int fw = (int)frame.size.x, fh = (int)frame.size.y;
		if (fw <= 0 || fh <= 0) return;
		size_t need = (size_t)fw * fh;
		if (shadow_shear_scratch.size() < need) shadow_shear_scratch.resize(need);
		std::fill_n(shadow_shear_scratch.data(), need, 0u);
		// Rasterize silhouette coverage (1 = covered) via the existing decoder.
		draw_frame_rgba(frame, flipped, shadow_shear_scratch.data(), (size_t)fw, 0, 0,
			(size_t)fw, (size_t)fh, [](uint8_t, uint32_t&) { return 1u; });
		// Project the silhouette flat onto the ground from the bottom (contact) row along the
		// sun vector: a pixel at height h above the contact lands at (+h*lx, +h*ly) from the
		// contact line, so the building TOP casts to the FAR end of a shadow lying on open
		// ground to the lower-right (SE, matching BW's upper-left baked light). This unfolds
		// the silhouette away from the body instead of leaning the occluded top in place.
		float lx = (hd_shadow_sign < 0 ? -1.f : 1.f) * (hd_shadow_len / 100.0f); // horiz px / row of height
		float ly = (hd_shadow_len / 100.0f) * 0.5f;   // vertical px / row (iso foreshorten; always down)
		int anchor = fh - 1;                          // bottom row = ground-contact line
		int anchor_y = screen_y + anchor;
		// Dedup coverage in a local LOGICAL-space mask so overlapping projected rows darken a
		// destination pixel only ONCE (the vertical foreshorten maps several source rows to
		// the same dst row; without this the overlap would compound shadow_darken to near-black).
		int spanx = (int)ceilf(fabsf(lx) * anchor), spany = (int)ceilf(ly * anchor);
		int minx = screen_x + (lx < 0 ? -spanx : 0) - 1;
		int maxx = screen_x + fw + (lx < 0 ? 0 : spanx) + 1;
		int miny = anchor_y - 1, maxy = anchor_y + spany + 1;
		int mw = maxx - minx, mh = maxy - miny;
		if (mw <= 0 || mh <= 0) return;
		shadow_dst_mask.assign((size_t)mw * mh, 0);
		for (int sy = 0; sy < fh; ++sy) {
			int h = anchor - sy;                      // height above the contact line
			int dxoff = (int)lroundf(h * lx);
			int dyoff = (int)lroundf(h * ly);
			const uint32_t* srow = shadow_shear_scratch.data() + (size_t)sy * fw;
			int my = (anchor_y + dyoff) - miny;
			if (my < 0 || my >= mh) continue;
			uint8_t* mrow = shadow_dst_mask.data() + (size_t)my * mw;
			for (int sx = 0; sx < fw; ++sx) {
				if (!srow[sx]) continue;
				int mx = (screen_x + sx + dxoff) - minx;
				if (mx >= 0 && mx < mw) mrow[mx] = 1;
			}
		}
		// Darken each covered logical pixel once, expanding to the ws x ws physical block.
		const int sw = (int)screen_width, sh = (int)screen_height;
		for (int my = 0; my < mh; ++my) {
			const uint8_t* mrow = shadow_dst_mask.data() + (size_t)my * mw;
			int ly0 = (miny + my) * ws;
			for (int mx = 0; mx < mw; ++mx) {
				if (!mrow[mx]) continue;
				int lx0 = (minx + mx) * ws;
				for (int oy = 0; oy < ws; ++oy) {
					int py = ly0 + oy;
					if (py < 0 || py >= sh) continue;
					uint32_t* prow = data + (size_t)py * data_pitch;
					for (int ox = 0; ox < ws; ++ox) {
						int px = lx0 + ox;
						if (px < 0 || px >= sw) continue;
						prow[px] = shadow_darken(prow[px]);
					}
				}
			}
		}
	}

	// SHADOWS Phase 2: additively splat the frame's point lights onto the world surface
	// (called after sprites, before fog). Each light is a soft radial glow: att = intensity
	// * (1 - d^2/r^2), clamped, added to the destination. Screen-space, read-only from
	// frame_lights (never touches sim). Radius/count are capped by the sourcing pass.
	void draw_point_lights(uint32_t* data, size_t data_pitch) {
		const int sw = (int)screen_width, sh = (int)screen_height;
		for (auto& L : frame_lights) {
			int rad = L.radius;
			if (rad < 2 || L.intensity <= 0) continue;
			int r2 = rad * rad;
			uint32_t lr = (L.rgb >> 16) & 0xff, lg = (L.rgb >> 8) & 0xff, lb = L.rgb & 0xff;
			int y0 = L.y - rad, y1 = L.y + rad + 1, x0 = L.x - rad, x1 = L.x + rad + 1;
			if (y0 < 0) y0 = 0; if (y1 > sh) y1 = sh;
			if (x0 < 0) x0 = 0; if (x1 > sw) x1 = sw;
			for (int y = y0; y < y1; ++y) {
				int dy = y - L.y;
				int dy2 = dy * dy;
				uint32_t* drow = data + (size_t)y * data_pitch;
				for (int x = x0; x < x1; ++x) {
					int dx = x - L.x;
					int d2 = dx * dx + dy2;
					if (d2 >= r2) continue;
					int att = L.intensity * (r2 - d2) / r2;   // intensity at center -> 0 at edge
					if (att <= 0) continue;
					uint32_t old = drow[x];
					uint32_t orr = (old >> 16) & 0xff, og = (old >> 8) & 0xff, ob = old & 0xff;
					orr += lr * att / 255; if (orr > 255) orr = 255;
					og  += lg * att / 255; if (og  > 255) og  = 255;
					ob  += lb * att / 255; if (ob  > 255) ob  = 255;
					drow[x] = 0xff000000u | (orr << 16) | (og << 8) | ob;
				}
			}
		}
	}

	// Faze 7 perf: decoded 8-bit GRP-frame scratch cache for the world_scale>1 SD
	// sprite path (blit_grp_frame_ws). The RLE->index decode depends only on
	// (frame, flipped), NOT on ws/position/remap, so caching it skips a full RLE
	// re-decode per cloak/blend/glow/shadow/selection sprite each frame; the
	// per-call upscale+remap loop still runs (the remap differs per call).
	struct grp_scratch_t { std::vector<uint8_t> px; size_t bytes = 0; uint64_t used = 0; int frame = -1; };
	std::map<std::pair<const void*, bool>, grp_scratch_t> grp_ws_cache;
	size_t grp_ws_cache_bytes = 0;
	bool grp_ws_trim_logged = false;
	bool hd_tiles_tried = false;
	bool hd_terrain_active = false;

	// open1161HD Faze 4.2: HD assets straight from the SC:R CASC (HD2 tier).
	// When hd_casc is set and no disk-mod HD dir is present, the HD anim/tileset
	// loaders pull bytes by CASC name (HD2/anim/main_NNN.anim,
	// HD2/tileset/<name>.dds.vr4) via load_hd_bytes instead of fopen. This is a
	// byte SOURCE swap only: the .anim parser and BC1/BC3 DDS decode are
	// unchanged (dxtcheck 1425/1425 byte-exact, CASC<->disk bytes identical -
	// ASSET-GUIDE 1.1/4.2), so nothing about the decoded pixels changes.
	std::function<bool(const a_string&, std::vector<uint8_t>&)> load_hd_bytes;
	// Faze 4.6: cheap CASC name existence check (has_name; no byte read). Used to
	// measure HD portrait coverage across the whole roster and to resolve portrait
	// webm variants without loading each candidate's bytes.
	std::function<bool(const a_string&)> hd_file_exists;
	// Faze 4.7 Variant B: hardware WebM cinematic decode (Android AMediaCodec). Set
	// by the glue; null on host/unsupported. play_cinematic tries these before SMK.
	// open returns an opaque handle (or null) + fills w/h/usf; next fills an RGB
	// frame (w*h*3) and returns false at EOF; close frees the handle.
	std::function<void*(const std::vector<uint8_t>&, int&, int&, double&)> hd_cine_open;
	std::function<bool(void*, std::vector<uint8_t>&)> hd_cine_next;
	std::function<void(void*)> hd_cine_close;
	// Faze 4.7 Variant A: play a WebM full-screen through the OS video player,
	// blocking until it finishes or is skipped. Returns true if it played. Set by
	// the Android glue (null on host); play_hd_cinematic prefers it over hd_cine_*.
	std::function<bool(const std::vector<uint8_t>&)> hd_cine_play_java;
	bool hd_casc = false;
	// CASC HD asset tier prefix: "HD2/" = half-res HD (scale 2, default), "" = full
	// HD (scale 4). SD tier just leaves hd_casc false. All HD CASC lookups prepend
	// this so the graphics setting can switch tiers without touching call sites.
	a_string hd_casc_prefix = "HD2/";
	bool hd_anim_diag = false; // DIAG: log each image's HD-anim load result (per-unit SD fallback)
	// open1161HD Faze 4.3: world supersample factor. 1 = classic (buffer == map
	// view; SD/HD2 stay here). >1 (FULL HD) = the buffer is world_scale x the map
	// view, so the RGBA world draws multiply screen coords + sizes by view_scale
	// and fill it at full detail while the same map area stays visible. The HUD
	// keeps drawing at screen_width (it is resolution-responsive). Only the RGBA/HD
	// world path scales; the 8-bit path is skipped under HD terrain anyway.
	int world_scale = 1;

	void load_hd_tiles() {
		if (hd_tiles_tried) return;
		hd_tiles_tried = true;
		if (hd_tileset_dir.empty() && !hd_casc) return;
		static const char* names[8] = {"badlands", "platform", "install", "ashworld", "jungle", "desert", "ice", "twilight"};
		size_t ti = (size_t)game_st.tileset_index;
		if (ti >= 8) return;
		if (!hd_tileset_dir.empty()) {
			// Disk-mod tileset/<name>.dds.vr4 takes priority over CASC.
			a_string path = hd_tileset_dir + "/" + names[ti] + ".dds.vr4";
			FILE* f = fopen(path.c_str(), "rb");
			if (!f) { ui::log("hd tiles: no %s\n", path.c_str()); return; }
			fseek(f, 0, SEEK_END);
			long fsz = ftell(f);
			fseek(f, 0, SEEK_SET);
			hd_tiles.raw.resize((size_t)fsz);
			bool read_ok = fread(hd_tiles.raw.data(), 1, (size_t)fsz, f) == (size_t)fsz;
			fclose(f);
			if (!read_ok) { hd_tiles.raw.clear(); return; }
		} else {
			// open1161HD Faze 4.2: HD2 tileset straight from CASC. Same .dds.vr4
			// container the disk path parses; bytes are CASC<->disk identical.
			a_string name = hd_casc_prefix + "tileset/" + names[ti] + ".dds.vr4";
			if (!load_hd_bytes || !load_hd_bytes(name, hd_tiles.raw)) {
				ui::log("hd tiles: no CASC %s\n", name.c_str());
				hd_tiles.raw.clear();
				return;
			}
		}
		if (hd_tiles.raw.size() < 12) { hd_tiles.raw.clear(); return; }
		long sz = (long)hd_tiles.raw.size();
		const uint8_t* d = hd_tiles.raw.data();
		int count = anim::detail::rd16(d + 4);
		size_t pos = 12;
		hd_tiles.ents.reserve(count);
		for (int i = 0; i != count; ++i) {
			if (pos + 8 > (size_t)sz) break;
			uint32_t dsz = anim::detail::rd32(d + pos + 4);
			pos += 8;
			if (pos + dsz > (size_t)sz) break;
			hd_tiles.ents.push_back({(uint32_t)pos, dsz});
			pos += dsz + 4; // data + u32 trailer
		}
		hd_tiles.ok = !hd_tiles.ents.empty();
		ui::log("hd tiles: %s %s -> %d megatiles\n", hd_tileset_dir.empty() ? "CASC" : "disk",
			names[ti], (int)hd_tiles.ents.size());
	}

	const std::vector<uint32_t>* get_hd_tile(size_t megatile) {
		if (!hd_tiles.ok || megatile >= hd_tiles.ents.size()) return nullptr;
		auto it = hd_tiles.cache.find(megatile);
		if (it != hd_tiles.cache.end()) {
			it->second.used = ++hd_cache_tick;      // touch for LRU recency
			it->second.frame = hd_cache_frame();
			return &it->second.rgba;
		}
		auto& e = hd_tiles.ents[megatile];
		if (!e.size) return nullptr;
		ddsgrp::frame_t full;
		int tw = 0, th = 0;
		if (!anim::detail::decode_dds(hd_tiles.raw.data() + e.off, e.size, full.rgba, tw, th)) return nullptr;
		full.w = tw;
		full.h = th;
		// open1161HD Faze 4.3: at world_scale>1 keep the megatile at 32*ws so the HD
		// source detail reaches the (physical-res) buffer 1:1 instead of crushed then upscaled.
		int ws = world_scale < 1 ? 1 : world_scale;
		ddsgrp::frame_t small = ddsgrp::scale_box(full, 32 * ws, 32 * ws);
		// Faze 7 perf: insert with byte accounting + recency, then trim to a 64 MB
		// budget via the shared LRU (drops the LEAST-recently-used entries not touched
		// this frame) -- no more full cache.clear() decode-storm at the cap.
		auto& slot = hd_tiles.cache[megatile];
		slot.bytes = small.rgba.size() * sizeof(uint32_t);
		slot.rgba = std::move(small.rgba);
		slot.used = ++hd_cache_tick;
		slot.frame = hd_cache_frame();
		hd_tiles_cache_bytes += slot.bytes;
		trim_hd_cache(hd_tiles.cache, hd_tiles_cache_bytes, (size_t)64 << 20, "tiles", hd_tiles_trim_logged);
		return &hd_tiles.cache[megatile].rgba; // just-inserted (frame==now) is protected from trim
	}

	// HD terrain overdraw on the RGBA surface (before sprites/fog). Mirrors
	// draw_tiles incl. the creep megatile substitution and creep edges.
	void draw_tiles_hd(uint32_t* data, size_t data_pitch) {
		load_hd_tiles();
		if (!hd_tiles.ok) return;
		auto screen_tile = screen_tile_bounds();
		size_t tile_index = screen_tile.from.y * game_st.map_tile_width + screen_tile.from.x;
		auto* megatile_index = &st.tiles_mega_tile_index[tile_index];
		auto* tile = &st.tiles[tile_index];
		size_t width = screen_tile.to.x - screen_tile.from.x;
		xy dirs[9] = {{1, 1}, {0, 1}, {-1, 1}, {1, 0}, {-1, 0}, {1, -1}, {0, -1}, {-1, -1}, {0, 0}};
		const int ws = world_scale < 1 ? 1 : world_scale;   // frame-constant, hoisted out of the tile loops
		const int ts = 32 * ws; // buffer px per map tile
		for (size_t tile_y = screen_tile.from.y; tile_y != screen_tile.to.y; ++tile_y) {
			for (size_t tile_x = screen_tile.from.x; tile_x != screen_tile.to.x; ++tile_x) {
				int screen_x = (tile_x * 32 - screen_pos.x) * ws;
				int screen_y = (tile_y * 32 - screen_pos.y) * ws;
				size_t index = *megatile_index;
				if (tile->flags & tile_t::flag_has_creep) {
					index = game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[tile_x + tile_y * game_st.map_tile_width]];
				}
				int x0 = screen_x < 0 ? -screen_x : 0;
				int y0 = screen_y < 0 ? -screen_y : 0;
				int w = std::min(ts, (int)screen_width - screen_x);
				int h = std::min(ts, (int)screen_height - screen_y);
				if (const std::vector<uint32_t>* t = get_hd_tile(index)) {
					// Faze 7: defensive width guard. w>x0 holds today (floor view_width
					// keeps every visible tile on-screen), but a future ceil/+1 change
					// would make (w-x0) negative -> a (size_t) multi-GB memcpy / heap smash.
					if (w > x0) for (int y = y0; y < h; ++y) {
						memcpy(data + (size_t)(screen_y + y) * data_pitch + screen_x + x0,
						    t->data() + (size_t)y * ts + x0, (size_t)(w - x0) * 4);
					}
				} else {
					// SD fallback (the indexed tile pass is skipped when HD
					// terrain is on, so paint the paletted megatile here). Nearest-
					// scale the 32x32 tile up to ts when world_scale>1.
					uint8_t scratch[32 * 32];
					memset(scratch, 0, sizeof(scratch));
					draw_tile(tileset_img, index, scratch, 32, 0, 0, 32, 32);
					for (int y = y0; y < h; ++y) {
						uint32_t* drow = data + (size_t)(screen_y + y) * data_pitch + screen_x;
						const uint8_t* srow = scratch + (size_t)(y / ws) * 32;
						for (int x = x0; x < w; ++x) drow[x] = pal_rgba(srow[x / ws]);
					}
				}
				if (~tile->flags & tile_t::flag_has_creep) {
					size_t creep_index = 0;
					for (size_t i = 0; i != 9; ++i) {
						int add_x = dirs[i].x;
						int add_y = dirs[i].y;
						if (tile_x + add_x >= game_st.map_tile_width) continue;
						if (tile_y + add_y >= game_st.map_tile_height) continue;
						if (st.tiles[tile_x + add_x + (tile_y + add_y) * game_st.map_tile_width].flags & tile_t::flag_has_creep) creep_index |= 1 << i;
					}
					size_t creep_frame = img.creep_edge_frame_index[creep_index];
					if (creep_frame) {
						auto& frame = tileset_img.creep_grp.frames.at(creep_frame - 1);
						if (ws > 1) {
							// Faze 4.3: scale the soft creep edge to the x ws world,
							// or zerg terrain shows hard creep tile borders. Anchor
							// is LOGICAL (blit_grp_frame_ws multiplies by ws).
							int lx = (int)(tile_x * 32 - screen_pos.x) + frame.offset.x;
							int ly = (int)(tile_y * 32 - screen_pos.y) + frame.offset.y;
							blit_grp_frame_ws(frame, false, lx, ly, ws, data, data_pitch,
								[this](uint8_t c, uint32_t&) { return pal_rgba(c); });
						} else {
							int cx = screen_x + frame.offset.x;
							int cy = screen_y + frame.offset.y;
							if (cx < (int)screen_width && cy < (int)screen_height && cx + (int)frame.size.x > 0 && cy + (int)frame.size.y > 0) {
								size_t offset_x = cx < 0 ? -cx : 0;
								size_t offset_y = cy < 0 ? -cy : 0;
								uint32_t* dst = data + (size_t)cy * data_pitch + cx;
								size_t w = std::min((size_t)frame.size.x, screen_width - cx);
								size_t h = std::min((size_t)frame.size.y, screen_height - cy);
								draw_frame_rgba(frame, false, dst, data_pitch, offset_x, offset_y, w, h, [&](uint8_t c, uint32_t&) {
									return pal_rgba(c);
								});
							}
						}
					}
				}
				++megatile_index;
				++tile;
			}
			megatile_index -= width;
			megatile_index += game_st.map_tile_width;
			tile -= width;
			tile += game_st.map_tile_width;
		}
	}

	ddsgrp::frame_t* get_icon_override(int frame_index) {
		if (icons_png_dir.empty() && hd_ui_dir.empty() && !hd_casc) return nullptr;
		auto it = icon_override_cache.find(frame_index);
		if (it == icon_override_cache.end()) {
			int bw = cmdicons_ok ? (int)cmdicons_grp.width : 36;
			int bh = cmdicons_ok ? (int)cmdicons_grp.height : 34;
			ddsgrp::frame_t f;
			if (!icons_png_dir.empty()) {
				char buf[32];
				std::snprintf(buf, sizeof(buf), "/%03d.png", frame_index);
				f = ddsgrp::load_png((icons_png_dir + buf).c_str());
			}
			if (!f.w && (!hd_ui_dir.empty() || hd_casc)) {
				if (!hd_cmdicons_dds_tried) {
					hd_cmdicons_dds_tried = true;
					if (!hd_ui_dir.empty()) {
						hd_cmdicons_dds = ddsgrp::load_file((hd_ui_dir + "/cmdicons.dds.grp").c_str());
					} else {
						// open1161HD Faze 4.2d: HD command icons from CASC.
						std::vector<uint8_t> tmp;
						if (load_hd_bytes && load_hd_bytes(hd_casc_prefix + "unit/cmdicons/cmdicons.dds.grp", tmp) && !tmp.empty())
							hd_cmdicons_dds = ddsgrp::parse(tmp.data(), tmp.size());
					}
					ui::log("hd icons: %s -> %d frames (ok=%d)\n", hd_ui_dir.empty() ? "CASC" : hd_ui_dir.c_str(), (int)hd_cmdicons_dds.frame_count(), (int)hd_cmdicons_dds.ok);
				}
				if (hd_cmdicons_dds.ok && frame_index >= 0 && frame_index < (int)hd_cmdicons_dds.frame_count()) {
					// Decoded on demand and dropped again below: what we keep is
					// the scaled icon, not the HD source.
					ddsgrp::frame_t src = hd_cmdicons_dds.decode((size_t)frame_index);
					if (src.w) f = std::move(src);
				}
			}
			cached_frame_t e;
			if (f.w) {
				// open1161HD Faze 4.7d: keep the HD/PNG icon at (near-)native
				// resolution. The old code crushed it to the SD cmdicons_grp size
				// (bw x bh = 36 x 34), so the "HD" command icons were really SD
				// upscaled -- crisp source thrown away, then re-magnified by
				// draw_cmdicon_fit onto the (large) HD cell. Cap the long side at
				// 128 px to bound the cache; still far above any cell.
				bool hd_src = !icons_png_dir.empty() || !hd_ui_dir.empty() || hd_casc;
				int cap = 128;
				if (hd_src && (f.w > cap || f.h > cap)) {
					int dw = f.w >= f.h ? cap : cap * f.w / f.h;
					int dh = f.h >= f.w ? cap : cap * f.h / f.w;
					e.f = ddsgrp::scale_box(f, dw, dh);
				} else if (hd_src) {
					e.f = std::move(f);
				} else {
					e.f = ddsgrp::scale_box(f, bw, bh);
				}
			}
			e.bytes = e.f.rgba.size() * 4;
			icon_override_bytes += e.bytes;
			it = icon_override_cache.emplace(frame_index, std::move(e)).first;
		}
		it->second.used = ++hd_cache_tick;
		it->second.frame = hd_cache_frame();
		ddsgrp::frame_t* r = it->second.f.w ? &it->second.f : nullptr;
		trim_hd_cache(icon_override_cache, icon_override_bytes, (size_t)8 << 20,
			"icons", icon_override_trim_logged);
		return r;
	}

	// Faze 4.7d: HD resource/status icons (game/icons.dds.grp), replacing the SD
	// game\icons.grp in the resource readout when HD assets are active.
	ddsgrp::file_t hd_res_icons_dds;
	bool hd_res_icons_tried = false;
	std::map<int, ddsgrp::frame_t> hd_res_icon_cache;
	ddsgrp::frame_t* get_hd_resource_icon(int frame) {
		if (!hd_casc && hd_ui_dir.empty()) return nullptr;
		if (!hd_res_icons_tried) {
			hd_res_icons_tried = true;
			if (!hd_ui_dir.empty()) {
				hd_res_icons_dds = ddsgrp::load_file((hd_ui_dir + "/icons.dds.grp").c_str());
			} else {
				std::vector<uint8_t> tmp;
				if (load_hd_bytes && load_hd_bytes(hd_casc_prefix + "game/icons.dds.grp", tmp) && !tmp.empty())
					hd_res_icons_dds = ddsgrp::parse(tmp.data(), tmp.size());
			}
			ui::log("hd resource icons: %d frames (ok=%d)\n", (int)hd_res_icons_dds.frame_count(), (int)hd_res_icons_dds.ok);
		}
		if (!hd_res_icons_dds.ok || frame < 0 || frame >= (int)hd_res_icons_dds.frame_count()) return nullptr;
		auto it = hd_res_icon_cache.find(frame);
		if (it == hd_res_icon_cache.end())
			it = hd_res_icon_cache.emplace(frame, hd_res_icons_dds.decode((size_t)frame)).first;
		return it->second.w ? &it->second : nullptr;
	}
	// Blit an HD (RGBA) icon fit to target_h, aspect-preserved, alpha-blended.
	void blit_hd_icon(uint32_t* data, size_t pitch, const ddsgrp::frame_t& f, int x, int y, int target_h) {
		if (f.w <= 0 || f.h <= 0 || target_h <= 0) return;
		int dh = target_h, dw = f.w * target_h / f.h;
		for (int yy = 0; yy < dh; ++yy) {
			int py = y + yy; if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint32_t* srow = &f.rgba[(size_t)((int64_t)yy * f.h / dh) * f.w];
			for (int xx = 0; xx < dw; ++xx) {
				int px = x + xx; if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c = srow[(int64_t)xx * f.w / dw];
				uint32_t a = c >> 24; if (a < 8) continue;
				if (a >= 248) { row[px] = 0xff000000u | (c & 0xffffff); continue; }
				uint32_t old = row[px], ia = 255 - a;
				uint32_t r = (((c >> 16) & 0xff) * a + ((old >> 16) & 0xff) * ia) / 255;
				uint32_t g = (((c >> 8) & 0xff) * a + ((old >> 8) & 0xff) * ia) / 255;
				uint32_t b = ((c & 0xff) * a + (old & 0xff) * ia) / 255;
				row[px] = 0xff000000u | (r << 16) | (g << 8) | b;
			}
		}
	}

	// Load main_NNN.anim by number from disk or CASC at the current tier/world_scale.
	anim::sprite_t load_hd_anim_num(int num) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "main_%03d.anim", num);
		if (!hd_anim_dir.empty())
			return anim::load_file((hd_anim_dir + "/" + buf).c_str(), world_scale);
		std::vector<uint8_t> tmp;
		a_string name = hd_casc_prefix + "anim/" + buf;
		if (load_hd_bytes && load_hd_bytes(name, tmp) && !tmp.empty())
			return anim::parse(tmp.data(), tmp.size(), world_scale);
		return anim::sprite_t{};
	}
	anim::sprite_t* get_hd_anim(int image_id, int grp_index = -1) {
		if (hd_anim_dir.empty() && !hd_casc) return nullptr;
		auto it = hd_anims.find(image_id);
		if (it == hd_anims.end()) {
			// Not resolved yet. Rather than block the render thread on the CASC
			// fetch + parse (and, for hero/alt ids, a whole sibling scan of MANY
			// candidate anims -- the ~1s PERF_HITCH), hand the image_id to the
			// background worker and draw the SD sprite this frame. hd_prefetch_drain
			// installs the HD result into hd_anims a few frames later, keying on the
			// SAME images.dat image id the SD chain resolves (oracle sd_hd_bridge.py:
			// 214/214, 0 mismatch). hd_pf_inflight de-dups so each id enqueues once;
			// on completion a miss is cached (bytes==0, never re-enqueued) exactly
			// like the old synchronous path. Diag/logging moved to the drain (main
			// thread) so the worker only touches CASC + parse + the queues.
			if (!hd_pf_inflight.count(image_id)) {
				hd_prefetch_start();
				hd_pf_inflight.insert(image_id);
				{
					std::lock_guard<std::mutex> lk(hd_pf_mtx);
					hd_pf_queue.emplace_back(image_id, grp_index);
				}
				hd_pf_cv.notify_one();
			}
			return nullptr;   // SD fallback until the worker's result is drained in
		}
		it->second.used = ++hd_cache_tick;
		it->second.frame = hd_cache_frame();
		anim::sprite_t* sp = it->second.sp.ok ? &it->second.sp : nullptr;
		trim_hd_cache(hd_anims, hd_anims_bytes, (size_t)64 << 20, "anims", hd_anims_trim_logged);
		return sp;
	}

	static uint32_t rgba_blend50(uint32_t a, uint32_t b) {
		return 0xff000000u | (((a & 0xfefefe) >> 1) + ((b & 0xfefefe) >> 1));
	}
	static uint32_t rgba_add_sat(uint32_t dstc, uint32_t add) {
		uint32_t r = ((dstc >> 16) & 0xff) + ((add >> 16) & 0xff);
		uint32_t g = ((dstc >> 8) & 0xff) + ((add >> 8) & 0xff);
		uint32_t b = (dstc & 0xff) + (add & 0xff);
		if (r > 255) r = 255;
		if (g > 255) g = 255;
		if (b > 255) b = 255;
		return 0xff000000u | (r << 16) | (g << 8) | b;
	}
	uint32_t pal_rgba(uint8_t index) {
		return 0xff000000u | game_palette[index];
	}

	// Draw one HD atlas frame. The HD canvas is anchored at the image center
	// (sprite position + image offset); frame offsets are relative to the
	// canvas top-left, so top-left = center - canvas/2 + frame offset.
	void draw_image_hd(const image_t* image, anim::sprite_t& ha, uint32_t* data, size_t data_pitch, size_t color_index) {
		auto& f = ha.frames[image->frame_index];
		bool flipped = i_flag(image, image_t::flag_horizontally_flipped);

		xy center = image->sprite->position + image->offset + interp_off_for(image->sprite);
		// Anchor math in HD units, one floor-division at the end - per-field
		// truncation made animated buildings jitter between frames.
		int u = ha.hd_per_logical;
		// round-to-nearest: halves the residual per-frame anchor wobble that
		// floor-division left on slow idle animations (zerg hatchery "bob")
		auto fdiv = [u](int v) { return v >= 0 ? (v + u / 2) / u : -((-v + u / 2) / u); };
		int x_off_hd = flipped ? ha.canvas_w_hd / 2 - (f.x_off_hd + f.w_hd) : f.x_off_hd - ha.canvas_w_hd / 2;
		int ws = world_scale < 1 ? 1 : world_scale;
		int screen_x = (center.x + fdiv(x_off_hd) - screen_pos.x) * ws;
		int screen_y = (center.y + fdiv(f.y_off_hd - ha.canvas_h_hd / 2) - screen_pos.y) * ws;
		if (i_flag(image, image_t::flag_y_frozen)) screen_y = (image->frozen_y_value - screen_pos.y) * ws;
		// f.w/f.h index the world_scale x logical atlas (the anim was parsed at
		// world_scale), so the frame draws 1:1 at real detail - only the anchor
		// position is x ws above.

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + f.w <= 0 || screen_y + f.h <= 0) return;

		int offset_x = screen_x < 0 ? -screen_x : 0;
		int offset_y = screen_y < 0 ? -screen_y : 0;
		int width = std::min(f.w, (int)screen_width - screen_x);
		int height = std::min(f.h, (int)screen_height - screen_y);

		uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
		uint32_t pc = game_palette[color_ptr[0]];
		uint32_t pr = (pc >> 16) & 0xff, pg = (pc >> 8) & 0xff, pb = pc & 0xff;
		bool has_tc = !ha.teamcolor.empty();
		bool has_em = hd_emissive && !ha.emissive.empty(); // Faze 7 self-glow
		// SHADOWS Phase 2: aggregate this sprite's emissive energy into a point light
		// (only when point lighting is on -- otherwise these accumulators cost nothing).
		bool acc_light = hd_point_lights && has_em;
		uint64_t li_sum = 0, li_cx = 0, li_cy = 0, li_r = 0, li_g = 0, li_b = 0;

		uint32_t* dst = data + (size_t)screen_y * data_pitch + screen_x;
		for (int y = offset_y; y < height; ++y) {
			const size_t arow = (size_t)(f.y + y) * ha.atlas_w;
			uint32_t* drow = dst + (size_t)y * data_pitch;
			for (int x = offset_x; x < width; ++x) {
				size_t ai = arow + f.x + (flipped ? f.w - 1 - x : x);
				uint32_t c = ha.diffuse[ai];
				uint32_t a = c >> 24;
				if (a < 8) continue;
				uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
				if (has_tc) {
					uint32_t m = ha.teamcolor[ai];
					if (m) {
						// Tint: keep the diffuse shading (luma) but take the
						// player color hue. Straight addition washes out pink.
						uint32_t l = (r * 77 + g * 150 + b * 29) >> 8;
						uint32_t tr = pr * l / 128; if (tr > 255) tr = 255;
						uint32_t tg = pg * l / 128; if (tg > 255) tg = 255;
						uint32_t tb = pb * l / 128; if (tb > 255) tb = 255;
						uint32_t im = 255 - m;
						r = div255(r * im + tr * m);
						g = div255(g * im + tg * m);
						b = div255(b * im + tb * m);
					}
				}
				uint32_t old = drow[x];
				if (a < 248) {
					uint32_t ia = 255 - a;
					r = div255(r * a + ((old >> 16) & 0xff) * ia);
					g = div255(g * a + ((old >> 8) & 0xff) * ia);
					b = div255(b * a + (old & 0xff) * ia);
				}
				// Faze 7: additive self-glow (SC:R emissive layer) on top of the
				// composited pixel -- weapon charges / psi / warp / shields brighten.
				if (has_em) {
					uint32_t ec = ha.emissive[ai];
					uint32_t ea = ec >> 24;
					if (ea) {
						r += div255(((ec >> 16) & 0xff) * ea); if (r > 255) r = 255;
						g += div255(((ec >> 8) & 0xff) * ea); if (g > 255) g = 255;
						b += div255((ec & 0xff) * ea); if (b > 255) b = 255;
						if (acc_light) {
							// (screen_x + x) / (screen_y + y) are >= 0 and < screen dims here:
							// offset_x/offset_y clamp the loop start so a partly-off-screen
							// sprite never yields negative coords (else the uint64 centroid
							// accumulation would blow up). Keep that invariant if the clip changes.
							li_sum += ea;
							li_cx += (uint64_t)ea * (screen_x + x);
							li_cy += (uint64_t)ea * (screen_y + y);
							li_r += (uint64_t)((ec >> 16) & 0xff) * ea;
							li_g += (uint64_t)((ec >> 8) & 0xff) * ea;
							li_b += (uint64_t)(ec & 0xff) * ea;
						}
					}
				}
				drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
			}
		}
		// Emit one point light for a sprite with enough glow. Threshold skips the tiny
		// residual glows (shield rims, idle blinkenlights); real spells/explosions/weapon
		// charges clear it. Radius/intensity scale with the accumulated energy (sqrt so a
		// big blast isn't absurdly large) and the user strength knob. In LOGICAL px * ws.
		if (acc_light && li_sum >= 2000) {
			light_source_t L;
			L.x = (int)(li_cx / li_sum);
			L.y = (int)(li_cy / li_sum);
			uint32_t lr = (uint32_t)(li_r / li_sum), lg = (uint32_t)(li_g / li_sum), lb = (uint32_t)(li_b / li_sum);
			L.rgb = (lr << 16) | (lg << 8) | lb;
			int e = (int)(li_sum / 256);                 // energy in ~pixel-alpha units
			int base = (int)(8.0 * std::sqrt((double)e)); // px, at world_scale=1
			L.radius = base * (world_scale < 1 ? 1 : world_scale) * hd_light_strength / 100;
			if (L.radius > 128) L.radius = 128;   // perf cap (full-res splat; quarter-res map = Phase 2b)
			L.intensity = 160 * hd_light_strength / 100;
			if (L.intensity > 255) L.intensity = 255;
			if (L.radius >= 4 && frame_lights.size() < 64) frame_lights.push_back(L);
		}
	}

	void draw_image_rgba(const image_t* image, uint32_t* data, size_t data_pitch, size_t color_index) {
		// HD path: normal/player-color images with a matching HD sprite.
		if ((!hd_anim_dir.empty() || hd_casc) && (image->modifier == 0 || image->modifier == 1)) {
			auto* ha = get_hd_anim((int)image->image_type->id, (int)image->image_type->grp_filename_index);
			if (ha && image->frame_index < ha->frames.size()) {
				draw_image_hd(image, *ha, data, data_pitch, color_index);
				return;
			}
		}
		int ws = world_scale < 1 ? 1 : world_scale;
		xy map_pos = get_image_map_position(image) + interp_off_for(image->sprite);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		// Faze 7 perf: screen_x/y are LOGICAL map px; the blit scales by ws. Cull in
		// LOGICAL space (screen_width/ws == view_width). The old check compared logical
		// coords against the PHYSICAL width, so at ws=2 the right/bottom cull fired at
		// 2x the real edge -> off-screen sprites still entered blit_grp_frame_ws and
		// looped fw*ws x fh*ws pixels, each skipped per-pixel. ws=1 is unchanged.
		if (screen_x >= (int)screen_width / ws || screen_y >= (int)screen_height / ws) return;

		auto& frame = image->grp->frames.at(image->frame_index);

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
		}

		uint32_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		bool flipped = i_flag(image, image_t::flag_horizontally_flipped);
		uint8_t* color_ptr = img.player_unit_colors.at(color_index).data();
		auto pcol = [color_ptr](uint8_t v) {
			if (v >= 8 && v < 16) return color_ptr[v - 8];
			return v;
		};

		// Faze 4.3: at world_scale>1 EVERY SD sprite-path modifier must scale and
		// position at x ws, or the effect renders shrunken and offset up-left over
		// the x ws world (the "VFX offset left" bug). blit_grp_frame_ws decodes the
		// frame to an 8-bit scratch and nearest-scale-blits it at the x ws origin,
		// applying the same per-pixel remap the 1x path uses (the blend/additive
		// modifiers read the already-scaled destination pixel). screen_x/screen_y
		// are LOGICAL (map - screen_pos); blit_grp_frame_ws multiplies by ws.
		if (ws > 1) {
			int m = image->modifier;
			if (m == 0 || m == 1) {
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[&](uint8_t c, uint32_t&) { return pal_rgba(pcol(c)); });
				return;
			} else if (m == 2 || m == 4) {
				uint8_t* selector = tileset_img.cloak_fade_selector.data();
				int value = image->modifier_data1;
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[&](uint8_t c, uint32_t& old) {
						uint32_t col = pal_rgba(pcol(c));
						return selector[c] <= value ? rgba_blend50(old, col) : col;
					});
				return;
			} else if (m == 3) {
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[&](uint8_t c, uint32_t& old) { return rgba_blend50(old, pal_rgba(pcol(c))); });
				return;
			} else if (m == 10) {
				if (hd_shadow_dir) { draw_shadow_sheared(frame, flipped, screen_x, screen_y, ws, data, data_pitch); return; }
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[this](uint8_t, uint32_t& old) { return shadow_darken(old); });
				return;
			} else if (m == 9) {
				auto& tdata = tileset_img.light_pcx.at(image->image_type->color_shift - 1).data;
				uint8_t* tptr = tdata.data();
				size_t tsize = tdata.size() / 256;
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[&](uint8_t c, uint32_t& old) -> uint32_t {
						uint8_t nv = (uint8_t)(c - 1);
						if (nv >= tsize) return old;
						return rgba_add_sat(old, game_palette[tptr[256u * nv]]);
					});
				return;
			} else if (m == 17) {
				blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
					[&](uint8_t, uint32_t& old) { return rgba_add_sat(old, 0x00583818); });
				return;
			} else if (m == 12) {
				// Warp-in: sample the warp texture per source pixel, gated by the
				// sprite shape. Noise texture, so flip is imperceptible - decode both
				// unflipped for simplicity and place at the x ws origin.
				int fw = (int)frame.size.x, fh = (int)frame.size.y;
				if (fw > 0 && fh > 0) {
					if (temporary_warp_texture_buffer.size() < (size_t)fw * fh) temporary_warp_texture_buffer.resize((size_t)fw * fh);
					auto& texture_frame = global_st.image_grp[(size_t)ImageTypes::IMAGEID_Warp_Texture]->frames.at(image->modifier_data1);
					draw_frame(texture_frame, false, temporary_warp_texture_buffer.data(), (size_t)fw, 0, 0, (size_t)fw, (size_t)fh);
					static std::vector<uint8_t> mask;
					mask.assign((size_t)fw * fh, 0);
					draw_frame(frame, false, mask.data(), (size_t)fw, 0, 0, (size_t)fw, (size_t)fh);
					uint8_t* tex = temporary_warp_texture_buffer.data();
					int bx = screen_x * ws, by = screen_y * ws;
					for (int y = 0; y < fh * ws; ++y) {
						int dy = by + y; if (dy < 0 || dy >= (int)screen_height) continue;
						uint32_t* drow = data + (size_t)dy * data_pitch;
						int sy = y / ws;
						for (int x = 0; x < fw * ws; ++x) {
							int dx = bx + x; if (dx < 0 || dx >= (int)screen_width) continue;
							int sx = x / ws;
							if (mask[(size_t)sy * fw + sx] == 0) continue;
							drow[dx] = pal_rgba(tex[(size_t)sy * fw + sx]);
						}
					}
				}
				return;
			}
			// mod 8 (distortion) + anything else: draw opaque at x ws so it is at
			// least correctly placed and sized (visual is approximate but rare).
			blit_grp_frame_ws(frame, flipped, screen_x, screen_y, ws, data, data_pitch,
				[&](uint8_t c, uint32_t&) { return pal_rgba(pcol(c)); });
			return;
		}

		if (image->modifier == 0 || image->modifier == 1) {
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t&) {
				return pal_rgba(pcol(c));
			});
		} else if (image->modifier == 2 || image->modifier == 4) {
			// Cloak: pixels the fade selector has reached blend 50% with the
			// scene (the 8-bit path does a two-pass table fade).
			uint8_t* selector = tileset_img.cloak_fade_selector.data();
			int value = image->modifier_data1;
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t& old) {
				uint32_t col = pal_rgba(pcol(c));
				if (selector[c] <= value) return rgba_blend50(old, col);
				return col;
			});
		} else if (image->modifier == 3) {
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t& old) {
				return rgba_blend50(old, pal_rgba(pcol(c)));
			});
		} else if (image->modifier == 8) {
			size_t data_size = data_pitch * screen_height;
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [dst, data_size](uint8_t c, uint32_t& old) -> uint32_t {
				size_t offset = &old - dst;
				if (offset >= c && data_size - offset > c) return *(&old + c);
				return old;
			});
		} else if (image->modifier == 10) {
			if (hd_shadow_dir) { draw_shadow_sheared(frame, flipped, screen_x, screen_y, 1, data, data_pitch); return; }
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [this](uint8_t, uint32_t& old) {
				return shadow_darken(old);
			});
		} else if (image->modifier == 9) {
			// Glow via the image's color_shift light table. The glow hue comes
			// from the TABLE, not the raw palette entry (raw entries gave SCV
			// exhaust blue centers); sample the table over black for the pure
			// glow color and blend it additively.
			auto& tdata = tileset_img.light_pcx.at(image->image_type->color_shift - 1).data;
			uint8_t* tptr = tdata.data();
			size_t tsize = tdata.size() / 256;
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t& old) {
				uint8_t nv = (uint8_t)(c - 1);
				if (nv >= tsize) return old;
				return rgba_add_sat(old, game_palette[tptr[256u * nv]]);
			});
		} else if (image->modifier == 12) {
			if (temporary_warp_texture_buffer.size() < frame.size.x * frame.size.y) temporary_warp_texture_buffer.resize(frame.size.x * frame.size.y);
			auto& texture_frame = global_st.image_grp[(size_t)ImageTypes::IMAGEID_Warp_Texture]->frames.at(image->modifier_data1);
			draw_frame(texture_frame, false, temporary_warp_texture_buffer.data(), frame.size.x, 0, 0, frame.size.x, frame.size.y);
			draw_frame_rgba_textured(frame, temporary_warp_texture_buffer.data(), flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t t, uint32_t&) {
				return pal_rgba(t);
			});
		} else if (image->modifier == 17) {
			// Fire/heat glow remaps destination palette indices in the 8-bit
			// path; approximate with a warm additive boost.
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t, uint32_t& old) {
				return rgba_add_sat(old, 0x00583818);
			});
		} else {
			draw_frame_rgba(frame, flipped, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t&) {
				return pal_rgba(pcol(c));
			});
		}
	}

	// Faze 4.3: nearest-scale-blit a GRP frame into the RGBA buffer at world_scale.
	// (lx,ly) is the logical top-left (map - screen_pos); scaled by ws here. remap
	// (index, dst_pixel) -> pixel; index 0 is transparent. Used by the RGBA world
	// overlays (selection circle, health bars) that would otherwise draw at 1x.
	template<typename F>
	void blit_grp_frame_ws(const grp_t::frame_t& frame, bool flipped, int lx, int ly, int ws, uint32_t* data, size_t pitch, F&& remap) {
		int fw = (int)frame.size.x, fh = (int)frame.size.y;
		if (fw <= 0 || fh <= 0) return;
		// Faze 7: reuse the decoded index scratch across calls (keyed by frame+flip).
		auto key = std::make_pair((const void*)&frame, flipped);
		const uint8_t* scrp;
		auto ci = grp_ws_cache.find(key);
		if (ci != grp_ws_cache.end()) {
			ci->second.used = ++hd_cache_tick; ci->second.frame = hd_cache_frame();
			scrp = ci->second.px.data();
		} else {
			auto& s = grp_ws_cache[key];
			s.px.assign((size_t)fw * fh, 0);
			draw_frame(frame, flipped, s.px.data(), (size_t)fw, 0, 0, (size_t)fw, (size_t)fh);
			s.bytes = (size_t)fw * fh; s.used = ++hd_cache_tick; s.frame = hd_cache_frame();
			grp_ws_cache_bytes += s.bytes;
			trim_hd_cache(grp_ws_cache, grp_ws_cache_bytes, (size_t)8 << 20, "grpws", grp_ws_trim_logged);
			scrp = grp_ws_cache[key].px.data();
		}
		int bx = lx * ws, by = ly * ws;
		for (int y = 0; y < fh * ws; ++y) {
			int dy = by + y; if (dy < 0 || dy >= (int)screen_height) continue;
			uint32_t* drow = data + (size_t)dy * pitch;
			const uint8_t* srow = scrp + (size_t)(y / ws) * fw;
			for (int x = 0; x < fw * ws; ++x) {
				int dx = bx + x; if (dx < 0 || dx >= (int)screen_width) continue;
				uint8_t c = srow[x / ws]; if (c == 0) continue;
				drow[dx] = remap(c, drow[dx]);
			}
		}
	}

	void draw_selection_circle_rgba(const sprite_t* sprite, const unit_t* u, uint32_t* data, size_t data_pitch) {
		auto* image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		xy map_pos = sprite->position + xy(0, sprite->sprite_type->selection_circle_vpos) + interp_off_for(sprite);

		auto* grp = global_st.image_grp[(size_t)image_type->id];
		auto& frame = grp->frames.at(0);

		map_pos.x += int(frame.offset.x - grp->width / 2);
		map_pos.y += int(frame.offset.y - grp->height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;
		int ws = world_scale < 1 ? 1 : world_scale;
		if (ws > 1) {
			size_t ci = st.players[sprite->owner].color;
			uint8_t col = img.player_unit_colors.at(ci)[0];
			if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) col = tileset_img.resource_minimap_color;
			blit_grp_frame_ws(frame, false, screen_x, screen_y, ws, data, data_pitch,
				[col, this](uint8_t c, uint32_t&) { return pal_rgba(c < 8 ? col : c); });
			return;
		}

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;

		size_t width = frame.size.x;
		size_t height = frame.size.y;

		if (screen_x + (int)width <= 0 || screen_y + (int)height <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) offset_x = -screen_x;
		if (screen_y < 0) offset_y = -screen_y;

		uint32_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, screen_width - screen_x);
		height = std::min(height, screen_height - screen_y);

		size_t color_index = st.players[sprite->owner].color;
		uint8_t color = img.player_unit_colors.at(color_index)[0];
		if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
			color = tileset_img.resource_minimap_color;
		}
		draw_frame_rgba(frame, false, dst, data_pitch, offset_x, offset_y, width, height, [&](uint8_t c, uint32_t&) {
			return pal_rgba(c < 8 ? color : c);
		});
	}

	void draw_health_bars_rgba(const sprite_t* sprite, const unit_t* u, uint32_t* data, size_t data_pitch) {
		auto* selection_circle_image_type = get_image_type((ImageTypes)((int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sprite->sprite_type->selection_circle));

		auto* selection_circle_grp = global_st.image_grp[(size_t)selection_circle_image_type->id];
		auto& selection_circle_frame = selection_circle_grp->frames.at(0);

		int offsety = sprite->sprite_type->selection_circle_vpos + selection_circle_frame.size.y / 2 + 8;

		bool has_shield = u->unit_type->has_shield;
		bool has_energy = ut_has_energy(u) || u_hallucination(u) || unit_is(u, UnitTypes::Zerg_Broodling);

		int width = sprite->sprite_type->health_bar_size;
		width -= (width - 1) % 3;
		if (width < 19) width = 19;
		int orig_width = width;
		int height = 5;
		if (has_shield) height += 2;
		if (has_energy) height += 6;

		xy map_pos = sprite->position + xy(0, offsety) + interp_off_for(sprite);

		map_pos.x += int(0 - width / 2);
		map_pos.y += int(0 - height / 2);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + width <= 0 || screen_y + height <= 0) return;

		auto filled_width = [&](int percent) {
			int r = percent * width / 100;
			if (r < 3) r = 3;
			else if (r % 3) {
				if (r % 3 > 1) r += 3 - (r % 3);
				else r -= r % 3;
			}
			return r;
		};

		int hp_percent = unit_hp_percent(u);
		int dw = filled_width(hp_percent);

		int shield_dw = 0;
		if (has_shield) {
			int shield_percent = (int)u->shield_points.integer_part() * 100 / std::max(u->unit_type->shield_points, 1);
			shield_dw = filled_width(shield_percent);
		}

		int energy_dw = 0;
		if (has_energy) {
			int energy_percent;
			if (ut_has_energy(u)) energy_percent = (int)u->energy.integer_part() * 100 / std::max((int)unit_max_energy(u).integer_part(), 1);
			else energy_percent = (int)u->remove_timer / std::max((int)default_remove_timer(u), 1);
			energy_dw = filled_width(energy_percent);
		}

		const int no_shield_colors_66[] = {18, 0, 1, 2, 18};
		const int no_shield_colors_33[] = {18, 3, 4, 5, 18};
		const int no_shield_colors_0[] = {18, 6, 7, 8, 18};
		const int no_shield_colors_bg[] = {18, 15, 16, 17, 18};

		const int with_shield_colors_66[] = {18, 0, 0, 1, 1, 2, 18};
		const int with_shield_colors_33[] = {18, 3, 3, 4, 4, 5, 18};
		const int with_shield_colors_0[] = {18, 6, 6, 7, 7, 8, 18};
		const int with_shield_colors_bg[] = {18, 15, 15, 16, 16, 17, 18};

		const int* colors_66 = has_shield ? with_shield_colors_66 : no_shield_colors_66;
		const int* colors_33 = has_shield ? with_shield_colors_33 : no_shield_colors_33;
		const int* colors_0 = has_shield ? with_shield_colors_0 : no_shield_colors_0;
		const int* colors_bg = has_shield ? with_shield_colors_bg : no_shield_colors_bg;

		int offset_x = 0;
		int offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
			dw = std::max(dw + screen_x, 0);
			shield_dw = std::max(shield_dw + screen_x, 0);
			energy_dw = std::max(energy_dw + screen_x, 0);
			width = std::max(width + screen_x, 0);
			screen_x = 0;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
			height += screen_y;
			screen_y = 0;
		}

		// Faze 4.3: at world_scale>1 render the bar into a small opaque scratch at
		// (0,0) and nearest-scale-blit it x ws to the real buffer at the function
		// tail. For an on-screen bar the clip above is a no-op, so this reuses the
		// intricate bar code unchanged. ws==1 keeps data == the real buffer.
		int hb_ws = world_scale < 1 ? 1 : world_scale;
		uint32_t* hb_real = data; size_t hb_real_pitch = data_pitch;
		int hb_lx = screen_x, hb_ly = screen_y, hb_bw = orig_width, hb_bh = height;
		static std::vector<uint32_t> hb_scr;
		if (hb_ws > 1) {
			hb_scr.assign((size_t)hb_bw * hb_bh, 0);
			data = hb_scr.data(); data_pitch = (size_t)hb_bw;
			offset_x = 0; offset_y = 0; screen_x = 0; screen_y = 0; width = orig_width;
		}

		uint32_t* dst = data + screen_y * data_pitch + screen_x;

		width = std::min(width, (int)screen_width - screen_x);
		height = std::min(height, (int)screen_height - screen_y);

		if (dw > width) dw = width;
		if (shield_dw > width) shield_dw = width;
		if (energy_dw > width) energy_dw = width;

		auto fill_row = [&](uint32_t* p, int n, int palette_index) {
			uint32_t c = pal_rgba((uint8_t)img.hp_bar_colors.at(palette_index));
			for (int i = 0; i != n; ++i) p[i] = c;
		};

		int hp_height = std::min(std::max((has_shield ? 7 : 5) - offset_y, 0), height);

		for (int i = offset_y; i < offset_y + hp_height; ++i) {
			int ci = hp_percent >= 66 ? colors_66[i] : hp_percent >= 33 ? colors_33[i] : colors_0[i];
			if (dw > 0) fill_row(dst, dw, ci);
			if (width - dw > 0) fill_row(dst + dw, width - dw, colors_bg[i]);
			dst += data_pitch;
		}

		if (has_shield) {
			const int shield_colors[] = {18, 10, 11, 18};
			const int shield_colors_bg[] = {18, 16, 17, 18};

			dst = data + screen_y * data_pitch + screen_x;

			for (int i = offset_y; i < std::min(4, height); ++i) {
				if (shield_dw > 0) fill_row(dst, shield_dw, shield_colors[i]);
				if (width - shield_dw > 0) fill_row(dst + shield_dw, width - shield_dw, shield_colors_bg[i]);
				dst += data_pitch;
			}
		}

		int energy_offset = std::max((has_shield ? 8 : 6) - offset_y, 0);
		int energy_begin = std::max(offset_y - (has_shield ? 8 : 6), 0);
		int energy_end = std::min(5, offset_y + height - (has_shield ? 8 : 6));

		if (has_energy) {
			dst = data + (screen_y + energy_offset) * data_pitch + screen_x;
			const int energy_colors[] = {18, 12, 13, 14, 18};
			for (int i = energy_begin; i < energy_end; ++i) {
				if (energy_dw > 0) fill_row(dst, energy_dw, energy_colors[i]);
				if (width - energy_dw > 0) fill_row(dst + energy_dw, width - energy_dw, no_shield_colors_bg[i]);
				dst += data_pitch;
			}
		}

		dst = data + screen_y * data_pitch + screen_x;
		if (offset_x % 3) dst += 3 - offset_x % 3;

		uint32_t border = pal_rgba((uint8_t)img.hp_bar_colors.at(18));
		for (int x = 0; x < orig_width; x += 3) {
			if (x < offset_x || x >= offset_x + width) continue;
			for (int y = 0; y != hp_height; ++y) {
				*dst = border;
				dst += data_pitch;
			}
			if (has_energy && energy_end > energy_begin) {
				if (energy_offset) dst += data_pitch;
				for (int i = energy_begin; i != energy_end; ++i) {
					*dst = border;
					dst += data_pitch;
				}
				if (energy_offset) dst -= data_pitch;
				dst -= (energy_end - energy_begin) * data_pitch;
			}
			dst -= hp_height * data_pitch;
			dst += 3;
		}
		// Faze 4.3: composite the scratch bar to the real buffer at x ws.
		if (hb_ws > 1) {
			int bx = hb_lx * hb_ws, by = hb_ly * hb_ws;
			for (int y = 0; y < hb_bh * hb_ws; ++y) {
				int dy = by + y; if (dy < 0 || dy >= (int)screen_height) continue;
				uint32_t* drow = hb_real + (size_t)dy * hb_real_pitch;
				const uint32_t* srow = hb_scr.data() + (size_t)(y / hb_ws) * hb_bw;
				for (int x = 0; x < hb_bw * hb_ws; ++x) {
					int dx = bx + x; if (dx < 0 || dx >= (int)screen_width) continue;
					uint32_t c = srow[x / hb_ws];
					if (c >> 24) drow[dx] = c;
				}
			}
		}
	}

	void process_rgba_sprite_queue(uint32_t* data, size_t data_pitch) {
		for (auto& e : rgba_sprite_queue) {
			if (e.image) draw_image_rgba(e.image, data, data_pitch, st.players[e.sprite->owner].color);
			else if (e.circle_u) draw_selection_circle_rgba(e.sprite, e.circle_u, data, data_pitch);
			else if (e.bars_u) draw_health_bars_rgba(e.sprite, e.bars_u, data, data_pitch);
		}
		rgba_sprite_queue.clear();
	}

	void draw_sprite(const sprite_t* sprite, uint8_t* data, size_t data_pitch) {
		const unit_t* draw_selection_u = current_selection_sprites_set.at(sprite->index);
		const unit_t* draw_health_bars_u = draw_selection_u;
		if (rgba_sprites) {
			// Defer everything in draw order; drawn onto the rgba surface
			// after the indexed world blit (process_rgba_sprite_queue).
			for (auto* image : ptr(reverse(sprite->images))) {
				if (i_flag(image, image_t::flag_hidden)) continue;
				if (draw_selection_u && image->modifier != 10) {
					rgba_sprite_queue.push_back({nullptr, sprite, draw_selection_u, nullptr});
					draw_selection_u = nullptr;
				}
				if (is_new_image(image)) image_draw_queue.push_back(image);
				else rgba_sprite_queue.push_back({image, sprite, nullptr, nullptr});
			}
			if (draw_health_bars_u && !u_invincible(draw_health_bars_u)) {
				rgba_sprite_queue.push_back({nullptr, sprite, nullptr, draw_health_bars_u});
			}
			return;
		}
		for (auto* image : ptr(reverse(sprite->images))) {
			if (i_flag(image, image_t::flag_hidden)) continue;
			if (draw_selection_u && image->modifier != 10) {
				draw_selection_circle(sprite, draw_selection_u, data, data_pitch);
				draw_selection_u = nullptr;
			}
			draw_image(image, data, data_pitch, st.players[sprite->owner].color);
		}
		if (draw_health_bars_u && !u_invincible(draw_health_bars_u)) {
			draw_health_bars(sprite, draw_health_bars_u, data, data_pitch);
		}
	}

	a_vector<std::pair<uint32_t, const sprite_t*>> sorted_sprites;

	// --- LAN render interpolation (pure presentation; net games only) ------
	// Lockstep delivers sim state in discrete ticks (latency=2), so on the
	// joiner units teleport one tick's worth of travel each received frame ->
	// visible stutter. Between advances we blend the previous tick's sprite
	// positions toward the current ones and add a per-sprite screen offset at
	// every render site, so units glide. This is PURELY visual: hit-testing,
	// orders and selection keep the true sim position (get_image_map_position
	// is NOT touched), and the whole path is inert unless is_net_game AND
	// render_interp. Costs ~1 tick of apparent latency, the standard render-
	// interpolation trade. Indexed by sprite->index (stable 0..2499).
	bool render_interp = true;              // toggle (dev setting); default on for net
	bool interp_active = false;             // offsets currently non-zero
	a_vector<xy> interp_prev, interp_cur, interp_off;
	a_vector<char> interp_prev_valid, interp_cur_valid;
	uint32_t interp_last_frame = 0xffffffffu;
	std::chrono::high_resolution_clock::time_point interp_last_advance;
	double interp_frame_ms = 42.0;          // measured ms between sim advances (smoothed)
	bool interp_single = false;             // last advance moved exactly 1 frame

	xy interp_off_for(const sprite_t* s) const {
		if (!interp_active || !s) return xy{0, 0};
		size_t ix = s->index;
		if (ix >= interp_off.size()) return xy{0, 0};
		return interp_off[ix];
	}

	void update_render_interp() {
		if (!render_interp || !is_net_game) {
			if (interp_active) { std::fill(interp_off.begin(), interp_off.end(), xy{0, 0}); interp_active = false; }
			return;
		}
		if (interp_off.size() != 2500) {
			interp_prev.assign(2500, xy{0, 0}); interp_cur.assign(2500, xy{0, 0});
			interp_off.assign(2500, xy{0, 0});
			interp_prev_valid.assign(2500, 0); interp_cur_valid.assign(2500, 0);
		}
		auto now = clock.now();
		uint32_t frame = st.current_frame;
		if (frame != interp_last_frame) {
			int adv = interp_last_frame == 0xffffffffu ? 1 : (int)(frame - interp_last_frame);
			if (interp_last_frame != 0xffffffffu && adv >= 1) {
				double dt = std::chrono::duration_cast<std::chrono::microseconds>(now - interp_last_advance).count() / 1000.0;
				double per = dt / adv;
				if (per > 5.0 && per < 500.0) interp_frame_ms = interp_frame_ms * 0.7 + per * 0.3;
			}
			std::swap(interp_prev, interp_cur);
			std::swap(interp_prev_valid, interp_cur_valid);
			std::fill(interp_cur_valid.begin(), interp_cur_valid.end(), (char)0);
			for (auto& v : sorted_sprites) {
				size_t ix = v.second->index;
				if (ix < 2500) { interp_cur[ix] = v.second->position; interp_cur_valid[ix] = 1; }
			}
			interp_last_frame = frame;
			interp_last_advance = now;
			interp_single = (adv == 1);
		}
		double fm = interp_frame_ms > 1.0 ? interp_frame_ms : 42.0;
		double dt = std::chrono::duration_cast<std::chrono::microseconds>(now - interp_last_advance).count() / 1000.0;
		double alpha = dt / fm;
		if (alpha < 0.0) alpha = 0.0;
		if (alpha > 1.0) alpha = 1.0;
		double w = 1.0 - alpha;  // render 1 tick behind: prev at alpha 0, cur at alpha 1
		std::fill(interp_off.begin(), interp_off.end(), xy{0, 0});
		if (interp_single) {
			for (auto& v : sorted_sprites) {
				size_t ix = v.second->index;
				if (ix < 2500 && interp_prev_valid[ix] && interp_cur_valid[ix]) {
					xy d = interp_prev[ix] - interp_cur[ix];
					// snap (no interp) across teleports/slot reuse -> avoid a ghost glide
					if (d.x > -256 && d.x < 256 && d.y > -256 && d.y < 256)
						interp_off[ix] = xy{ (int)(d.x * w), (int)(d.y * w) };
				}
			}
		}
		interp_active = true;
	}

	void draw_sprites(uint8_t* data, size_t data_pitch) {

		image_draw_queue.clear();
		rgba_sprite_queue.clear();

		sorted_sprites.clear();

		auto screen_tile = screen_tile_bounds();

		size_t from_y = screen_tile.from.y;
		if (from_y < 4) from_y = 0;
		else from_y -= 4;
		size_t to_y = screen_tile.to.y;
		if (to_y >= game_st.map_tile_height - 4) to_y = game_st.map_tile_height - 1;
		else to_y += 4;
		// v0.76 item 3: sprites the local player cannot see are not drawn.
		// Until now nothing here consulted vision at all: an enemy standing on
		// an explored-but-not-currently-visible tile was drawn and merely dimmed
		// 42% by the fog overlay, i.e. visible through the fog. visibility_flags
		// is the engine's own "which players can see this sprite" mask, so this
		// uses exactly the predicate the simulation uses.
		int vis_owner = fog_vision_owner();
		for (size_t y = from_y; y != to_y; ++y) {
			for (auto* sprite : ptr(st.sprites_on_tile_line.at(y))) {
				if (s_hidden(sprite)) continue;
				if (vis_owner >= 0 && !(sprite->visibility_flags & (1 << vis_owner))) continue;
				sorted_sprites.emplace_back(sprite_depth_order(sprite), sprite);
			}
		}

		std::sort(sorted_sprites.begin(), sorted_sprites.end());

		hd_prefetch_drain();   // perf #2: install any HD anims the worker finished
		update_render_interp();

		for (auto uid : current_selection) {
			auto* u = get_unit(uid);
			if (!u) continue;
			current_selection_sprites_set.at(u->sprite->index) = u;
			current_selection_sprites.push_back(u->sprite);
		}

		for (auto& v : sorted_sprites) {
			draw_sprite(v.second, data, data_pitch);
		}

		// Show a health bar under every damaged unit on screen (not just the
		// selection), so combat is readable at a glance. Full-health units stay
		// clean. Selected units already have their bar from draw_sprite.
		if ((show_damaged_health || hud_health_always) && game_input) {
			for (unit_t* u : find_units({screen_pos, screen_pos + xy((int)view_width, (int)view_height)})) {
				if (!u->sprite || u_invincible(u)) continue;
				if (current_selection_sprites_set.at(u->sprite->index)) continue;
				if (s_hidden(u->sprite)) continue;
				bool damaged = u->hp < u->unit_type->hitpoints
					|| (u->unit_type->has_shield && u->shield_points < fp8::integer(u->unit_type->shield_points));
				if (damaged || hud_health_always) {
					if (rgba_sprites) rgba_sprite_queue.push_back({nullptr, u->sprite, nullptr, u});
					else draw_health_bars(u->sprite, u, data, data_pitch);
				}
			}
		}

		for (auto* s : current_selection_sprites) {
			current_selection_sprites_set.at(s->index) = nullptr;
		}
		current_selection_sprites.clear();
	}
	bool show_damaged_health = true;

	void fill_rectangle(uint8_t* data, size_t data_pitch, rect area, uint8_t index) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t pitch = data_pitch;
		size_t from_y = area.from.y;
		size_t to_y = area.to.y;
		uint8_t* ptr = data + data_pitch * from_y + area.from.x;
		for (size_t i = from_y; i != to_y; ++i) {
			memset(ptr, index, width);
			ptr += pitch;
		}
	}

	void line_rectangle(uint8_t* data, size_t data_pitch, rect area, uint8_t index) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t height = area.to.y - area.from.y;
		uint8_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;
		memset(p, index, width);
		memset(p + data_pitch * height, index, width);
		for (size_t y = 0; y != height; ++y) {
			p[data_pitch * y] = index;
			p[data_pitch * y + width - 1] = index;
		}
	}

	void line_rectangle_rgba(uint32_t* data, size_t data_pitch, rect area, uint32_t color) {
		if (area.from.x < 0) area.from.x = 0;
		if (area.from.y < 0) area.from.y = 0;
		if (area.to.x > (int)screen_width) area.to.x = screen_width;
		if (area.to.y > (int)screen_height) area.to.y = screen_height;
		if (area.from.x >= area.to.x || area.from.y >= area.to.y) return;
		size_t width = area.to.x - area.from.x;
		size_t height = area.to.y - area.from.y;
		uint32_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;
		// The bottom edge belongs on the LAST row inside the rect (to.y-1), i.e.
		// offset (height-1). Writing at offset `height` lands on row to.y, which is
		// one PAST the rect -- and when a bottom-anchored HUD rect reaches the buffer
		// edge (to.y == screen_height, clamped above), that row is one past the whole
		// surface: an out-of-bounds heap write of a full row. Because this is the
		// most-used outline primitive, that stray write silently corrupts whatever
		// heap object follows the surface (e.g. an HWUI mutex -> a "destroyed mutex"
		// SIGABRT in hwuiTask0, far from the real cause). height>=1 here (guard above).
		for (size_t x = 0; x != width; ++x) {
			p[x] = color;
			p[(height - 1) * data_pitch + x] = color;
		}
		for (size_t y = 0; y != height; ++y) {
			p[data_pitch * y] = color;
			p[data_pitch * y + width - 1] = color;
		}
	}

	// Bresenham line into the RGBA HUD buffer (used for order-queue waypoints).
	void draw_line_rgba(uint32_t* data, size_t data_pitch, int x0, int y0, int x1, int y1, uint32_t color) {
		int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
		int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (;;) {
			if (x0 >= 0 && y0 >= 0 && x0 < (int)screen_width && y0 < (int)screen_height)
				data[(size_t)y0 * data_pitch + x0] = color;
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	}

	bool unit_visble_on_minimap(unit_t* u) {
		if (u->owner < 8 && u->sprite->visibility_flags == 0) return false;
		if (ut_turret(u)) return false;
		if (unit_is_trap(u)) return false;
		if (unit_is(u, UnitTypes::Spell_Dark_Swarm)) return false;
		if (unit_is(u, UnitTypes::Spell_Disruption_Web)) return false;
		return true;
	}

	// The console (tconsole.pcx, 640x480 in BW) is presented at a single UNIFORM
	// scale so its square slots (minimap, command buttons) stay square on non-4:3
	// screens, instead of being stretched by an independent x/y scale. It is
	// centered horizontally and anchored to the bottom; the game shows through
	// the side gaps ("free space at the edges"). Every console-relative slot maps
	// through console_to_screen()/console_len() using authentic 640x480 coords.
	float hud_scale() {
		// Fit to WIDTH (uniform): the console bar spans the full screen width with
		// square slots, and the console's unused top viewport border scales off
		// the top edge (anchored to the bottom via hud_off_y). This avoids the
		// pillarbox gaps a fit-to-height scale would leave on a wide screen.
		float cw = console_ok ? (float)console_pcx.width : 640.0f;
		return (float)screen_width / cw;
	}
	int hud_off_x() {
		float cw = console_ok ? (float)console_pcx.width : 640.0f;
		return (int)(((float)screen_width - cw * hud_scale()) / 2.0f);
	}
	int hud_off_y() {
		float chh = console_ok ? (float)console_pcx.height : 480.0f;
		return (int)((float)screen_height - chh * hud_scale());
	}
	xy console_to_screen(float cx, float cy) {
		float s = hud_scale();
		return xy(hud_off_x() + (int)(cx * s), hud_off_y() + (int)(cy * s));
	}
	int console_len(float cl) { return (int)(cl * hud_scale()); }

	// The opaque console bar (minimap / command card / build grid) starts at
	// console-space y~334 and reaches the screen bottom. Because the 4:3 console
	// is fit to WIDTH, on a 16:10 tablet it covers ~40% of the height - so the
	// "playfield" (the world the player can actually see) is only the area above
	// this line. Camera clamping, minimap centering and the minimap viewport
	// rect all use the playfield height, not the full screen, so the map's
	// bottom rows can be scrolled up above the console instead of hiding behind
	// it, and a minimap tap lands its target in the visible area.
	int playfield_bottom_y() {
		if (!game_input && replay_st.end_frame != 0) {
			// Replay: the world reaches down to the replay player's bar (which is not
			// the game HUD's height), so the camera clamp, the minimap's view box and
			// "is this press on the world?" all agree with what is drawn.
			replay_layout_t lay = replay_layout();
			if (lay.ok) return lay.band_top;
		}
		// HUD v3 has no bar: the world reaches the bottom of the screen (fog,
		// overlays and input see all of it); only the CAMERA stops at the info
		// card (playfield_view_height), so the map's last rows can still rise above it.
		if (hud_v3 && game_input) return (int)screen_height;
		if (!ui_legacy) {
			// HUD v2: the world reaches down to the top of the bottom bar.
			int y = (int)screen_height - hud2_bar_h() - hud2_margin();
			if (y < 96 || y > (int)screen_height) return (int)screen_height;
			return y;
		}
		if (!console_ok) return (int)screen_height;
		int y = console_to_screen(0.0f, 334.0f).y;
		if (y < 96 || y > (int)screen_height) return (int)screen_height;
		return y;
	}
	// Visible world height above the console, in world px (matches view_height's
	// world/screen scaling).
	int playfield_view_height() {
		const int bottom = hud_v3 && game_input && !(replay_st.end_frame != 0 && !game_input) ? hud3_layout().card.from.y : playfield_bottom_y();
		int h = (fp16::integer(bottom) / view_scale).integer_part();
		if (h < 32) h = (int)view_height;
		return h;
	}

	// ---- HUD v2 (native, responsive) layout ---------------------------
	// Anchored to screen edges; square dark-metal panels. All sizes derive from
	// the screen so it adapts to any tablet/phone aspect. bar_h = bottom bar.
	// Scale a HUD size authored at the base render resolution by the current
	// zoom so min-size clamps and fixed-pixel constants stay a constant FRACTION
	// of the (zoom-resized) buffer - and thus a constant on-screen size - instead
	// of a fixed buffer-pixel value that the zoom's buffer resize would upscale
	// in lockstep with the world (that lockstep was why the whole HUD appeared to
	// zoom in). At the default zoom (base == screen) the ratio is 1, so un-zoomed
	// play is unchanged. base_render_w/h are captured on the first set_zoom().
	// Faze 4.3: the HUD is authored for the HD2/half-res buffer. At FULL HD
	// (world_scale=2) the render buffer is 2x taller/wider, so fixed-pixel HUD
	// constants and fonts occupy HALF the on-screen fraction and read as tiny.
	// hud_us() (=world_scale) restores them to the same on-screen size. It is 1
	// for SD/HD2, so those verified tiers are byte-identical.
	int hud_us() { return world_scale < 1 ? 1 : world_scale; }
	bool hud_scale_text = false; // scaled-glyph mode: in-game HUD (draw_hud2) AND briefing/pause/tooltip
	bool diag_in_hud = false;    // TRUE only inside draw_hud2 proper -> the HUD_TEXT_CLIP scope
	                             // (hud_scale_text is broader, so the clip detector gates on this)
	int hud_h_px(int base_px) { int b = base_render_h ? base_render_h : (int)screen_height; return (b ? (int)((long long)base_px * (int)screen_height / b) : base_px) * hud_us(); }
	int hud_w_px(int base_px) { int b = base_render_w ? base_render_w : (int)screen_width; return (b ? (int)((long long)base_px * (int)screen_width / b) : base_px) * hud_us(); }
	// HUD font metrics. Faze 4.7: these now track hud_h_px() -- i.e. hud_us() PLUS
	// the pinch-zoom buffer factor (screen_height/base_render_h) -- exactly like the
	// panels, so at any zoom the glyphs/layout stay a constant fraction of the
	// (zoom-compensated) HUD instead of ballooning past the panels when you zoom in.
	// At the default zoom (base_render_h==0 -> factor 1) this equals the old value,
	// so SD/HD2 at rest are byte-identical. usp_adv() is the glyph advance (w+1).
	int usp_w() { return hud_h_px(ui::unispace_w); }
	int usp_h() { return hud_h_px(ui::unispace_h); }
	int usp_adv() { return hud_h_px(ui::unispace_w + 1); }
	// Multiply a draw_text_rgba_scaled pct by the HUD zoom+world factor (used by the
	// hud_scale_text path so glyph size matches the usp_* layout metrics under zoom).
	int hud_text_pct(int pct) {
		int b = base_render_h ? base_render_h : (int)screen_height;
		return (int)((long long)pct * hud_us() * (int)screen_height / (b ? b : 1));
	}
	int hud2_bar_h() { int h = (int)screen_height * 24 / 100; int m = (int)screen_height * 32 / 100; int fl = hud_h_px(96); return h < fl ? fl : (h > m ? m : h); }
	// ~1cm inset from the SIDE edges only, applied to the bottom-bar elements
	// (minimap, unit display, menu/portrait, 3x3 card). The bar itself still
	// sits at the very bottom. The right-edge Groups/QUEUE strips are NOT inset.
	int hud2_edge() { int e = (int)screen_width * 4 / 100; int fl = hud_w_px(12); return e < fl ? fl : e; }
	int hud2_margin() { return hud_h_px(4); }
	// v0.125 (approved geometry): the accent
	// line runs unbroken along the top of the bottom bar. The minimap panel, MENU,
	// the card panel and SHIFT used to START on the line's own row, so their top
	// border replaced it for their whole width (it was visible on 59% of the bar).
	// One rule now: the line is hud2_line_h() thick at hud2_bar_top(), and every
	// panel / button top sits hud2_top_inset() below the bar top -- one line and an
	// equal strip of bar -- the same on the left, in the middle and on the right.
	// Only SHIFT deliberately rides the line (hud2_queue_strip). Panel bottoms and
	// the bar height are unchanged; legacy HUD and the playfield bound are not
	// affected (the world still ends at the bar top).
	int hud2_bar_top() { return (int)screen_height - hud2_bar_h() - hud2_margin(); }
	int hud2_line_h() { return hud_us(); }
	int hud2_top_inset() { return 2 * hud2_line_h(); }
	rect hud2_minimap_panel() { if (hud_v3) return hud3_layout().minimap; int b = hud2_bar_h(), e = hud2_edge(), m = hud2_margin(); return {{e, hud2_bar_top() + hud2_top_inset()}, {e + b, (int)screen_height - m}}; }
	rect hud2_card_panel() { if (hud_v3) return hud3_layout().dock; int b = hud2_bar_h(), e = hud2_edge(), m = hud2_margin(); int w = b; return {{(int)screen_width - e - w, hud2_bar_top() + hud2_top_inset()}, {(int)screen_width - e, (int)screen_height - m}}; }
	// (MENU keeps its height and the portrait its size: both move down by the
	// inset, and the latency box under the portrait gives up those pixels.)
	rect hud2_portrait_panel() { rect c = hud2_card_panel(); int b = hud2_bar_h(); int w = b * 55 / 100; int x1 = c.from.x - hud2_margin() * 2; return {{x1 - w, c.from.y + b * 22 / 100}, {x1, c.to.y}}; }
	rect hud2_menu_panel() { if (hud_v3) return hud3_layout().menu; rect p = hud2_portrait_panel(); rect c = hud2_card_panel(); return {{p.from.x, c.from.y}, {p.to.x, p.from.y - 3}}; }
	// v0.75 item 9: the portrait is SQUARE at the top of its panel; the strip
	// left below it is the LAN latency readout. The video used to be stretched
	// over the whole (taller than wide) panel, so there was no space to put it
	// and the text landed on top of the picture.
	rect hud2_portrait_video() {
		rect p = hud2_inner(hud2_portrait_panel());
		int side = p.to.x - p.from.x;
		if (side > p.to.y - p.from.y) side = p.to.y - p.from.y;
		return {{p.from.x, p.from.y}, {p.from.x + side, p.from.y + side}};
	}
	// The framed box around the portrait -- the video plus its 3px border. The
	// latency box is aligned to THIS, not to the panel, so the two read as a
	// stack of equal-width boxes.
	rect hud2_portrait_frame() {
		rect v = hud2_portrait_video();
		return {{v.from.x - 3, v.from.y - 3}, {v.to.x + 3, v.to.y + 3}};
	}
	rect hud2_latency_strip() {
		rect p = hud2_inner(hud2_portrait_panel());
		rect f = hud2_portrait_frame();
		// v0.82.018: was {p.from.x, v.to.y} .. {p.to.x, p.to.y} -- it started at
		// the video's bottom edge, so the portrait frame's own bottom border sat
		// inside it, and it spanned the panel width while the frame above spanned
		// the square. Same width, and clear of the frame by 3px.
		return {{f.from.x, f.to.y + 3}, {f.to.x, p.to.y}};
	}
	// v0.29 item 6: the Unit Display is shortened ~15% from the top; the freed
	// strip above it holds the horizontal control-group (0-9) row.
	int hud2_groups_row_h() { return hud2_bar_h() * 15 / 100; }
	rect hud2_unit_panel() { if (hud_v3) return hud3_layout().card_full; rect mm = hud2_minimap_panel(); rect mp = hud2_menu_panel(); int g = hud2_margin() * 3; return {{mm.to.x + g, hud2_bar_top() + hud2_groups_row_h()}, {mp.from.x - g, mm.to.y}}; }
	rect hud2_inner(rect r) { int i = hud_h_px(3); return {{r.from.x + i, r.from.y + i}, {r.to.x - i, r.to.y - i}}; } // zoom-locked inset
	// The Unit Display wireframe box. One definition, because the production
	// strip has to lay itself out beside it -- when the two computed the same
	// expression separately they drifted apart every time the size changed.
	int hud2_wireframe_box() {
		rect panel = hud2_inner(hud2_unit_panel());
		int PW = panel.to.x - panel.from.x, PH = panel.to.y - panel.from.y;
		int wf = PH * 79 / 100;                 // v0.82.023: +20% again
		if (wf > PW * 2 / 5) wf = PW * 2 / 5;
		// ...but never past the room the HP/shield/energy lines need underneath.
		// At 79% the shield line first fell off the panel, and clamping the text
		// upwards instead just put it on top of the wireframe. Worst case is
		// three pools, so reserve for three.
		int small_lh = (ui::unispace_h + 2) * 4 / 5;
		int room = PH - 6 * 2 - small_lh * 3 - 2;
		if (wf > room) wf = room;
		if (wf < 16) wf = 16;
		return wf;
	}
	// v0.29 item 6: control groups are a HORIZONTAL row in the freed strip above the
	// Unit Display (was a vertical strip on the right edge).
	rect hud2_groups_strip() { rect r = hud2_group_row(); return {{r.from.x, r.from.y}, {r.from.x + hud2_group_cell_w() * 10, r.to.y}}; }
	// QUEUE toggle keeps its own small strip on the right edge (standalone now).
	rect hud2_group_row() { rect u = hud2_unit_panel(); return {{u.from.x, hud2_bar_top() + hud2_top_inset()}, {u.to.x, u.from.y - hud2_margin()}}; }
	int hud2_group_cell_w() { rect r = hud2_group_row(); int w = (r.to.x - r.from.x) / 11; return w < 1 ? 1 : w; }
	// v0.64 item 25: QUEUE is the eleventh cell of the control-group row rather
	// than a floating strip on the screen edge, so both are same-height sibling
	// buttons and the band stays inside the Unit Display's horizontal bounds.
	// v0.82.021: runs to the Unit Display's right edge instead of stopping at a
	// nominal eleventh cell, so the row has no dead strip on its end.
	rect hud2_queue_strip() {
		if (hud_v3) return hud3_layout().shift;
		rect r = hud2_group_row(); int w = hud2_group_cell_w();
		int x0 = r.from.x + w * 10 + hud2_margin();
		int x1 = hud2_unit_panel().to.x;
		if (x1 < x0 + w) x1 = r.from.x + w * 11;
		// v0.125: SHIFT is the one control that rides the accent line -- taller than
		// the group cells, its top above the line, its bottom level with theirs, and
		// (drawn in draw_hud2_side_strips) an outer border in the line's colour.
		return {{x0, hud2_bar_top() - hud_h_px(6)}, {x1, r.to.y}};
	}
	// Map-ping button: a small square button just above the minimap (v0.29 item 6 -
	// original BW has no ping button, so this is a beveled console-style button).
	// v0.75 item 6: ping / idle / army form one row spanning the minimap's full
	// width, directly above it, instead of a fixed-size square plus two
	// fixed-width buttons that stopped short of the panel edge.
	int hud2_util_row_h() { return hud_h_px(30); }
	rect hud2_util_cell(int i) {
		if (hud_v3) { hud3_layout_t L = hud3_layout(); return i == 0 ? L.ping : i == 1 ? L.idle : L.army; }
		rect mm = hud2_minimap_panel();
		int h = hud2_util_row_h(), m = hud2_margin();
		int total = mm.to.x - mm.from.x;
		int w = (total - m * 2) / 3;
		int x = mm.from.x + i * (w + m);
		int y = hud2_bar_top() - h - m;
		// The last cell absorbs the rounding so the row ends flush with the
		// minimap's right edge.
		int x2 = i == 2 ? mm.to.x : x + w;
		return {{x, y}, {x2, y + h}};
	}
	rect hud2_ping_button() { return hud2_util_cell(0); }
	bool ping_armed = false;
	xy ping_pos = xy(-1, -1);
	int ping_frame = 0;

	// A minimap ping arrived through the action stream (opcode 88) — from self
	// or an ally. Show it on the minimap. Enemy pings are ignored.
	void on_ping_minimap(int owner, int x, int y) override {
		if (game_local_player < 0) return;
		if (owner != game_local_player && !st.alliances[game_local_player][owner]) return;
		ping_pos = xy(x, y);
		ping_frame = (int)st.current_frame;
	}

	// The minimap's on-screen rect: the map's aspect ratio fitted inside the
	// console's square minimap slot (legacy) or the HUD v2 minimap panel.
	// draw_minimap_scaled scales the native minimap into this rect; input
	// hit-testing maps clicks back through it.
	rect get_minimap_area() {
		size_t mtw = game_st.map_tile_width, mth = game_st.map_tile_height;
		if (mtw == 0 || mth == 0) return {};
		int ox0, oy0, slot;
		replay_layout_t rlay = replay_layout();   // not ok unless a replay is playing
		if (rlay.ok) {
			// Replay: the minimap well in the replay player's bar.
			int in = std::max(1, (int)screen_height / 400) * 2;
			slot = rlay.minimap.to.x - rlay.minimap.from.x - 2 * in;
			ox0 = rlay.minimap.from.x + in; oy0 = rlay.minimap.from.y + in;
		} else if (ui_legacy) {
			xy a = console_to_screen(5.0f, 346.0f);
			slot = console_len(129.0f);
			ox0 = a.x; oy0 = a.y;
		} else {
			rect p = hud2_inner(hud2_minimap_panel());
			slot = std::min(p.to.x - p.from.x, p.to.y - p.from.y);
			ox0 = p.from.x + (p.to.x - p.from.x - slot) / 2; oy0 = p.from.y + (p.to.y - p.from.y - slot) / 2;
		}
		if (slot <= 0) return {};
		int w = slot, h = slot;
		if (mtw > mth) h = (int)((int64_t)slot * mth / mtw);
		else if (mth > mtw) w = (int)((int64_t)slot * mtw / mth);
		int ox = ox0 + (slot - w) / 2, oy = oy0 + (slot - h) / 2;
		return {{ox, oy}, {ox + w, oy + h}};
	}

	// Map a screen point inside the (scaled) minimap slot to a map position.
	xy minimap_to_map(int mx, int my) {
		rect a = get_minimap_area();
		int w = a.to.x - a.from.x, h = a.to.y - a.from.y;
		if (w <= 0 || h <= 0) return xy(0, 0);
		int64_t px = (int64_t)(mx - a.from.x) * (int)game_st.map_tile_width * 32 / w;
		int64_t py = (int64_t)(my - a.from.y) * (int)game_st.map_tile_height * 32 / h;
		if (px < 0) px = 0;
		if (px >= (int)game_st.map_width) px = (int)game_st.map_width - 1;
		if (py < 0) py = 0;
		if (py >= (int)game_st.map_height) py = (int)game_st.map_height - 1;
		return xy((int)px, (int)py);
	}

	// Render the minimap (terrain + units + viewport rect) into an indexed
	// buffer at native tile size, at the given area. The RGBA HUD then scales
	// this into the console's minimap slot (see draw_minimap_scaled).
	void draw_minimap_into(uint8_t* data, size_t data_pitch, rect area) {
		size_t minimap_width = area.to.x - area.from.x;
		size_t minimap_height = area.to.y - area.from.y;
		if (minimap_width != game_st.map_tile_width) return;
		if (minimap_height != game_st.map_tile_height) return;
		fill_rectangle(data, data_pitch, area, 0);
		line_rectangle(data, data_pitch, {area.from - xy(1, 1), area.to + xy(1, 1)}, 0);

		uint8_t* p = data + data_pitch * (size_t)area.from.y + (size_t)area.from.x;

		size_t pitch = data_pitch - game_st.map_tile_width;
		for (size_t y = 0; y != game_st.map_tile_height; ++y) {
			for (size_t x = 0; x != game_st.map_tile_width; ++x) {
				size_t index;
				if (~st.tiles[y * game_st.map_tile_width + x].flags & tile_t::flag_has_creep) index = st.tiles_mega_tile_index[y * game_st.map_tile_width + x];
				else index = game_st.cv5.at(1).mega_tile_index[creep_random_tile_indices[y * game_st.map_tile_width + x]];
				auto* images = &tileset_img.vx4.at(index).images[0];
				auto* bitmap = &tileset_img.vr4.at(*images / 2).bitmap[0];
				auto val = bitmap[55 / sizeof(vr4_entry::bitmap_t)];
				size_t shift = 8 * (55 % sizeof(vr4_entry::bitmap_t));
				val >>= shift;
				// Fog: unexplored tiles are black on the minimap too. Melee uses
				// the local player; a replay uses the chosen vision player (if
				// any). fog_vision_owner() also honours the dev reveal toggle,
				// which the minimap used to ignore -- that is why the toggle
				// looked broken: the world uncovered but the minimap stayed dark.
				int vis_owner = fog_vision_owner();
				if (vis_owner >= 0
					&& (st.tiles[y * game_st.map_tile_width + x].explored & (1 << vis_owner))) {
					val = 0;
				}
				*p++ = (uint8_t)val;
			}
			p += pitch;
		}

		// v0.76 item 3: the minimap only plots what the local player can see.
		// unit_visble_on_minimap() tests visibility_flags == 0, which is NOT a
		// local-vision test -- it is the "who can see this sprite" mask, and a
		// unit's own owner always sees it, so it was essentially never zero and
		// every enemy showed permanently. Test the local player's bit instead,
		// which is the same predicate the simulation uses.
		int mm_vis = fog_vision_owner();
		for (size_t i = 12; i != 0;) {
			--i;
			for (unit_t* u : ptr(st.player_units[i])) {
				if (!unit_visble_on_minimap(u)) continue;
				if (mm_vis >= 0 && !(u->sprite->visibility_flags & (1 << mm_vis))) continue;
				int color = img.player_minimap_colors.at(st.players[u->owner].color);
				size_t w = u->unit_type->placement_size.x / 32u;
				size_t h = u->unit_type->placement_size.y / 32u;
				if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser)) {
					color = tileset_img.resource_minimap_color;
				}
				if (ut_building(u)) {
					if (w > 4) w = 4;
					if (h > 4) h = 4;
				}
				if (w < 2) w = 2;
				if (h < 2) h = 2;
				rect unit_area;
				unit_area.from = area.from + (u->sprite->position - u->unit_type->placement_size / 2) / 32u;
				unit_area.to = unit_area.from + xy(w, h);
				fill_rectangle(data, data_pitch, unit_area, color);
			}
		}

		rect view_rect;
		view_rect.from = area.from + xy(screen_pos.x / 32u, screen_pos.y / 32u);
		// Height is the visible playfield (above the console), not the full
		// screen, so the white box matches what the player actually sees.
		view_rect.to = view_rect.from + xy((view_width + 31) / 32u, (playfield_view_height() + 31) / 32u);
		line_rectangle(data, data_pitch, view_rect, 255);

	}

	a_vector<uint8_t> minimap_scratch;
	// A6 (C-perf): minimap divide LUT. The scale loop runs ~16k px/frame every frame
	// at every tier; replace its per-row `y*mth/dh` and per-pixel `x*mtw/dw` divides
	// with precomputed source-index tables. BYTE-IDENTICAL by construction -- each
	// entry stores exactly the same integer expression the inner loop used, and
	// mtw/mth (<=256 tiles) and dw/dh (a few hundred px) keep every value well under
	// uint16, so the cast is lossless. Rebuilt only when the dims change.
	a_vector<uint16_t> minimap_col_lut, minimap_row_lut;
	int minimap_lut_dims[4] = {-1, -1, -1, -1};   // last {dw, dh, (int)mtw, (int)mth}

	// Render the minimap at native tile size into a scratch buffer, then
	// nearest-neighbor scale it up into the console's minimap slot (RGBA),
	// coloring indices through the game palette. This fills the (square) slot
	// regardless of the map's tile dimensions.
	void draw_minimap_scaled(uint32_t* data, size_t pitch) {
		size_t mtw = game_st.map_tile_width, mth = game_st.map_tile_height;
		if (mtw == 0 || mth == 0) return;
		rect slot = get_minimap_area();
		int dw = slot.to.x - slot.from.x, dh = slot.to.y - slot.from.y;
		if (dw <= 0 || dh <= 0) return;
		// Render into a buffer with a 1px margin: draw_minimap_into draws a border
		// one pixel outside the area, so the map body goes at (1,1).
		size_t sw = mtw + 2, sh = mth + 2;
		// + slack rows: draw_minimap_into clips its rectangles to the SCREEN, not to
		// this buffer -- the border's bottom line lands one row past `sh`, and a
		// building or the view box on the map's last rows runs a few rows further.
		// Without the slack that was a heap overflow every frame (found by the host
		// harness, whose allocator checks; the scaler below never reads these rows).
		minimap_scratch.assign(sw * (sh + 8), 0);
		draw_minimap_into(minimap_scratch.data(), sw, {{1, 1}, {(int)mtw + 1, (int)mth + 1}});
		// A6: (re)build the source-index LUTs when the dims change. Each entry is the
		// exact integer the divide would have produced -> the loop below is byte-for-byte
		// equal to the divide-per-pixel version, just with a table load instead.
		if (minimap_lut_dims[0] != dw || minimap_lut_dims[1] != dh
		    || minimap_lut_dims[2] != (int)mtw || minimap_lut_dims[3] != (int)mth) {
			minimap_col_lut.resize(dw);
			for (int x = 0; x < dw; ++x) minimap_col_lut[x] = (uint16_t)((size_t)x * mtw / dw);
			minimap_row_lut.resize(dh);
			for (int y = 0; y < dh; ++y) minimap_row_lut[y] = (uint16_t)(1 + (size_t)y * mth / dh);
			minimap_lut_dims[0] = dw; minimap_lut_dims[1] = dh;
			minimap_lut_dims[2] = (int)mtw; minimap_lut_dims[3] = (int)mth;
		}
		for (int y = 0; y < dh; ++y) {
			int py = slot.from.y + y;
			if (py < 0 || py >= (int)screen_height) continue;
			size_t sy = minimap_row_lut[y];
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = minimap_scratch.data() + sy * sw + 1;
			for (int x = 0; x < dw; ++x) {
				int px = slot.from.x + x;
				if (px < 0 || px >= (int)screen_width) continue;
				row[px] = 0xff000000u | game_palette[srow[minimap_col_lut[x]]];
			}
		}
	}

	int replay_frame = 0;

	// The indexed pass draws no UI any more (everything is in the RGBA pass); kept
	// so callers don't change.
	void draw_ui(uint8_t* data, size_t data_pitch) { (void)data; (void)data_pitch; }

	// ---- Replay player (launcher redesign F10) --------------------------------
	// The HUD shown while watching a .rep, in the launcher's design language
	// (loading_screen::pal + its panel / button / text primitives -- the same ones
	// the loading screen and the mission briefing draw with):
	//
	//   tablet   a bottom bar: the timeline across the full width, then the minimap,
	//            one card per player (portrait, name, resources, stats, a vision
	//            toggle) and the control block (|< -10 pause +10 >|, speed - / +,
	//            EXIT). Tapping a card opens a side panel with tabs (army /
	//            upgrades / buildings). Active production stays top-left.
	//   phone    (wide, short screens) the bar keeps the timeline, a small minimap
	//            and the controls on one row; the roster shrinks to a strip top-left
	//            and the cards move into a PLAYERS panel; EXIT sits top-right.
	//
	// Pure viewer layer: it reads st and writes only the viewer's own state
	// (replay_frame = the seek target, is_paused, game_speed, the vision player) --
	// exactly what the old bar wrote -- so replay determinism is untouched. Sizes
	// derive from the buffer height (not hud_us()), so FULL HD, HD2 and a reduced
	// render scale all show the same physical size.
	//
	// Cost: the bar is composed into two cached layers (static chrome; chrome + the
	// numbers, refreshed a few times a second) and copied per frame; only the
	// timeline's fill, the minimap and the small overlays are drawn every frame.
	int replay_layout_mode = -1;             // -1 auto (by aspect), 0 tablet, 1 phone
	int observer_flyout_player = -1;         // whose side panel is open (-1 = none)
	int observer_flyout_tab = 0;             // 0 army, 1 upgrades, 2 buildings, 3 economy
	bool replay_players_open = false;        // phone: the PLAYERS panel
	enum { rbtn_start, rbtn_back, rbtn_pause, rbtn_fwd, rbtn_end, rbtn_slower, rbtn_faster, rbtn_exit, replay_btn_count };
	static const int replay_tab_count = 4;
	static const int replay_tab_economy = 3;

	// ---- Replay scan (F10 step 2): what the timeline marks and the Economy tab show
	// The whole replay is played once on a worker thread, on its OWN copy of the
	// state (taken at frame 0), the action state and the replay data, so the marks
	// exist for the part not watched yet. Nothing is shared with the viewer's
	// simulation but the immutable game_state; the viewer only reads the finished
	// numbers under a mutex. One sample a game second:
	//   per player   resources gathered so far, army value, workers
	//   all players  the value destroyed in combat (st.unit_kills deltas) and where
	// Battles are the stretches where the destroyed value stays high; bases are new
	// grounded resource depots away from the player's other ones.
	struct replay_scan_sample { int32_t gathered = 0, army = 0, workers = 0; };
	struct replay_scan_battle { int f0 = 0, f1 = 0, value = 0, x = 0, y = 0; };
	struct replay_scan_base { int frame = 0, player = 0, x = 0, y = 0; };
	struct replay_scan_data {
		int step = 24;                       // frames per sample (~1 s)
		int frames = 0;                      // scanned so far
		bool done = false, failed = false;
		std::vector<std::array<replay_scan_sample, 8>> samples;
		std::vector<int32_t> lost;           // per sample: value destroyed in combat, all players
		std::vector<int32_t> lost_x, lost_y; // per sample: where (centroid of the kills), -1 = none
		std::vector<replay_scan_battle> battles;
		std::vector<replay_scan_base> bases;
	};
	struct replay_scanner {
		struct funcs : replay_functions {
			using replay_functions::replay_functions;
			int64_t kx = 0, ky = 0; int kn = 0;
			virtual void on_kill_unit(unit_t* u) override {
				if (!u || !u->sprite || ut_turret(u) || !u_completed(u)) return;
				if (u->unit_type->mineral_cost + u->unit_type->gas_cost <= 0) return;
				kx += u->sprite->position.x; ky += u->sprite->position.y; ++kn;
			}
			int unit_value(const unit_type_t* ut) const {
				int v = ut->mineral_cost + ut->gas_cost;
				return ut_two_units_in_one_egg(ut) ? v / 2 : v;
			}
			bool counts_as_army(const unit_t* u) const {
				if (!u_completed(u) || ut_building(u) || ut_worker(u) || ut_turret(u) || u_hallucination(u)) return false;
				switch (u->unit_type->id) {
				case UnitTypes::Zerg_Larva: case UnitTypes::Zerg_Egg: case UnitTypes::Zerg_Lurker_Egg: case UnitTypes::Zerg_Cocoon:
				case UnitTypes::Zerg_Overlord: case UnitTypes::Terran_Vulture_Spider_Mine: case UnitTypes::Terran_Nuclear_Missile:
				case UnitTypes::Protoss_Scarab:
					return false;
				default: return true;
				}
			}
		};
		state st;
		action_state ast;
		replay_state rst;
		std::thread th;
		std::atomic<bool> stop{false};
		std::atomic<int> version{0};
		std::mutex mtx;
		replay_scan_data data;               // guarded by mtx
		~replay_scanner() { stop = true; if (th.joinable()) th.join(); }

		// Battles from the per-sample losses: a 10 s window holding >= 250 resources
		// of destroyed units, runs closer than 6 s merged. O(samples), done per publish.
		static void find_battles(replay_scan_data& d) {
			d.battles.clear();
			const int n = (int)d.lost.size(), W = 10, T = 250, GAP = 6;
			int64_t sum = 0;
			int run_start = -1, last_hot = -1;
			auto close = [&]() {
				if (run_start < 0) return;
				int a = run_start - (W - 1); if (a < 0) a = 0;
				while (a < last_hot && d.lost[(size_t)a] == 0) ++a;
				int b = last_hot;
				while (b > a && d.lost[(size_t)b] == 0) --b;
				replay_scan_battle bt;
				bt.f0 = a * d.step; bt.f1 = (b + 1) * d.step;
				int peak = -1;
				for (int i = a; i <= b; ++i) {
					bt.value += d.lost[(size_t)i];
					if (d.lost_x[(size_t)i] >= 0 && (peak < 0 || d.lost[(size_t)i] > d.lost[(size_t)peak])) peak = i;
				}
				if (peak >= 0) { bt.x = d.lost_x[(size_t)peak]; bt.y = d.lost_y[(size_t)peak]; } else { bt.x = bt.y = -1; }
				d.battles.push_back(bt);
				run_start = -1;
			};
			for (int i = 0; i < n; ++i) {
				sum += d.lost[(size_t)i];
				if (i >= W) sum -= d.lost[(size_t)(i - W)];
				if (sum >= T) {
					if (run_start >= 0 && i - last_hot > GAP + W) close();
					if (run_start < 0) run_start = i;
					last_hot = i;
				}
			}
			close();
		}

		void run() {
			try {
				funcs f(st, ast, rst);
				a_vector<int> cost((size_t)UnitTypes::None, 0);
				for (size_t i = 0; i != cost.size(); ++i) cost[i] = f.unit_value(f.get_unit_type((UnitTypes)i));
				struct depot_t { int x, y; };
				std::array<std::vector<depot_t>, 8> ever;
				int64_t prev_killed = 0;
				int since_publish = 0;
				const int step = data.step, end = rst.end_frame;
				while (!stop) {
					const int frame = (int)st.current_frame;
					if (frame % step == 0) {
						std::array<replay_scan_sample, 8> row{};
						std::vector<replay_scan_base> new_bases;
						int64_t killed = 0;
						for (int p = 0; p != 8; ++p) {
							replay_scan_sample& s = row[(size_t)p];
							s.gathered = st.total_minerals_gathered[(size_t)p] + st.total_gas_gathered[(size_t)p];
							std::vector<depot_t> now;
							for (const unit_t* u : ptr(st.player_units[(size_t)p])) {
								if (f.counts_as_army(u)) s.army += cost[(size_t)u->unit_type->id];
								else if (f.u_completed(u) && f.ut_worker(u)) ++s.workers;
								if (f.ut_resource_depot(u) && f.u_grounded_building(u) && u->sprite) now.push_back({u->sprite->position.x, u->sprite->position.y});
							}
							for (auto& dp : now) {
								bool known = false, far = true;
								// (a lifted building that lands where, or next to where, one stood is not a new base)
								for (auto& o : ever[(size_t)p]) {
									if (o.x == dp.x && o.y == dp.y) { known = true; break; }
									if (std::abs(o.x - dp.x) < 384 && std::abs(o.y - dp.y) < 384) far = false;
								}
								if (!known) ever[(size_t)p].push_back(dp);
								if (!known && far && frame > 0) new_bases.push_back({frame, p, dp.x, dp.y});
							}
							for (size_t t = 0; t != cost.size(); ++t) killed += (int64_t)st.unit_kills[(size_t)p][(UnitTypes)t] * cost[t];
						}
						std::lock_guard<std::mutex> l(mtx);
						data.samples.push_back(row);
						data.lost.push_back((int32_t)(killed - prev_killed));
						data.lost_x.push_back(f.kn ? (int32_t)(f.kx / f.kn) : -1);
						data.lost_y.push_back(f.kn ? (int32_t)(f.ky / f.kn) : -1);
						for (auto& b : new_bases) data.bases.push_back(b);
						data.frames = frame;
						prev_killed = killed; f.kx = f.ky = 0; f.kn = 0;
						if (++since_publish >= 20 || frame >= end) {
							since_publish = 0;
							find_battles(data);
							++version;
						}
					}
					if (frame >= end) break;
					f.next_frame();
				}
			} catch (...) {
				std::lock_guard<std::mutex> l(mtx);
				data.failed = true;
			}
			std::lock_guard<std::mutex> l(mtx);
			find_battles(data);
			data.done = true;
			++version;
		}
	};
	std::unique_ptr<replay_scanner> replay_scan;
	replay_scan_data replay_scan_view;       // the viewer's copy of the scanner's data
	int replay_scan_seen = -1;               // its version
	std::vector<int> replay_seek_frames;     // frames the glue holds a rewind state for (ticks on the timeline)

	// Start the scan. Must be called before the replay is stepped (the scanner needs
	// the frame-0 state); a no-op in a game, when already started, or later.
	void replay_scan_start() {
#ifndef EMSCRIPTEN
		if (replay_scan || game_input || replay_st.end_frame == 0 || st.current_frame != 0) return;
		replay_scan = std::make_unique<replay_scanner>();
		replay_scan->st = copy_state(st);
		replay_scan->ast = copy_state(action_st, st, replay_scan->st);
		replay_scan->rst = replay_st;
		replay_scanner* s = replay_scan.get();
		s->th = std::thread([s]() { s->run(); });
#endif
	}
	// Block until the scan is finished (the host harness: deterministic screenshots).
	void replay_scan_wait() {
		if (replay_scan && replay_scan->th.joinable()) replay_scan->th.join();
	}
	void replay_scan_refresh() {
		if (!replay_scan) return;
		const int v = replay_scan->version.load();
		if (v == replay_scan_seen) return;
		std::lock_guard<std::mutex> l(replay_scan->mtx);
		replay_scan_view = replay_scan->data;
		replay_scan_seen = v;
	}

	struct replay_layout_t {
		bool ok = false, compact = false, two_col = false;
		float s = 1.0f;                      // one design unit in buffer pixels
		int n = 0, rows = 1;                 // players listed; card rows
		int band_top = 0;                    // the playfield ends here
		rect timeline;                       // the timeline row (drag hit area)
		rect track;                          // the bar inside it
		float time_h = 0;                    // text height of the two time labels
		rect minimap;                        // square slot
		rect cards;                          // where the player cards go (phone: inside the panel)
		int card_h = 0, card_gap = 0;
		rect btn[replay_btn_count];
		rect speed;                          // the speed readout between - and +
		rect panel;                          // tablet: the side panel; phone: the PLAYERS panel
		rect flyout;                         // the tabbed composition view (phone: right part of the panel)
		rect close_btn;                      // the panel's close button
		rect players_btn;                    // phone only
		rect mini;                           // phone: the roster strip top-left
		int mini_row_h = 0;
		int rp(float v) const { int r = (int)(v * s + 0.5f); return r < 1 ? 1 : r; }
	};
	static bool rp_in(const rect& r, int x, int y) { return x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y; }
	static bool rp_empty(const rect& r) { return r.to.x <= r.from.x || r.to.y <= r.from.y; }

	// The layout is pure arithmetic on a handful of inputs, but it is asked for
	// from per-sprite clipping (playfield_bottom_y), so it is memoised on them.
	replay_layout_t replay_layout_memo;
	int64_t replay_layout_key = -1;
	replay_layout_t replay_layout() {
		replay_layout_t lay;
#ifdef EMSCRIPTEN
		return lay;
#endif
		if (game_input || replay_st.end_frame == 0) return lay;
		const int sw = (int)screen_width, sh = (int)screen_height;
		if (sw < 480 || sh < 240) return lay;
		const int64_t key = ((int64_t)sw << 40) | ((int64_t)sh << 20) | ((int64_t)observer_cache.size() << 8) | (int64_t)(replay_layout_mode + 1);
		if (key == replay_layout_key) return replay_layout_memo;
		replay_layout_key = key;
		struct memo_t { ui_functions* u; replay_layout_t& l; ~memo_t() { u->replay_layout_memo = l; } } memo{this, lay};
		// A phone is recognised by its aspect (>= ~2:1); tablets are 4:3 .. 16:9. The
		// buffer height alone cannot tell them apart (it depends on the render scale).
		lay.compact = replay_layout_mode >= 0 ? replay_layout_mode == 1 : sw * 100 >= sh * 195;
		lay.s = (float)sh / (lay.compact ? 400.0f : 800.0f);
		lay.n = (int)observer_cache.size();
		const int n = lay.n < 1 ? 1 : lay.n;
		lay.time_h = 15.0f * lay.s;
		const int label_w = (int)std::ceil(loading_screen::text_width("000:00", lay.time_h));
		if (!lay.compact) {
			const int m = lay.rp(8), gap = lay.rp(6);
			lay.two_col = n > 4;
			lay.rows = lay.two_col ? (n + 1) / 2 : n;
			lay.card_h = lay.rp(lay.rows <= 2 ? 58.0f : lay.rows == 3 ? 46.0f : 44.0f);
			lay.card_gap = lay.rp(4);
			int content_h = lay.rows * lay.card_h + (lay.rows - 1) * lay.card_gap;
			if (content_h < lay.rp(120)) content_h = lay.rp(120);
			const int tl_h = lay.rp(44);
			lay.band_top = sh - (tl_h + content_h + m);
			const int cy0 = lay.band_top + tl_h, cy1 = cy0 + content_h;
			// control block, right: five transport buttons over [-] speed [+] [EXIT]
			const int bw = lay.rp(54);
			int bh = (content_h - gap) / 2; if (bh > lay.rp(60)) bh = lay.rp(60);
			const int cw = 5 * bw + 4 * gap;
			const int cx0 = sw - m - cw, by0 = cy0 + (content_h - (2 * bh + gap)) / 2, by1 = by0 + bh + gap;
			for (int i = 0; i < 5; ++i) lay.btn[i] = {{cx0 + i * (bw + gap), by0}, {cx0 + i * (bw + gap) + bw, by0 + bh}};
			lay.btn[rbtn_slower] = {{cx0, by1}, {cx0 + bw, by1 + bh}};
			lay.btn[rbtn_exit] = {{cx0 + cw - (2 * bw + gap), by1}, {cx0 + cw, by1 + bh}};
			lay.btn[rbtn_faster] = {{lay.btn[rbtn_exit].from.x - gap - bw, by1}, {lay.btn[rbtn_exit].from.x - gap, by1 + bh}};
			lay.speed = {{lay.btn[rbtn_slower].to.x + gap, by1}, {lay.btn[rbtn_faster].from.x - gap, by1 + bh}};
			int ms = content_h; if (ms > lay.rp(200)) ms = lay.rp(200);
			lay.minimap = {{m, cy0 + (content_h - ms) / 2}, {m + ms, cy0 + (content_h - ms) / 2 + ms}};
			lay.cards = {{m + ms + lay.rp(10), cy0}, {cx0 - lay.rp(10), cy1}};
			lay.timeline = {{0, lay.band_top}, {sw, cy0}};
			const int th = lay.rp(10), ty = lay.band_top + (tl_h - th) / 2 + lay.rp(1);
			lay.track = {{m + label_w + lay.rp(10), ty}, {sw - m - label_w - lay.rp(10), ty + th}};
			// side panel: under the production strip, down to the bar
			const int ptop = m + lay.rp(58) + m;
			int pw = lay.rp(372); if (pw > sw / 2) pw = sw / 2;
			lay.panel = {{m, ptop}, {m + pw, lay.band_top - m}};
			lay.flyout = lay.panel;
			const int cb = lay.rp(48);
			lay.close_btn = {{lay.panel.to.x - cb - lay.rp(4), lay.panel.from.y + lay.rp(4)}, {lay.panel.to.x - lay.rp(4), lay.panel.from.y + lay.rp(4) + cb}};
		} else {
			const int m = lay.rp(6), gap = lay.rp(5);
			const int tl_h = lay.rp(44), content_h = lay.rp(72);
			lay.band_top = sh - (tl_h + content_h + m);
			const int cy0 = lay.band_top + tl_h;
			const int bw = lay.rp(48), bh = lay.rp(48), by0 = cy0 + (content_h - bh) / 2;
			const int spw = lay.rp(50);
			int x = sw - m - bw;
			lay.btn[rbtn_faster] = {{x, by0}, {x + bw, by0 + bh}};
			x -= gap + spw;
			lay.speed = {{x, by0}, {x + spw, by0 + bh}};
			x -= gap + bw;
			lay.btn[rbtn_slower] = {{x, by0}, {x + bw, by0 + bh}};
			x -= 3 * gap;
			for (int i = 4; i >= 0; --i) { x -= gap + bw; lay.btn[i] = {{x, by0}, {x + bw, by0 + bh}}; }
			lay.minimap = {{m, cy0}, {m + content_h, cy0 + content_h}};
			lay.timeline = {{0, lay.band_top}, {sw, cy0}};
			const int th = lay.rp(10), ty = lay.band_top + (tl_h - th) / 2 + lay.rp(1);
			lay.track = {{m + label_w + lay.rp(8), ty}, {sw - m - label_w - lay.rp(8), ty + th}};
			// top-right: [PLAYERS] [EXIT]; top-left: the roster strip
			const int tbh = lay.rp(48), exw = lay.rp(76), plw = lay.rp(118);
			lay.btn[rbtn_exit] = {{sw - m - exw, m}, {sw - m, m + tbh}};
			lay.players_btn = {{lay.btn[rbtn_exit].from.x - gap - plw, m}, {lay.btn[rbtn_exit].from.x - gap, m + tbh}};
			lay.mini_row_h = lay.rp(22);
			int mw = lay.rp(270); if (mw > lay.players_btn.from.x - 2 * m) mw = lay.players_btn.from.x - 2 * m;
			lay.mini = {{m, m}, {m + mw, m + lay.mini_row_h * n}};
			// the PLAYERS panel fills the playfield under the top buttons
			lay.panel = {{m, m + tbh + gap}, {sw - m, lay.band_top - m}};
			const int head = lay.rp(48), pad = lay.rp(6);
			const int split = lay.panel.from.x + (lay.panel.to.x - lay.panel.from.x) * 60 / 100;
			lay.cards = {{lay.panel.from.x + pad, lay.panel.from.y + head}, {split - pad, lay.panel.to.y - pad}};
			lay.flyout = {{split, lay.panel.from.y + head}, {lay.panel.to.x - pad, lay.panel.to.y - pad}};
			lay.close_btn = {{lay.panel.to.x - lay.rp(52), lay.panel.from.y}, {lay.panel.to.x, lay.panel.from.y + head}};
			// four or more players: two columns, so the cards keep a tappable height
			lay.two_col = n > 3;
			lay.rows = lay.two_col ? (n + 1) / 2 : n;
			lay.card_gap = lay.rp(4);
			lay.card_h = lay.rp(48);
			const int avail = lay.cards.to.y - lay.cards.from.y;
			if (lay.rows * lay.card_h + (lay.rows - 1) * lay.card_gap > avail) lay.card_h = (avail - (lay.rows - 1) * lay.card_gap) / lay.rows;
			if (lay.card_h < lay.rp(22)) lay.card_h = lay.rp(22);
		}
		if (lay.band_top < sh / 3 || rp_empty(lay.track)) return lay;
		lay.ok = true;
		return lay;
	}

	// The player card at cache index i (one column; two columns of ceil(n/2) for 5+).
	rect replay_card_rect(const replay_layout_t& lay, int i) {
		if (!lay.ok || i < 0 || i >= lay.n) return {};
		const rect& c = lay.cards;
		if (!lay.two_col) {
			int y = c.from.y + i * (lay.card_h + lay.card_gap);
			return {{c.from.x, y}, {c.to.x, y + lay.card_h}};
		}
		const int gap = lay.rp(6), colw = (c.to.x - c.from.x - gap) / 2;
		const int col = i < lay.rows ? 0 : 1, row = i < lay.rows ? i : i - lay.rows;
		int x = c.from.x + col * (colw + gap), y = c.from.y + row * (lay.card_h + lay.card_gap);
		return {{x, y}, {x + colw, y + lay.card_h}};
	}
	// The cards are on screen all the time on a tablet, on a phone only in the panel.
	bool replay_cards_visible(const replay_layout_t& lay) const { return lay.ok && (!lay.compact || replay_players_open); }
	// Width of the vision toggle at the right end of a card.
	int replay_eye_w(const replay_layout_t& lay) const { return lay.rp(54); }
	rect observer_vision_btn_area(const replay_layout_t& lay, int idx) {
		rect r = replay_card_rect(lay, idx); if (rp_empty(r)) return {};
		const int in = lay.rp(1);
		return {{r.to.x - replay_eye_w(lay) - in, r.from.y + in}, {r.to.x - in, r.to.y - in}};
	}
	int observer_vision_btn_hit(int mx, int my) {
		replay_layout_t lay = replay_layout();
		if (!replay_cards_visible(lay)) return -1;
		for (int i = 0; i < lay.n; ++i) if (rp_in(observer_vision_btn_area(lay, i), mx, my)) return observer_cache[i].p;
		return -1;
	}
	// The player whose card is at (mx,my), or -1.
	int observer_row_hit(int mx, int my) {
		replay_layout_t lay = replay_layout();
		if (!replay_cards_visible(lay)) return -1;
		for (int i = 0; i < lay.n; ++i) if (rp_in(replay_card_rect(lay, i), mx, my)) return observer_cache[i].p;
		return -1;
	}
	int replay_btn_hit(int mx, int my) {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return -1;
		for (int i = 0; i < replay_btn_count; ++i) if (rp_in(lay.btn[i], mx, my)) return i;
		return -1;
	}
	// The timeline's drag area: the whole row, as wide as the track plus the handle.
	rect get_replay_slider_area() {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return {};
		// (a little of the world above the row counts too: the thumb is wide)
		const int hw = lay.rp(14);
		return {{lay.track.from.x - hw, lay.timeline.from.y - lay.rp(6)}, {lay.track.to.x + hw, lay.timeline.to.y}};
	}
	// Seek target for a pointer at mx on the timeline.
	int replay_frame_at(int mx) {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return replay_frame;
		const int tw = lay.track.to.x - lay.track.from.x, end = (int)replay_st.end_frame;
		int x = mx - lay.track.from.x;
		if (x < 0) x = 0; if (x > tw) x = tw;
		int f = (int)((int64_t)x * end / tw);
		if (f >= end) f = end - 1; if (f < 0) f = 0;
		return f;
	}
	// A player colour made readable on the dark panels (dark browns / blues are lifted).
	static uint32_t rp_legible(uint32_t rgb) {
		const int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
		const int lum = (r * 54 + g * 183 + b * 19) >> 8;
		if (lum >= 110) return rgb;
		const float k = (110 - lum) / 150.0f + 0.15f;
		return loading_screen::mix(rgb, 0xffffff, k > 0.6f ? 0.6f : k);
	}
	// --- timeline marks (from the scan) ---
	// A battle is a red block above the track, as wide as it lasted and taller the
	// more was destroyed; a new base is a diamond in its owner's colour below it.
	int replay_track_x(const replay_layout_t& lay, int frame) const {
		const int tw = lay.track.to.x - lay.track.from.x, end = (int)replay_st.end_frame;
		if (end <= 0) return lay.track.from.x;
		if (frame < 0) frame = 0; if (frame > end) frame = end;
		return lay.track.from.x + (int)((int64_t)frame * tw / end);
	}
	rect replay_battle_rect(const replay_layout_t& lay, const replay_scan_battle& b) const {
		int x0 = replay_track_x(lay, b.f0), x1 = replay_track_x(lay, b.f1);
		const int minw = lay.rp(4);
		if (x1 - x0 < minw) { x0 = (x0 + x1 - minw) / 2; x1 = x0 + minw; }
		int h = lay.rp(4) + (int)((int64_t)lay.rp(9) * (b.value > 3000 ? 3000 : b.value) / 3000);
		const int y1 = lay.track.from.y - lay.rp(2);
		return {{x0, y1 - h}, {x1, y1}};
	}
	xy replay_base_mark(const replay_layout_t& lay, const replay_scan_base& b) const {
		return xy(replay_track_x(lay, b.frame), lay.track.to.y + lay.rp(8));
	}
	// The mark under a press on the timeline row: sets the frame to seek to (a few
	// seconds before it) and the map position to show. Presses on the track itself
	// never hit a mark -- that is plain scrubbing.
	bool replay_mark_at(int mx, int my, int& frame, xy& pos) {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return false;
		const int lead = 3 * 1000 / 42, grow = lay.rp(5);
		if (my < lay.track.from.y) {
			for (auto& b : replay_scan_view.battles) {
				rect r = replay_battle_rect(lay, b);
				if (mx >= r.from.x - grow && mx < r.to.x + grow && my >= lay.timeline.from.y - lay.rp(6)) {
					frame = b.f0 > lead ? b.f0 - lead : 0; pos = xy(b.x, b.y);
					return true;
				}
			}
		} else if (my >= lay.track.to.y) {
			const replay_scan_base* best = nullptr; int bd = lay.rp(12);
			for (auto& b : replay_scan_view.bases) {
				int d = std::abs(replay_base_mark(lay, b).x - mx);
				if (d < bd) { bd = d; best = &b; }
			}
			if (best) {
				frame = best->frame; pos = xy(best->x, best->y);
				return true;
			}
		}
		return false;
	}
	bool replay_panel_visible(const replay_layout_t& lay) const {
		if (!lay.ok) return false;
		return lay.compact ? replay_players_open : observer_flyout_player >= 0;
	}
	rect replay_tab_rect(const replay_layout_t& lay, int i) {
		const rect& f = lay.flyout;
		const int pad = lay.rp(8), th = lay.rp(48);
		const int y = f.from.y + (lay.compact ? lay.rp(4) : lay.rp(64));
		const int w = (f.to.x - f.from.x - 2 * pad) / replay_tab_count;
		return {{f.from.x + pad + i * w, y}, {f.from.x + pad + (i + 1) * w, y + th}};
	}
	// A tap on a replay overlay that is not the bar itself: the phone's PLAYERS
	// button / roster strip, the panel's close button and tabs. True = consumed
	// (also for a tap on the panel's body, so nothing behind it reacts).
	bool replay_overlay_click(int mx, int my) {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return false;
		if (lay.compact && (rp_in(lay.players_btn, mx, my) || (!replay_players_open && rp_in(lay.mini, mx, my)))) {
			replay_players_open = !replay_players_open;
			return true;
		}
		if (!replay_panel_visible(lay)) return false;
		if (rp_in(lay.close_btn, mx, my)) {
			if (lay.compact) replay_players_open = false; else observer_flyout_player = -1;
			return true;
		}
		for (int i = 0; i < replay_tab_count; ++i) if (rp_in(replay_tab_rect(lay, i), mx, my)) { observer_flyout_tab = i; return true; }
		if (!rp_in(lay.panel, mx, my)) return false;
		// Inside the panel: the phone's cards live here and are handled by the caller.
		if (lay.compact && rp_in(lay.cards, mx, my)) return false;
		return true;
	}
	// A point the replay HUD covers above the bar (so a press there must not start a
	// drag-select in the world behind it).
	bool replay_overlay_at(int mx, int my) {
		replay_layout_t lay = replay_layout(); if (!lay.ok) return false;
		if (replay_panel_visible(lay) && rp_in(lay.panel, mx, my)) return true;
		if (lay.compact) return rp_in(lay.players_btn, mx, my) || rp_in(lay.btn[rbtn_exit], mx, my) || (!replay_players_open && rp_in(lay.mini, mx, my));
		return false;
	}

	// A player's vision toggle is ON when we are currently watching that player's
	// fog of war -- directly, or because the viewed player shares vision with them
	// (team game with shared vision). With no player selected (full-map view,
	// replay_vision_player < 0) every toggle is off.
	bool observer_vision_active(int p) {
		if (replay_vision_player < 0) return false;
		if (p == replay_vision_player) return true;
		return (st.shared_vision[(size_t)replay_vision_player] & (1u << p)) != 0;
	}

	// Per-player scans, cached (recomputed a few times a game second).
	static const int observer_max_prod = 5;
	struct observer_prod { int frame; int secs; uint32_t col; };
	struct observer_row {
		int p; int army; int workers; int nprod; observer_prod prod[observer_max_prod];
	};
	a_vector<observer_row> observer_cache;
	int observer_cache_frame = -1;
	std::vector<loading_screen::player_t> replay_players;   // identity of each cache row (name, race, colour, portrait)

	const char* observer_name(int p) {
		const char* nm = replay_st.player_name[p].c_str();
		if (nm && *nm) return nm;
		static char fb[16]; snprintf(fb, sizeof fb, "Player %d", p + 1); return fb;
	}

	void observer_rebuild_cache() {
		int f = (int)st.current_frame;
		if (observer_cache_frame >= 0 && f - observer_cache_frame < 8 && f >= observer_cache_frame) return;
		observer_cache_frame = f;
		observer_cache.clear();
		for (int p = 0; p != 8; ++p) {
			auto c = st.players[p].controller;
			// Real competitors: humans + AI. In a replay the recorded human
			// players can be marked controller_computer (the replay drives them),
			// so include all three controller kinds -- otherwise those rows vanish.
			if (c != player_t::controller_occupied && c != player_t::controller_computer
			    && c != player_t::controller_computer_game) continue;
			// (No ghost-slot filter: it was dropping real early-game players whose
			// gathered/kills counters were still 0. The controller check above is
			// enough to exclude empty lobby slots.)
			// An observer seat (tournament maps are hosted with one or two) is not a
			// competitor: it never owns a unit. A player who lost everything did
			// lose units, so stays listed.
			if (st.player_units[p].empty() && units_lost[(size_t)p] == 0 && observer_kills(p) == 0
			    && st.total_minerals_gathered[(size_t)p] + st.total_gas_gathered[(size_t)p] <= 50) continue;
			observer_row row{};
			row.p = p;
			bool zerg = (size_t)st.players[p].race == 0;
			for (unit_t* u : ptr(st.player_units[p])) {
				if (u_completed(u) && !ut_building(u)) {
					if (ut_worker(u)) ++row.workers; else ++row.army;
				}
				// Active production. Only buildings: u->building is a union, so
				// reading its fields on a non-building unit would be garbage. Does
				// not cover Zerg unit morphs (egg/lurker) -- accepted.
				if (ut_building(u) && row.nprod < observer_max_prod) {
					int frame = -1, rem = 0; uint32_t col = 0xff90d090u; // build/train green
					if (u->building.researching_type) {
						frame = tech_icon(u->building.researching_type->id);
						rem = u->building.upgrade_research_time;
						col = zerg ? 0xffc884ffu : 0xffe8c828u; // zerg evolve purple / research yellow
					} else if (u->building.upgrading_type) {
						frame = upgrade_icon(u->building.upgrading_type->id);
						rem = u->building.upgrade_research_time;
						col = zerg ? 0xffc884ffu : 0xffe8c828u;
					} else if (!u->build_queue.empty()) {
						frame = (int)u->build_queue[0]->id;
						rem = u->current_build_unit ? u->current_build_unit->remaining_build_time
						                            : u->remaining_build_time;
						col = zerg ? 0xffc884ffu : 0xff90d090u;
					} else if (!u_completed(u) && ut_building(u)) {
						frame = (int)u->unit_type->id;
						rem = u->remaining_build_time;
						col = zerg ? 0xffc884ffu : 0xff90d090u;
					}
					if (frame >= 0 && rem > 0) {
						row.prod[row.nprod++] = { frame, rem * 42 / 1000, col };
					}
				}
			}
			observer_cache.push_back(row);
		}
		// Identity of each row. A .rep stores names only, so the portrait is the
		// race's default one (same rule as the replay loading screen); with no
		// portrait data at all the card shows a colour badge with the race letter.
		replay_players.clear();
		for (auto& row : observer_cache) {
			loading_screen::player_t pl;
			pl.name = loading_screen::ascii_text(observer_name(row.p), std::string("Player ") + std::to_string(row.p + 1));
			pl.race = (int)st.players[row.p].race;
			if (pl.race < 0 || pl.race > 2) pl.race = -1;
			pl.color = loading_player_rgb(st.players[row.p].color);
			int id = player_portrait_id(false, false, replay_st.player_name[row.p], pl.race);
			if (id >= 0) {
				const loading_portrait_t& lp = loading_portrait(id);
				if (lp.px && lp.w > 0 && lp.h > 0) {
					pl.portrait_tile = true; pl.portrait_id = id;
					pl.portrait_w = lp.w; pl.portrait_h = lp.h; pl.portrait = lp.px;
				}
			}
			replay_players.push_back(std::move(pl));
		}
	}

	// --- drawing helpers ---------------------------------------------------------
	struct rp_hash {
		uint64_t v = 1469598103934665603ull;
		void i(int64_t x) { for (int k = 0; k != 8; ++k) { v ^= (uint8_t)(x >> (k * 8)); v *= 1099511628211ull; } }
		void s(const std::string& t) { for (char q : t) i((uint8_t)q); i(-1); }
		void r(const rect& q) { i(q.from.x); i(q.from.y); i(q.to.x); i(q.to.y); }
		uint64_t get() const { return v ? v : 1; }
	};
	// Copy a cached layer onto the frame, opaque (alpha 256) or at a constant alpha.
	void rp_blit(uint32_t* data, size_t pitch, const loading_screen::cache_t& l, int dx, int dy, int alpha = 256) {
		const int sw = (int)screen_width, sh = (int)screen_height;
		if (l.px.size() != (size_t)l.w * l.h) return;
		int x0 = dx < 0 ? -dx : 0, x1 = l.w;
		if (dx + x1 > sw) x1 = sw - dx;
		if (x1 <= x0) return;
		const int n = x1 - x0;
		const uint32_t a = (uint32_t)alpha, ia = 256 - a;
		for (int y = 0; y < l.h; ++y) {
			int py = dy + y;
			if (py < 0 || py >= sh) continue;
			const uint32_t* s = l.px.data() + (size_t)y * l.w + x0;
			uint32_t* d = data + (size_t)py * pitch + (dx + x0);
			if (alpha >= 256) { memcpy(d, s, (size_t)n * 4); continue; }
			for (int x = 0; x < n; ++x) {
				uint32_t sp = s[x], dp = d[x];
				uint32_t rb = (((sp & 0xff00ffu) * a + (dp & 0xff00ffu) * ia) >> 8) & 0xff00ffu;
				uint32_t g = (((sp & 0x00ff00u) * a + (dp & 0x00ff00u) * ia) >> 8) & 0x00ff00u;
				d[x] = 0xff000000u | rb | g;
			}
		}
	}
	// Tint a screen rectangle towards rgb (alpha 0..256): the backing of the small
	// overlays that sit directly on the game view.
	void rp_dim(uint32_t* data, size_t pitch, rect r, uint32_t rgb, int alpha) {
		if (r.from.x < 0) r.from.x = 0;
		if (r.from.y < 0) r.from.y = 0;
		if (r.to.x > (int)screen_width) r.to.x = (int)screen_width;
		if (r.to.y > (int)screen_height) r.to.y = (int)screen_height;
		const uint32_t a = (uint32_t)alpha, ia = 256 - a;
		const uint32_t srb = (rgb & 0xff00ffu) * a, sg = (rgb & 0x00ff00u) * a;
		for (int y = r.from.y; y < r.to.y; ++y) {
			uint32_t* d = data + (size_t)y * pitch;
			for (int x = r.from.x; x < r.to.x; ++x) {
				uint32_t dp = d[x];
				d[x] = 0xff000000u | (((srb + (dp & 0xff00ffu) * ia) >> 8) & 0xff00ffu) | (((sg + (dp & 0x00ff00u) * ia) >> 8) & 0x00ff00u);
			}
		}
	}
	// (Re)allocate a layer covering screen rect r; the canvas draws into it with
	// screen coordinates (see loading_screen::canvas::window).
	loading_screen::canvas rp_layer(loading_screen::cache_t& l, const rect& r) {
		const int w = r.to.x - r.from.x, h = r.to.y - r.from.y;
		l.px.assign((size_t)w * h, 0xff000000u); l.w = w; l.h = h;
		loading_screen::canvas c;
		c.data = l.px.data(); c.pitch = (size_t)w; c.w = (int)screen_width; c.h = (int)screen_height;
		c.window(r.from.x, r.from.y, w, h);
		return c;
	}
	loading_screen::canvas rp_screen(uint32_t* data, size_t pitch) {
		loading_screen::canvas c;
		c.data = data; c.pitch = pitch; c.w = (int)screen_width; c.h = (int)screen_height;
		return c;
	}
	// Horizontal span with anti-aliased ends (the vector glyphs below).
	static void rp_span(loading_screen::canvas& c, float xa, float xb, int y, uint32_t rgb, int alpha = 255) {
		int ia = (int)std::floor(xa), ib = (int)std::ceil(xb);
		for (int x = ia; x < ib; ++x) {
			float cov = std::min(xb, x + 1.0f) - std::max(xa, (float)x);
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			c.blend(x, y, rgb, (int)(alpha * cov));
		}
	}
	// Triangle pointing right (dir > 0) or left, centred on (cx, cy).
	static void rp_tri_h(loading_screen::canvas& c, float cx, float cy, float hw, float hh, int dir, uint32_t rgb) {
		int y0 = (int)std::floor(cy - hh), y1 = (int)std::ceil(cy + hh);
		for (int y = y0; y < y1; ++y) {
			float t = 1.0f - std::fabs(y + 0.5f - cy) / hh;
			if (t <= 0) continue;
			float len = 2 * hw * t;
			float xa = dir > 0 ? cx - hw : cx + hw - len;
			rp_span(c, xa, xa + len, y, rgb);
		}
	}
	static void rp_tri_up(loading_screen::canvas& c, float cx, float cy, float hw, float hh, uint32_t rgb) {
		int y0 = (int)std::floor(cy - hh), y1 = (int)std::ceil(cy + hh);
		for (int y = y0; y < y1; ++y) {
			float t = (y + 0.5f - (cy - hh)) / (2 * hh);
			if (t <= 0) continue;
			if (t > 1) t = 1;
			rp_span(c, cx - hw * t, cx + hw * t, y, rgb);
		}
	}
	static void rp_diamond(loading_screen::canvas& c, float cx, float cy, float r, uint32_t rgb) {
		int y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
		for (int y = y0; y < y1; ++y) {
			float half = r - std::fabs(y + 0.5f - cy);
			if (half <= 0) continue;
			rp_span(c, cx - half * 0.8f, cx + half * 0.8f, y, rgb);
		}
	}
	static void rp_bar(loading_screen::canvas& c, float x0, float y0, float x1, float y1, uint32_t rgb) {
		c.fill((int)(x0 + 0.5f), (int)(y0 + 0.5f), (int)(x1 + 0.5f), (int)(y1 + 0.5f), rgb, 255);
	}
	static void rp_cross(loading_screen::canvas& c, float cx, float cy, float r, float t, uint32_t rgb) {
		for (float k = -r; k <= r; k += 0.5f) {
			c.disc(cx + k, cy + k, t, rgb, 255);
			c.disc(cx + k, cy - k, t, rgb, 255);
		}
	}
	// A launcher button body: 0 secondary (steel), 1 primary (green).
	static void rp_button(loading_screen::canvas& c, const rect& r, int kind) {
		namespace ls = loading_screen;
		const int u = std::max(1, c.h / 400);
		if (kind == 1) ls::fill_vgrad(c, r.from.x, r.from.y, r.to.x, r.to.y, ls::pal::sel_top, ls::pal::sel_bot);
		else ls::fill_vgrad(c, r.from.x, r.from.y, r.to.x, r.to.y, ls::pal::btn_top, ls::pal::btn_bot);
		c.frame(r.from.x, r.from.y, r.to.x, r.to.y, u, kind == 1 ? ls::pal::label : ls::pal::steel, 255);
	}
	static void rp_text_center(loading_screen::canvas& c, const rect& r, const std::string& s, uint32_t rgb, float h, float tracking = 0) {
		namespace ls = loading_screen;
		const float w = (float)(r.to.x - r.from.x);
		float fh = ls::fit_height(s, h, h * 0.5f, w * 0.90f, tracking);
		ls::draw_text_centered(c, (r.from.x + r.to.x) * 0.5f, (r.from.y + r.to.y) * 0.5f - fh * 0.5f, s, rgb, fh, tracking);
	}
	// The glyph of a control button (EXIT and the pause/play state included).
	void rp_button_glyph(loading_screen::canvas& c, const replay_layout_t& lay, int b, bool paused) {
		namespace ls = loading_screen;
		const rect& r = lay.btn[b];
		const float cx = (r.from.x + r.to.x) * 0.5f, cy = (r.from.y + r.to.y) * 0.5f;
		const float g = std::min(r.to.x - r.from.x, r.to.y - r.from.y) * 0.19f;   // half size of a glyph
		const float t = std::max(2.0f, g * 0.36f);                                 // bar thickness
		const uint32_t col = ls::pal::accent;
		const float th = 15.0f * lay.s;
		switch (b) {
		case rbtn_start:
			rp_bar(c, cx - g - t, cy - g, cx - g, cy + g, col);
			rp_tri_h(c, cx + t * 0.5f, cy, g, g, -1, col);
			break;
		case rbtn_end:
			rp_bar(c, cx + g, cy - g, cx + g + t, cy + g, col);
			rp_tri_h(c, cx - t * 0.5f, cy, g, g, 1, col);
			break;
		case rbtn_back: rp_text_center(c, r, "-10", col, th); break;
		case rbtn_fwd: rp_text_center(c, r, "+10", col, th); break;
		case rbtn_pause:
			if (paused) rp_tri_h(c, cx + g * 0.1f, cy, g, g, 1, ls::pal::title);
			else {
				rp_bar(c, cx - g * 0.75f, cy - g, cx - g * 0.75f + t * 1.3f, cy + g, ls::pal::title);
				rp_bar(c, cx + g * 0.75f - t * 1.3f, cy - g, cx + g * 0.75f, cy + g, ls::pal::title);
			}
			break;
		case rbtn_slower: rp_bar(c, cx - g * 0.8f, cy - t * 0.5f, cx + g * 0.8f, cy + t * 0.5f, col); break;
		case rbtn_faster:
			rp_bar(c, cx - g * 0.8f, cy - t * 0.5f, cx + g * 0.8f, cy + t * 0.5f, col);
			rp_bar(c, cx - t * 0.5f, cy - g * 0.8f, cx + t * 0.5f, cy + g * 0.8f, col);
			break;
		case rbtn_exit: rp_text_center(c, r, L("EXIT"), col, th, 0.06f); break;
		}
	}

	// --- player cards --------------------------------------------------------------
	// Column metrics shared by every card of a layout (so the numbers line up).
	// Three shapes, tried in this order until everything fits the card's width:
	//   one line     portrait | name over race | resources | stats | eye
	//   two lines    portrait | name  resources / stats | eye
	//   stacked      [portrait] name / resources / APM ARM WK / K L income | eye
	// and the text shrinks (down to ~60 %) before a shape is given up on.
	struct replay_card_metrics_t {
		bool two_line = false, stacked = false, tile = true;
		float name_h = 0, small_h = 0, num_h = 0, stat_h = 0;
		int edge = 0, pad = 0, ft = 0, tile_avail = 0, tile_w = 0;
		int x_name = 0, name_w = 0;          // offsets from the card's left edge
		int x_res[3] = {0, 0, 0};            // minerals, gas, supply (glyph, then the number)
		int res_glyph = 0;
		int x_stats = 0;
		int stat_label_x[6] = {0}, stat_value_x[6] = {0};
	};
	struct replay_card_rows_t { float name_y = 0, race_y = -1, num_y = 0, stat_y = 0, stat2_y = 0; };
	static const char* replay_stat_label(int i) {
		static const char* lbl[6] = {"APM", "ARM", "WK", "K", "L", ""};
		return lbl[i];
	}
	replay_card_metrics_t replay_card_metrics(const replay_layout_t& lay) {
		namespace ls = loading_screen;
		replay_card_metrics_t m;
		if (!lay.ok || lay.n < 1) return m;
		rect r0 = replay_card_rect(lay, 0);
		const int cw = r0.to.x - r0.from.x, ch = lay.card_h;
		const int u = std::max(1, (int)screen_height / 400);
		m.edge = lay.rp(4); m.pad = lay.rp(3); m.ft = u;
		m.tile_avail = ch - 2 * m.pad - 2 * m.ft;
		int full_tile_w;
		{
			ls::player_t probe;     // every portrait clip is 60x56; fit once
			ls::portrait_tile_t g = ls::portrait_tile_fit(probe, m.tile_avail, m.ft);
			full_tile_w = g.tw();
		}
		static const int digits[6] = {3, 3, 3, 3, 3, 9};   // the last one is "+9999/min"
		const int right = cw - replay_eye_w(lay) - lay.rp(2) - lay.rp(8);
		const bool roomy = ch >= lay.rp(52);
		// shape 0 = one line, 1 = two lines, 2 = stacked (with the portrait), 3 = stacked without it
		for (int shape = 0; shape < 4; ++shape) {
			for (int shrink = 0; shrink < 7; ++shrink) {
				const float f = 1.0f - 0.07f * shrink;
				m.two_line = shape == 1; m.stacked = shape >= 2; m.tile = shape != 3;
				m.tile_w = m.tile ? full_tile_w : 0;
				m.name_h = (roomy ? 16.0f : 14.0f) * lay.s * f;
				m.small_h = 11.5f * lay.s * f;
				m.num_h = (roomy ? 15.0f : 13.5f) * lay.s * f;
				m.stat_h = 12.0f * lay.s * f;
				const float avail = (float)(ch - 2 * m.pad) - 2.0f * lay.s, gap = 3.0f * lay.s;
				if (m.two_line) {
					float need = m.num_h + m.stat_h + gap;
					if (need > avail) { float k = avail / need; m.name_h *= k; m.num_h *= k; m.stat_h *= k; }
					if (m.name_h > m.num_h) m.name_h = m.num_h;
				} else if (m.stacked) {
					float need = m.name_h + m.num_h + 2 * m.stat_h + 3 * gap;
					if (need > avail) {
						if (shrink == 0 && shape == 2) break;           // not tall enough for four lines: try without the tile? no -- same height; give up on stacking
						float k = avail / need; m.name_h *= k; m.num_h *= k; m.stat_h *= k; m.small_h *= k;
					}
				}
				float widest = 0;
				for (auto& pl : replay_players) widest = std::max(widest, ls::text_width(pl.name, m.name_h));
				float nmin = ls::text_advance(m.name_h) * 6, nmax = ls::text_advance(m.name_h) * 15;
				m.name_w = (int)std::ceil(std::min(nmax, std::max(nmin, widest)));
				m.x_name = m.edge + m.pad + m.tile_w + (m.tile ? lay.rp(8) : lay.rp(4));
				m.res_glyph = (int)(m.num_h * 0.82f);
				const int rg = m.res_glyph + (int)(m.num_h * 0.30f);
				const int col_gap = (int)(m.num_h * 0.95f);
				int x = m.stacked ? m.x_name : m.x_name + m.name_w + (int)(m.num_h * 1.2f);
				m.x_res[0] = x; x += rg + (int)std::ceil(ls::text_width("99999", m.num_h)) + col_gap;
				m.x_res[1] = x; x += rg + (int)std::ceil(ls::text_width("99999", m.num_h)) + col_gap;
				m.x_res[2] = x; x += rg + (int)std::ceil(ls::text_width("200/200", m.num_h));
				const int res_end = x;
				int sx = m.two_line || m.stacked ? m.x_name : res_end + (int)(m.num_h * 1.6f);
				m.x_stats = sx;
				int stats_end = 0;
				for (int i = 0; i < 6; ++i) {
					if (m.stacked && i == 3) { stats_end = std::max(stats_end, sx); sx = m.x_name; }   // second stats line
					m.stat_label_x[i] = sx;
					std::string lb = replay_stat_label(i);
					if (!lb.empty()) sx += (int)std::ceil(ls::text_width(lb, m.stat_h, 0.04f) + m.stat_h * 0.45f);
					m.stat_value_x[i] = sx;
					sx += (int)std::ceil(ls::text_advance(m.stat_h) * digits[i]);
					if (i != 5) sx += (int)(m.stat_h * 0.9f);
				}
				stats_end = std::max(stats_end, sx);
				const int widest_line = m.stacked ? std::max(std::max(m.x_name + m.name_w, res_end), stats_end) : std::max(res_end, stats_end);
				if (widest_line <= right) return m;
			}
		}
		return m;   // nothing fits even stacked and shrunk: the smallest stacked shape, clipped by the eye
	}
	// Text tops of a card's rows.
	replay_card_rows_t replay_card_rows(const replay_layout_t& lay, const replay_card_metrics_t& m, const rect& r) {
		replay_card_rows_t o;
		const float cy = (r.from.y + r.to.y) * 0.5f, gap = 3.0f * lay.s;
		if (m.stacked) {
			const float block = m.name_h + m.num_h + 2 * m.stat_h + 3 * gap;
			o.name_y = cy - block * 0.5f;
			o.num_y = o.name_y + m.name_h + gap;
			o.stat_y = o.num_y + m.num_h + gap;
			o.stat2_y = o.stat_y + m.stat_h + gap;
		} else if (!m.two_line) {
			const bool race = r.to.y - r.from.y >= lay.rp(44);
			const float block = m.name_h + (race ? gap + m.small_h : 0);
			o.name_y = cy - block * 0.5f;
			o.race_y = race ? o.name_y + m.name_h + gap : -1;
			o.num_y = cy - m.num_h * 0.5f;
			o.stat_y = o.stat2_y = cy - m.stat_h * 0.5f;
		} else {
			const float block = m.num_h + gap + m.stat_h;
			o.num_y = cy - block * 0.5f;
			o.name_y = o.num_y + (m.num_h - m.name_h) * 0.5f;
			o.stat_y = o.stat2_y = o.num_y + m.num_h + gap;
		}
		return o;
	}
	// The part of a card that never changes during the replay.
	void replay_card_static(loading_screen::canvas& c, const replay_layout_t& lay, const replay_card_metrics_t& m, int i) {
		namespace ls = loading_screen;
		if (i >= (int)replay_players.size()) return;
		const ls::player_t& pl = replay_players[(size_t)i];
		rect r = replay_card_rect(lay, i); if (rp_empty(r)) return;
		const int u = std::max(1, c.h / 400);
		ls::draw_panel(c, r.from.x, r.from.y, r.to.x, r.to.y);
		c.fill(r.from.x, r.from.y, r.from.x + m.edge, r.to.y, pl.color, 255);
		const int tx = r.from.x + m.edge + m.pad;
		if (!m.tile) {
			// no room for the tile: the race letter sits in the colour edge's place
		} else if (pl.portrait_tile) {
			ls::portrait_tile_t g = ls::portrait_tile_fit(pl, m.tile_avail, m.ft);
			ls::draw_portrait_tile(c, pl, g, tx, r.from.y + (r.to.y - r.from.y - g.th()) / 2, false);
		} else {
			// No portrait data: a colour badge with the race letter (the loading screen's fallback).
			const int bs = std::min(m.tile_w, r.to.y - r.from.y - 2 * m.pad), by = r.from.y + (r.to.y - r.from.y - bs) / 2;
			ls::fill_vgrad(c, tx, by, tx + bs, by + bs, ls::mix(pl.color, 0xffffff, 0.10f), ls::mix(pl.color, 0x000000, 0.28f));
			c.frame(tx, by, tx + bs, by + bs, u, 0x000000, 70);
			const char* letter = pl.race == 0 ? "Z" : pl.race == 1 ? "T" : pl.race == 2 ? "P" : "?";
			float lh = bs * 0.62f;
			ls::draw_text_centered(c, tx + bs * 0.5f, by + (bs - lh) * 0.5f, letter, ls::luma(pl.color) > 150 ? 0x101418 : 0xffffff, lh);
		}
		replay_card_rows_t ro = replay_card_rows(lay, m, r);
		std::string nm = ls::ellipsize(pl.name, m.name_h, (float)m.name_w);
		if (!m.tile) {
			// race letter in front of the name, in the player's colour
			const char* letter = pl.race == 0 ? "Z" : pl.race == 1 ? "T" : pl.race == 2 ? "P" : "?";
			nm = std::string(letter) + " " + nm;
		}
		ls::draw_text(c, (float)(r.from.x + m.x_name), ro.name_y, nm, ls::pal::title, m.name_h);
		if (ro.race_y >= 0) {
			const char* race = pl.race == 0 ? "Zerg" : pl.race == 1 ? "Terran" : pl.race == 2 ? "Protoss" : "";
			ls::draw_text(c, (float)(r.from.x + m.x_name), ro.race_y, race, ls::pal::text2, m.small_h);
		}
		// resource glyphs: minerals (diamond), gas (disc), supply (triangle)
		const float gy = ro.num_y + m.num_h * 0.5f, gr = m.res_glyph * 0.5f;
		rp_diamond(c, r.from.x + m.x_res[0] + gr, gy, gr, 0x30d8f0);
		c.disc(r.from.x + m.x_res[1] + gr, gy, gr * 0.82f, 0x5fe07a, 255);
		rp_tri_up(c, r.from.x + m.x_res[2] + gr, gy, gr * 0.9f, gr * 0.82f, ls::pal::title);
		for (int k = 0; k < 6; ++k) {
			std::string lb = replay_stat_label(k);
			const float y = m.stacked && k >= 3 ? ro.stat2_y : ro.stat_y;
			if (!lb.empty()) ls::draw_text(c, (float)(r.from.x + m.stat_label_x[k]), y, lb, ls::pal::text2, m.stat_h, 0.04f);
		}
	}
	// What a card shows that changes while watching.
	struct replay_vals_t {
		int p = 0, minerals = 0, gas = 0, su = 0, sm = 0, apm = 0, army = 0, workers = 0, kills = 0, lost = 0, income = 0;
	};
	void replay_card_dynamic(loading_screen::canvas& c, const replay_layout_t& lay, const replay_card_metrics_t& m, int i,
			const replay_vals_t& v, bool selected, bool vision) {
		namespace ls = loading_screen;
		rect r = replay_card_rect(lay, i); if (rp_empty(r)) return;
		const int u = std::max(1, c.h / 400);
		rect eye = observer_vision_btn_area(lay, i);
		if (selected) {
			// the launcher's "selected row": a green tint and the green edge
			c.fill(r.from.x + m.x_name - lay.rp(4), r.from.y + u, eye.from.x - lay.rp(2), r.to.y - u, ls::pal::label, ls::pal::sel_tint);
			c.frame(r.from.x, r.from.y, r.to.x, r.to.y, u, ls::pal::label, 255);
		}
		replay_card_rows_t ro = replay_card_rows(lay, m, r);
		const float num_y = ro.num_y;
		const int rg = m.res_glyph + (int)(m.num_h * 0.30f);
		char b[24];
		snprintf(b, sizeof b, "%d", v.minerals);
		ls::draw_text(c, (float)(r.from.x + m.x_res[0] + rg), num_y, b, 0x30d8f0, m.num_h);
		snprintf(b, sizeof b, "%d", v.gas);
		ls::draw_text(c, (float)(r.from.x + m.x_res[1] + rg), num_y, b, 0x5fe07a, m.num_h);
		snprintf(b, sizeof b, "%d/%d", v.su, v.sm);
		ls::draw_text(c, (float)(r.from.x + m.x_res[2] + rg), num_y, b, v.su >= v.sm ? ls::pal::danger : ls::pal::title, m.num_h);
		const int vals[5] = {v.apm, v.army, v.workers, v.kills, v.lost};
		for (int k = 0; k < 5; ++k) {
			snprintf(b, sizeof b, "%d", vals[k]);
			ls::draw_text(c, (float)(r.from.x + m.stat_value_x[k]), m.stacked && k >= 3 ? ro.stat2_y : ro.stat_y, b, ls::pal::accent, m.stat_h);
		}
		snprintf(b, sizeof b, "+%d/min", v.income);
		ls::draw_text(c, (float)(r.from.x + m.stat_value_x[5]), m.stacked ? ro.stat2_y : ro.stat_y, b, ls::pal::text, m.stat_h);
		// vision toggle: an eye -- lit while we watch through this player's fog
		rp_button(c, eye, vision ? 1 : 0);
		const float ex = (eye.from.x + eye.to.x) * 0.5f, ey = (eye.from.y + eye.to.y) * 0.5f;
		const float er = std::min(eye.to.x - eye.from.x, eye.to.y - eye.from.y) * 0.24f;
		const uint32_t body = vision ? ls::mix(ls::pal::sel_top, ls::pal::sel_bot, 0.5f) : ls::mix(ls::pal::btn_top, ls::pal::btn_bot, 0.5f);
		c.disc(ex, ey, er, vision ? ls::pal::label : ls::pal::text2, 255);
		c.disc(ex, ey, er * 0.70f, body, 255);
		if (vision) c.disc(ex, ey, er * 0.36f, ls::pal::title, 255);
	}

	// --- the bottom bar -----------------------------------------------------------
	struct replay_hud_cache_t {
		loading_screen::cache_t chrome;      // the bar's static part
		loading_screen::cache_t comp;        // chrome + the numbers (what is copied to the frame)
		loading_screen::cache_t panel;       // the side / PLAYERS panel's static part
		a_vector<replay_vals_t> vals;
		std::chrono::steady_clock::time_point sampled{};
		bool sampled_once = false;
		void release() { chrome.release(); comp.release(); panel.release(); vals.clear(); sampled_once = false; }
	} replay_hud;

	static void rp_time(char* out, size_t n, int frame) {
		int s = (int)((int64_t)frame * 42 / 1000);
		snprintf(out, n, "%d:%02d", s / 60, s % 60);
	}

	void replay_draw_bar_chrome(loading_screen::canvas& c, const replay_layout_t& lay, const replay_card_metrics_t& m) {
		namespace ls = loading_screen;
		const int sw = (int)screen_width, sh = (int)screen_height;
		const int u = std::max(1, sh / 400);
		// body: the launcher's dark base, a red hairline along the top
		ls::fill_vgrad(c, 0, lay.band_top, sw, sh, 0x0b1114, ls::pal::base);
		c.fill(0, lay.band_top, sw, lay.band_top + u, ls::pal::line, 255);
		c.fill(0, lay.band_top + u, sw, lay.band_top + 2 * u, 0x000000, 255);
		// timeline track + minute ticks + the total time
		const rect& t = lay.track;
		c.fill(t.from.x - u, t.from.y - u, t.to.x + u, t.to.y + u, 0x000000, 255);
		c.fill(t.from.x, t.from.y, t.to.x, t.to.y, ls::pal::track, 255);
		{
			const int end = (int)replay_st.end_frame, tw = t.to.x - t.from.x;
			const double per_min = 60.0 * 1000.0 / 42.0;
			int step = 1;
			while (end / (per_min * step) > 45) step = step == 1 ? 5 : step * 2;
			for (int k = 1; k * per_min * step < end; ++k) {
				int x = t.from.x + (int)((double)tw * (k * per_min * step) / end);
				c.fill(x, (t.from.y + t.to.y) / 2, x + u, t.to.y, ls::pal::steel, 150);
			}
			char tb[24]; rp_time(tb, sizeof tb, end);
			ls::draw_text(c, (float)(t.to.x + lay.rp(10)), (lay.timeline.from.y + lay.timeline.to.y) * 0.5f - lay.time_h * 0.5f + lay.rp(1), tb, ls::pal::text2, lay.time_h);
		}
		// minimap well
		const rect& mm = lay.minimap;
		ls::draw_panel(c, mm.from.x, mm.from.y, mm.to.x, mm.to.y, true);
		c.fill(mm.from.x + 2 * u, mm.from.y + 2 * u, mm.to.x - 2 * u, mm.to.y - 2 * u, 0x000000, 255);
		if (!lay.compact) for (int i = 0; i < lay.n; ++i) replay_card_static(c, lay, m, i);
		// controls (pause/play and the speed value are drawn with the numbers)
		for (int b = 0; b < replay_btn_count; ++b) {
			if (b == rbtn_pause || (lay.compact && b == rbtn_exit)) continue;
			rp_button(c, lay.btn[b], 0);
			rp_button_glyph(c, lay, b, false);
		}
		c.fill(lay.speed.from.x, lay.speed.from.y, lay.speed.to.x, lay.speed.to.y, 0x000000, 120);
		c.frame(lay.speed.from.x, lay.speed.from.y, lay.speed.to.x, lay.speed.to.y, u, ls::pal::rim, 255);
	}

	void draw_replay_bar(uint32_t* data, size_t pitch, const replay_layout_t& lay) {
		namespace ls = loading_screen;
		const int sw = (int)screen_width, sh = (int)screen_height;
		const rect band{{0, lay.band_top}, {sw, sh}};
		replay_card_metrics_t m = replay_card_metrics(lay);
		// 1. static chrome
		rp_hash h;
		h.i(sw); h.i(sh); h.i(lay.compact); h.i(lay.n); h.i(lay.band_top); h.i(lay.card_h); h.i(m.two_line); h.i((int)(m.num_h * 16));
		h.i((int64_t)replay_st.end_frame); h.i(lang_lt);
		for (auto& pl : replay_players) { h.s(pl.name); h.i(pl.race); h.i(pl.color); h.i(pl.portrait_tile); h.i(pl.portrait_id); h.i(pl.portrait_w); }
		bool fresh = false;
		if (replay_hud.chrome.sig != h.get() || replay_hud.chrome.w != sw || replay_hud.chrome.h != sh - lay.band_top) {
			ls::canvas c = rp_layer(replay_hud.chrome, band);
			replay_draw_bar_chrome(c, lay, m);
			replay_hud.chrome.sig = h.get();
			fresh = true;
		}
		// 2. the numbers: sampled a few times a second (APM alone changes every frame;
		//    nobody reads digits faster), at once after a tap that changes what is shown.
		auto now = std::chrono::steady_clock::now();
		const int flyout_shown = lay.compact && !replay_players_open ? -1 : observer_flyout_player;
		rp_hash hi;
		hi.i(is_paused); hi.i(game_speed.raw_value); hi.i(flyout_shown); hi.i(replay_vision_player); hi.i(lay.n);
		if (fresh || !replay_hud.sampled_once || (int)replay_hud.vals.size() != lay.n
			|| now - replay_hud.sampled >= std::chrono::milliseconds(120) || replay_hud_instant != hi.get()) {
			replay_hud.sampled = now; replay_hud.sampled_once = true;
			replay_hud_instant = hi.get();
			replay_hud.vals.clear();
			for (auto& row : observer_cache) {
				replay_vals_t v;
				int p = row.p;
				size_t race = (size_t)st.players[p].race; if (race > 2) race = 0;
				apm[p].update((int)st.current_frame);
				v.p = p;
				v.minerals = st.current_minerals[p]; v.gas = st.current_gas[p];
				v.su = st.supply_used[p][race].integer_part();
				v.sm = st.supply_available[p][race].integer_part(); if (v.sm > 200) v.sm = 200;
				v.apm = apm[p].current_apm; v.army = row.army; v.workers = row.workers;
				v.kills = observer_kills(p); v.lost = units_lost[p]; v.income = observer_income_per_min(p);
				replay_hud.vals.push_back(v);
			}
		}
		rp_hash hc;
		hc.i((int64_t)replay_hud.chrome.sig); hc.i((int64_t)replay_hud_instant);
		hc.i((int)((int64_t)st.current_frame * 42 / 1000));
		for (auto& v : replay_hud.vals) {
			hc.i(v.p); hc.i(v.minerals); hc.i(v.gas); hc.i(v.su); hc.i(v.sm); hc.i(v.apm); hc.i(v.army); hc.i(v.workers);
			hc.i(v.kills); hc.i(v.lost); hc.i(v.income); hc.i(observer_vision_active(v.p));
		}
		if (replay_hud.comp.sig != hc.get() || replay_hud.comp.px.size() != replay_hud.chrome.px.size()) {
			replay_hud.comp.px = replay_hud.chrome.px;
			replay_hud.comp.w = replay_hud.chrome.w; replay_hud.comp.h = replay_hud.chrome.h;
			replay_hud.comp.sig = hc.get();
			ls::canvas c;
			c.data = replay_hud.comp.px.data(); c.pitch = (size_t)sw; c.w = sw; c.h = sh;
			c.window(0, lay.band_top, sw, sh - lay.band_top);
			if (!lay.compact) for (int i = 0; i < lay.n && i < (int)replay_hud.vals.size(); ++i) {
				const replay_vals_t& v = replay_hud.vals[(size_t)i];
				replay_card_dynamic(c, lay, m, i, v, v.p == observer_flyout_player, observer_vision_active(v.p));
			}
			rp_button(c, lay.btn[rbtn_pause], 1);
			rp_button_glyph(c, lay, rbtn_pause, is_paused);
			char sp[16]; snprintf(sp, sizeof sp, "%gx", (double)game_speed.raw_value / 256.0);
			rp_text_center(c, lay.speed, sp, ls::pal::text, 15.0f * lay.s);
			char tb[24]; rp_time(tb, sizeof tb, (int)st.current_frame);
			float tw = ls::text_width(tb, lay.time_h);
			ls::draw_text(c, (float)(lay.track.from.x - lay.rp(10)) - tw, (lay.timeline.from.y + lay.timeline.to.y) * 0.5f - lay.time_h * 0.5f + lay.rp(1), tb, ls::pal::accent, lay.time_h);
		}
		rp_blit(data, pitch, replay_hud.comp, 0, lay.band_top);
		// 3. per frame: the played part of the timeline and its handle (the seek target)
		{
			ls::canvas sc = rp_screen(data, pitch);
			const int u = std::max(1, sh / 400);
			const rect& t = lay.track;
			const int tw = t.to.x - t.from.x, end = (int)replay_st.end_frame;
			int x = end > 0 ? (int)((int64_t)replay_frame * tw / end) : 0;
			if (x < 0) x = 0; if (x > tw) x = tw;
			if (x > 0) {
				ls::fill_vgrad(sc, t.from.x, t.from.y, t.from.x + x, t.to.y, 0x3f7896, 0x2a5570);
				sc.fill(t.from.x, t.from.y, t.from.x + x, t.from.y + u, 0x7fb4d0, 255);
			}
			// rewind states (seeking back to one of these is instant): notches on the top edge
			for (int f : replay_seek_frames) {
				const int sx = replay_track_x(lay, f);
				sc.fill(sx, t.from.y, sx + u, t.from.y + lay.rp(3), ls::pal::title, 90);
			}
			// the scan: how far it got (while it runs), battles above, new bases below
			const replay_scan_data& sd = replay_scan_view;
			if (replay_scan && !sd.done && sd.frames < end)
				sc.fill(t.from.x, t.to.y + u, replay_track_x(lay, sd.frames), t.to.y + 2 * u, ls::pal::label, 150);
			for (auto& b : sd.battles) {
				rect r = replay_battle_rect(lay, b);
				sc.fill(r.from.x - u, r.from.y - u, r.to.x + u, r.to.y, 0x000000, 255);
				sc.fill(r.from.x, r.from.y, r.to.x, r.to.y, ls::pal::danger, 255);
			}
			for (auto& b : sd.bases) {
				xy c = replay_base_mark(lay, b);
				const uint32_t col = b.player >= 0 && b.player < 8 ? rp_legible(loading_player_rgb(st.players[(size_t)b.player].color)) : ls::pal::label;
				rp_diamond(sc, (float)c.x, (float)c.y, 6.6f * lay.s, 0x000000);
				rp_diamond(sc, (float)c.x, (float)c.y, 5.2f * lay.s, col);
			}
			const int hw = lay.rp(5), hh = lay.rp(5);
			sc.fill(t.from.x + x - hw - u, t.from.y - hh - u, t.from.x + x + hw + u, t.to.y + hh + u, 0x000000, 255);
			sc.fill(t.from.x + x - hw, t.from.y - hh, t.from.x + x + hw, t.to.y + hh, ls::pal::accent, 255);
		}
		draw_minimap_scaled(data, pitch);
	}

	// --- active production, top-left (tablet) ------------------------------------
	// Each cell: the owner's colour along the top, the icon, a type-coloured timer.
	void draw_replay_production(uint32_t* data, size_t pitch, const replay_layout_t& lay) {
		namespace ls = loading_screen;
		if (lay.compact) return;                       // phone: hidden (spec 14.2)
		load_cmdicons();
		ls::canvas sc = rp_screen(data, pitch);
		const int u = std::max(1, (int)screen_height / 400);
		const int m = lay.rp(8), icon = lay.rp(36), cellw = icon + lay.rp(6), cellh = lay.rp(58), gap = lay.rp(4);
		const float th = 11.5f * lay.s;
		const int maxx = (int)screen_width * 45 / 100;
		int x = m, y = m;
		for (auto& r : observer_cache) {
			if (r.nprod <= 0) continue;
			uint32_t pc = loading_player_rgb(st.players[r.p].color);
			for (int i = 0; i < r.nprod; ++i) {
				if (x + cellw > maxx) { x = m; y += cellh + gap; }
				rect cell{{x, y}, {x + cellw, y + cellh}};
				rp_dim(data, pitch, cell, ls::pal::panel, 224);
				sc.frame(cell.from.x, cell.from.y, cell.to.x, cell.to.y, u, ls::pal::line, 255);
				sc.fill(cell.from.x, cell.from.y, cell.to.x, cell.from.y + lay.rp(3), pc, 255);
				rect ic{{x + lay.rp(3), y + lay.rp(5)}, {x + lay.rp(3) + icon, y + lay.rp(5) + icon}};
				if (cmdicons_ok) draw_cmdicon_fit(data, pitch, r.prod[i].frame, ic);
				char tb[12]; int s = r.prod[i].secs;
				snprintf(tb, sizeof tb, "%d:%02d", s / 60, s % 60);
				ls::draw_text_centered(sc, x + cellw * 0.5f, (float)(cell.to.y - lay.rp(3)) - th, tb, r.prod[i].col & 0xffffffu, th);
				x += cellw + gap;
			}
		}
	}

	// --- the side panel (tablet) / PLAYERS panel (phone) ---------------------------
	// What the tapped player has: army composition, upgrades, buildings -- one
	// section per tab. Pure st read.
	a_vector<std::pair<int,int>> replay_flyout_items(int p, int tab) {
		a_vector<std::pair<int,int>> v;
		if (p < 0 || p >= 8 || tab == replay_tab_economy) return v;
		if (tab == 1) {
			for (int i = 0; i < (int)UpgradeTypes::None; ++i) {
				int lv = player_upgrade_level(p, (UpgradeTypes)i);
				if (lv > 0) v.push_back({i, lv});
			}
			return v;
		}
		static const int max_ut = (int)UnitTypes::None;
		a_vector<int> count(max_ut, 0);
		for (unit_t* u : ptr(st.player_units[p])) {
			if (!u_completed(u)) continue;
			int id = (int)u->unit_type->id; if (id < 0 || id >= max_ut) continue;
			if (tab == 2) { if (ut_building(u)) ++count[id]; }
			else if (!ut_building(u) && !ut_worker(u)) ++count[id];
		}
		for (int i = 0; i < max_ut; ++i) if (count[i] > 0) v.push_back({i, count[i]});
		std::stable_sort(v.begin(), v.end(), [](const std::pair<int,int>& a, const std::pair<int,int>& b){ return a.second > b.second; });
		return v;
	}
	// Cell k of the composition grid (empty rect = it does not fit the panel).
	rect replay_flyout_cell(const replay_layout_t& lay, int k) {
		const rect& f = lay.flyout;
		const int pad = lay.rp(8), cell = lay.rp(54), gap = lay.rp(6);
		int cols = (f.to.x - f.from.x - 2 * pad + gap) / (cell + gap); if (cols < 1) cols = 1;
		rect t0 = replay_tab_rect(lay, 0);
		int x = f.from.x + pad + (k % cols) * (cell + gap), y = t0.to.y + lay.rp(10) + (k / cols) * (cell + gap);
		if (y + cell > f.to.y - pad) return {};
		return {{x, y}, {x + cell, y + cell}};
	}
	int replay_cache_index(int p) const {
		for (int i = 0; i < (int)observer_cache.size(); ++i) if (observer_cache[i].p == p) return i;
		return -1;
	}

	// --- Economy tab: income, army value and workers over the whole replay --------
	// One line per player in the player's colour (the panel's player drawn thicker),
	// a playhead at the current frame, the values under the playhead in the title row.
	static const int replay_chart_count = 3;
	// Chart k: `whole` = title row + plot, `plot` = the plot alone. Stacked when the
	// panel is tall enough (tablet), side by side otherwise (phone).
	bool replay_chart_rects(const replay_layout_t& lay, int k, rect& whole, rect& plot) {
		const rect& f = lay.flyout;
		const int pad = lay.rp(8), head = lay.rp(20), gap = lay.rp(8);
		rect t0 = replay_tab_rect(lay, 0);
		const int top = t0.to.y + lay.rp(8), bot = f.to.y - pad;
		int h = (bot - top - (replay_chart_count - 1) * gap) / replay_chart_count;
		if (h >= head + lay.rp(44)) {
			if (h > lay.rp(150)) h = lay.rp(150);
			const int y = top + k * (h + gap);
			whole = {{f.from.x + pad, y}, {f.to.x - pad, y + h}};
		} else {
			const int w = (f.to.x - f.from.x - 2 * pad - (replay_chart_count - 1) * gap) / replay_chart_count;
			if (bot - top < head + lay.rp(24) || w < lay.rp(60)) return false;
			const int x = f.from.x + pad + k * (w + gap);
			whole = {{x, top}, {x + w, bot}};
		}
		plot = {{whole.from.x, whole.from.y + head}, whole.to};
		return true;
	}
	// metric: 0 income per minute (over the last minute), 1 army value, 2 workers
	static int replay_chart_value(const replay_scan_data& d, int metric, int i, int p) {
		const replay_scan_sample& r = d.samples[(size_t)i][(size_t)p];
		if (metric == 1) return r.army;
		if (metric == 2) return r.workers;
		const int w = i < 60 ? i : 60;
		if (w <= 0) return 0;
		const int64_t dg = (int64_t)r.gathered - d.samples[(size_t)(i - w)][(size_t)p].gathered;
		return (int)(dg * 60000 / ((int64_t)w * d.step * 42));
	}
	static const char* replay_chart_title(int k) { return k == 0 ? "INCOME / MIN" : k == 1 ? "ARMY VALUE" : "WORKERS"; }
	void replay_draw_charts(loading_screen::canvas& c, const replay_layout_t& lay, int sel_idx) {
		namespace ls = loading_screen;
		const replay_scan_data& d = replay_scan_view;
		const int u = std::max(1, c.h / 400);
		const int n = (int)d.samples.size(), end = (int)replay_st.end_frame;
		for (int k = 0; k < replay_chart_count; ++k) {
			rect whole, pl;
			if (!replay_chart_rects(lay, k, whole, pl)) return;
			const float th = 11.0f * lay.s;
			ls::draw_text(c, (float)whole.from.x, (float)whole.from.y + (lay.rp(20) - th) * 0.5f, ls::upper(L(replay_chart_title(k))), ls::pal::text2, th, 0.06f);
			c.fill(pl.from.x, pl.from.y, pl.to.x, pl.to.y, 0x0d1317, 255);
			c.frame(pl.from.x, pl.from.y, pl.to.x, pl.to.y, u, ls::pal::rim, 255);
			if (n < 2 || end <= 0) continue;
			int vmax = 1;
			for (int i = 0; i < n; ++i) for (auto& row : observer_cache) {
				int v = replay_chart_value(d, k, i, row.p);
				if (v > vmax) vmax = v;
			}
			// the scale: the top of the plot is vmax
			char mb[16]; snprintf(mb, sizeof mb, "%d", vmax);
			const float mh = 9.5f * lay.s;
			ls::draw_text(c, (float)(pl.from.x + lay.rp(4)), (float)(pl.from.y + lay.rp(3)), mb, ls::pal::text2, mh);
			const int x0 = pl.from.x + u, x1 = pl.to.x - u, y0 = pl.from.y + u + lay.rp(3), y1 = pl.to.y - 2 * u;
			const int w = x1 - x0, h = y1 - y0;
			if (w < 8 || h < 8) continue;
			// minute grid
			{
				const double per_min = 60.0 * 1000.0 / 42.0;
				int stepm = 5;
				while (end / (per_min * stepm) > 8) stepm *= 2;
				for (int m = 1; m * per_min * stepm < end; ++m) {
					const int gx = x0 + (int)((double)w * (m * per_min * stepm) / end);
					c.fill(gx, y0, gx + u, y1, ls::pal::steel, 60);
				}
			}
			for (int pass = 0; pass < 2; ++pass) for (int ci = 0; ci < (int)observer_cache.size(); ++ci) {
				const bool sel = ci == sel_idx;
				if (sel != (pass == 1)) continue;         // the panel's player on top
				const int p = observer_cache[(size_t)ci].p;
				const uint32_t col = ci < (int)replay_players.size() ? rp_legible(replay_players[(size_t)ci].color) : ls::pal::text;
				const int t = sel ? 2 * u : u;
				int py = -1;
				for (int x = 0; x < w; ++x) {
					const int i = (int)((int64_t)x * end / w) / d.step;
					if (i >= n) break;
					int y = y1 - t - (int)((int64_t)replay_chart_value(d, k, i, p) * (h - t) / vmax);
					if (y < y0) y = y0;
					const int ya = py < 0 ? y : std::min(y, py), yb = py < 0 ? y : std::max(y, py);
					c.fill(x0 + x, ya, x0 + x + 1, yb + t, col, sel ? 255 : 215);
					py = y;
				}
			}
		}
	}
	// Per frame: the playhead and the values under it (every player, in its colour).
	void replay_draw_charts_live(loading_screen::canvas& c, const replay_layout_t& lay) {
		namespace ls = loading_screen;
		const replay_scan_data& d = replay_scan_view;
		const int u = std::max(1, c.h / 400);
		const int n = (int)d.samples.size(), end = (int)replay_st.end_frame;
		if (end <= 0) return;
		const int i = (int)st.current_frame / d.step;
		for (int k = 0; k < replay_chart_count; ++k) {
			rect whole, pl;
			if (!replay_chart_rects(lay, k, whole, pl)) return;
			const int w = pl.to.x - pl.from.x - 2 * u;
			const int x = pl.from.x + u + (int)((int64_t)st.current_frame * w / end);
			c.fill(x, pl.from.y + u, x + u, pl.to.y - u, ls::pal::accent, 220);
			if (i >= n || n == 0) continue;
			const float th = 12.0f * lay.s;
			float tx = (float)whole.to.x;
			const float left = (float)whole.from.x + ls::text_width(ls::upper(L(replay_chart_title(k))), 11.0f * lay.s, 0.06f) + th;
			for (int ci = (int)observer_cache.size() - 1; ci >= 0; --ci) {
				char b[16]; snprintf(b, sizeof b, "%d", replay_chart_value(d, k, i, observer_cache[(size_t)ci].p));
				const float tw = ls::text_width(b, th);
				if (tx - tw < left) break;
				tx -= tw;
				ls::draw_text(c, tx, (float)whole.from.y + (lay.rp(20) - th) * 0.5f, b, ci < (int)replay_players.size() ? rp_legible(replay_players[(size_t)ci].color) : ls::pal::text, th);
				tx -= th * 0.9f;
			}
		}
	}
	void draw_replay_panel(uint32_t* data, size_t pitch, const replay_layout_t& lay) {
		namespace ls = loading_screen;
		if (!replay_panel_visible(lay) || rp_empty(lay.panel)) return;
		const int sh = (int)screen_height;
		const int u = std::max(1, sh / 400);
		const int idx = replay_cache_index(observer_flyout_player);
		if (!lay.compact && idx < 0) return;            // tablet: the panel belongs to a player
		const int p = idx >= 0 ? observer_flyout_player : -1;
		a_vector<std::pair<int,int>> items = replay_flyout_items(p, observer_flyout_tab);
		replay_card_metrics_t m;
		if (lay.compact) m = replay_card_metrics(lay);
		// static part (cached): body, header, tabs, the empty cells
		rp_hash h;
		h.r(lay.panel); h.r(lay.flyout); h.i(lay.compact); h.i(p); h.i(observer_flyout_tab); h.i((int64_t)items.size()); h.i(lang_lt); h.i(lay.n);
		const bool economy = observer_flyout_tab == replay_tab_economy;
		if (economy) h.i(replay_scan_seen);
		if (idx >= 0 && idx < (int)replay_players.size()) { auto& pl = replay_players[(size_t)idx]; h.s(pl.name); h.i(pl.race); h.i(pl.color); h.i(pl.portrait_id); }
		if (lay.compact) for (auto& pl : replay_players) { h.s(pl.name); h.i(pl.race); h.i(pl.color); h.i(pl.portrait_id); }
		if (replay_hud.panel.sig != h.get()) {
			ls::canvas c = rp_layer(replay_hud.panel, lay.panel);
			const rect& pr = lay.panel;
			c.fill(pr.from.x, pr.from.y, pr.to.x, pr.to.y, ls::pal::panel, 255);
			c.frame(pr.from.x, pr.from.y, pr.to.x, pr.to.y, u, ls::pal::line, 255);
			c.frame(pr.from.x + u, pr.from.y + u, pr.to.x - u, pr.to.y - u, u, 0x000000, 255);
			const rect& f = lay.flyout;
			const int pad = lay.rp(8);
			if (lay.compact) {
				const float lh = 13.0f * lay.s;
				ls::draw_text(c, (float)(pr.from.x + pad), (lay.close_btn.from.y + lay.close_btn.to.y) * 0.5f - lh * 0.5f, ls::upper(L("PLAYERS")), ls::pal::label, lh, 0.12f);
				for (int i = 0; i < lay.n; ++i) replay_card_static(c, lay, m, i);
			} else if (idx >= 0 && idx < (int)replay_players.size()) {
				// header: portrait, name, race
				const ls::player_t& pl = replay_players[(size_t)idx];
				const int hy = f.from.y + lay.rp(8), hh = lay.rp(48);
				int tx = f.from.x + pad, tw = 0;
				if (pl.portrait_tile) {
					ls::portrait_tile_t g = ls::portrait_tile_fit(pl, hh - 2 * u, u);
					ls::draw_portrait_tile(c, pl, g, tx, hy + (hh - g.th()) / 2, false);
					tw = g.tw();
				} else {
					c.fill(tx, hy + hh / 4, tx + lay.rp(8), hy + hh * 3 / 4, pl.color, 255);
					tw = lay.rp(8);
				}
				const float nh = 16.0f * lay.s, rh = 11.5f * lay.s;
				const float nx = (float)(tx + tw + lay.rp(10)), nmax = (float)(lay.close_btn.from.x - lay.rp(6)) - nx;
				const char* race = pl.race == 0 ? "Zerg" : pl.race == 1 ? "Terran" : pl.race == 2 ? "Protoss" : "";
				ls::draw_text(c, nx, hy + hh * 0.5f - (nh + rh + 3.0f * lay.s) * 0.5f, ls::ellipsize(pl.name, nh, nmax), ls::pal::title, nh);
				ls::draw_text(c, nx, hy + hh * 0.5f - (nh + rh + 3.0f * lay.s) * 0.5f + nh + 3.0f * lay.s, race, ls::pal::text2, rh);
			}
			// close button
			{
				const rect& cb = lay.close_btn;
				const float cx = (cb.from.x + cb.to.x) * 0.5f, cy = (cb.from.y + cb.to.y) * 0.5f;
				rp_cross(c, cx, cy, 6.0f * lay.s, std::max(1.0f, 1.1f * lay.s), ls::pal::text2);
			}
			// tabs
			static const char* tab_name[replay_tab_count] = {"ARMY", "UPGRADES", "BUILDINGS", "ECONOMY"};
			for (int i = 0; i < replay_tab_count; ++i) {
				rect t = replay_tab_rect(lay, i);
				const bool on = i == observer_flyout_tab;
				rp_button(c, t, on ? 1 : 0);
				rp_text_center(c, t, ls::upper(L(tab_name[i])), on ? ls::pal::title : ls::pal::text, 12.0f * lay.s, 0.06f);
			}
			if (economy) {
				replay_draw_charts(c, lay, idx);
			} else if (p < 0) {
				rect t0 = replay_tab_rect(lay, 0);
				ls::draw_text(c, (float)(f.from.x + pad), (float)(t0.to.y + lay.rp(14)), L("Tap a player"), ls::pal::text2, 12.0f * lay.s);
			} else if (items.empty()) {
				rect t0 = replay_tab_rect(lay, 0);
				ls::draw_text(c, (float)(f.from.x + pad), (float)(t0.to.y + lay.rp(14)), L("Nothing yet"), ls::pal::text2, 12.0f * lay.s);
			}
			for (int k = 0; k < (int)items.size(); ++k) {
				rect cell = replay_flyout_cell(lay, k); if (rp_empty(cell)) break;
				c.fill(cell.from.x, cell.from.y, cell.to.x, cell.to.y, 0x0d1317, 255);
				c.frame(cell.from.x, cell.from.y, cell.to.x, cell.to.y, u, ls::pal::rim, 255);
			}
			replay_hud.panel.sig = h.get();
		}
		rp_blit(data, pitch, replay_hud.panel, lay.panel.from.x, lay.panel.from.y, lay.compact ? 248 : 240);
		// per frame: icons, counts, and (phone) the cards' numbers
		ls::canvas sc = rp_screen(data, pitch);
		load_cmdicons();
		const float ch = 13.0f * lay.s;
		for (int k = 0; k < (int)items.size(); ++k) {
			rect cell = replay_flyout_cell(lay, k); if (rp_empty(cell)) break;
			if (cmdicons_ok) {
				int frame = observer_flyout_tab == 1 ? upgrade_icon((UpgradeTypes)items[k].first) : items[k].first;
				if (frame >= 0) draw_cmdicon_fit(data, pitch, frame, cell);
			}
			char nb[12]; snprintf(nb, sizeof nb, observer_flyout_tab == 1 ? "L%d" : "%d", items[k].second);
			float w = ls::text_width(nb, ch);
			ls::draw_text(sc, (float)(cell.to.x - lay.rp(3)) - w, (float)(cell.to.y - lay.rp(2)) - ch, nb, ls::pal::accent, ch);
		}
		if (economy) replay_draw_charts_live(sc, lay);
		if (lay.compact) {
			for (int i = 0; i < lay.n && i < (int)replay_hud.vals.size(); ++i) {
				const replay_vals_t& v = replay_hud.vals[(size_t)i];
				replay_card_dynamic(sc, lay, m, i, v, v.p == observer_flyout_player, observer_vision_active(v.p));
			}
		}
	}

	// --- small overlays on the game view ------------------------------------------
	// "VISION: <player>" while watching through a player's fog; on a phone also the
	// roster strip (top-left) and the PLAYERS / EXIT buttons (top-right).
	void draw_replay_overlays(uint32_t* data, size_t pitch, const replay_layout_t& lay) {
		namespace ls = loading_screen;
		ls::canvas sc = rp_screen(data, pitch);
		const int sw = (int)screen_width;
		const int u = std::max(1, (int)screen_height / 400);
		const int m = lay.rp(lay.compact ? 6.0f : 8.0f);
		if (lay.compact) {
			rp_button(sc, lay.btn[rbtn_exit], 0);
			rp_button_glyph(sc, lay, rbtn_exit, false);
			rp_button(sc, lay.players_btn, replay_players_open ? 1 : 0);
			rp_text_center(sc, lay.players_btn, ls::upper(L("PLAYERS")), replay_players_open ? ls::pal::title : ls::pal::text, 12.0f * lay.s, 0.06f);
			if (!replay_players_open && !rp_empty(lay.mini)) {
				// roster strip: colour, name, minerals, gas, supply
				const float th = 12.5f * lay.s;
				rp_dim(data, pitch, lay.mini, ls::pal::panel, 200);
				for (int i = 0; i < lay.n && i < (int)replay_hud.vals.size() && i < (int)replay_players.size(); ++i) {
					const replay_vals_t& v = replay_hud.vals[(size_t)i];
					const ls::player_t& pl = replay_players[(size_t)i];
					const int y0 = lay.mini.from.y + i * lay.mini_row_h;
					const float ty = y0 + (lay.mini_row_h - th) * 0.5f;
					sc.fill(lay.mini.from.x, y0 + u, lay.mini.from.x + lay.rp(3), y0 + lay.mini_row_h - u, pl.color, 255);
					float x = (float)(lay.mini.from.x + lay.rp(8));
					const float name_w = ls::text_advance(th) * 9;
					ls::draw_text(sc, x, ty, ls::ellipsize(pl.name, th, name_w), ls::pal::title, th);
					x += name_w + th * 0.6f;
					char b[24];
					snprintf(b, sizeof b, "%d", v.minerals); ls::draw_text(sc, x, ty, b, 0x30d8f0, th); x += ls::text_advance(th) * 5.6f;
					snprintf(b, sizeof b, "%d", v.gas); ls::draw_text(sc, x, ty, b, 0x5fe07a, th); x += ls::text_advance(th) * 5.6f;
					snprintf(b, sizeof b, "%d/%d", v.su, v.sm); ls::draw_text(sc, x, ty, b, v.su >= v.sm ? ls::pal::danger : ls::pal::title, th);
				}
			}
		}
		if (replay_vision_player >= 0 && replay_vision_player < 8 && !(lay.compact && replay_players_open)) {
			const float th = 12.5f * lay.s;
			std::string s = std::string(L("VISION")) + ": " + loading_screen::ascii_text(observer_name(replay_vision_player), "?");
			const int w = (int)std::ceil(ls::text_width(s, th)) + lay.rp(16), hgt = lay.rp(26);
			const int y = lay.compact ? lay.btn[rbtn_exit].to.y + lay.rp(5) : m;
			rect chip{{sw - m - w, y}, {sw - m, y + hgt}};
			rp_dim(data, pitch, chip, ls::pal::panel, 224);
			sc.frame(chip.from.x, chip.from.y, chip.to.x, chip.to.y, u, ls::pal::label, 255);
			ls::draw_text(sc, (float)(chip.from.x + lay.rp(8)), chip.from.y + (hgt - th) * 0.5f, s, ls::pal::label, th);
		}
	}

	uint64_t replay_hud_instant = 0;         // what the last sample of the numbers was taken for

	// Entry point (RGBA pass, after the fog): the whole replay HUD. No-op in a game.
	void draw_replay_hud(uint32_t* data, size_t pitch) {
		if (game_input || replay_st.end_frame == 0) {
			if (!replay_hud.chrome.px.empty() || !replay_hud.panel.px.empty()) replay_hud.release();
			return;
		}
		replay_scan_start();                 // (no-op once started or past frame 0)
		replay_scan_refresh();
		observer_rebuild_cache();
		replay_layout_t lay = replay_layout();
		if (!lay.ok) return;
		if (!replay_panel_visible(lay) && !replay_hud.panel.px.empty()) replay_hud.panel.release();
		draw_replay_bar(data, pitch, lay);
		draw_replay_production(data, pitch, lay);
		draw_replay_panel(data, pitch, lay);
		draw_replay_overlays(data, pitch, lay);
	}

	virtual void draw_callback(uint8_t* data, size_t data_pitch) {
	}

	a_vector<const image_t*> image_draw_queue;

	bool use_new_images = false;

	bool new_images_index_loaded = false;

	a_vector<char> grp_new_image_state = a_vector<char>((size_t)ImageTypes::None);
	a_unordered_set<a_string> new_images_index;
	string_table_data images_tbl;

	a_vector<a_vector<std::unique_ptr<native_window_drawing::surface>>> new_images;
	a_vector<a_vector<std::unique_ptr<native_window_drawing::surface>>> new_images_flipped;

	bool is_new_image(const image_t* image) {
		if (!use_new_images) return false;
		if (!new_images_index_loaded) {
			new_images_index_loaded = true;
			async_read_file("index.txt", [this](const uint8_t* data, size_t len) {
				if (!data) {
					ui::log("failed to load index.txt :(\n");
					return;
				}
				char* c = (char*)data;
				char* e = (char*)data + len;
				while (c != e && (*c == '\r' || *c =='\n' || *c == ' ')) ++c;
				while (c != e) {
					char* s = c;
					while (c != e && *c != '\r' && *c !='\n' && *c != ' ') ++c;
					a_string fn(s, c - s);
					while (c != e && (*c == '\r' || *c =='\n' || *c == ' ')) ++c;
					for (char& c : fn) {
						if (c == '\\') c = '/';
					}
					if (!fn.empty() && fn.front() == '/') fn.erase(fn.begin());
					if (!fn.empty() && fn.back() == '/') fn.erase(std::prev(fn.end()));
					ui::log("index entry '%s'\n", fn);
					new_images_index.insert(std::move(fn));
				}
			});
		}
		if (new_images_index.empty()) return false;
		size_t index = image->grp - global_st.grps.data();
		auto& state = grp_new_image_state.at(index);
		if (state == 0) {
			state = 2;
			a_string fn = images_tbl.at(image->image_type->grp_filename_index);
			for (char& c : fn) {
				if (c == '\\') c = '/';
			}
			for (auto i = fn.rbegin(); i != fn.rend(); ++i) {
				if (*i == '.') {
					fn.erase(std::prev(i.base()), fn.end());
				}
				if (*i == '/') break;
			}
			if (!fn.empty() && fn.front() == '/') fn.erase(fn.begin());
			fn = "unit/" + fn;
			ui::log("checking '%s' (image %d, grp %u)\n", fn, (int)image->image_type->id, index);
			if (new_images_index.count(fn)) {
				size_t frames = image->grp->frames.size();
				if (new_images.size() <= index) new_images.resize(index + 1);
				new_images[index].resize(frames);
				if (new_images_flipped.size() <= index) new_images_flipped.resize(index + 1);
				new_images_flipped[index].resize(frames);
				ui::log("loading %d frames...\n", frames);
				auto frames_left = std::make_shared<size_t>(frames);
				for (size_t i = 0; i != frames; ++i) {
					a_string frame_fn = format("%s/%02u.png", fn, i);
					async_read_file(frame_fn, [this, frame_fn, index, i, frames_left](const uint8_t* data, size_t len) {
						if (!data) {
							ui::log("failed to load '%s'\n", frame_fn);
							return;
						}
						new_images.at(index).at(i) = native_window_drawing::load_image(data, len);
						--*frames_left;
						if (*frames_left == 0) {
							grp_new_image_state.at(index) = 1;

							ui::log("grp %d successfully loaded %d frames\n", index, new_images.at(index).size());
						}
					});
				}
			}
		}
		//ui::log("index %d state %d\n", index, state);
		return state == 1;
	}

	native_window_drawing::surface* get_new_image_surface(const image_t* image, bool flipped) {
		size_t index = image->grp - global_st.grps.data();
		size_t frame = image->frame_index;
		if (flipped) {
			auto& r = new_images_flipped.at(index).at(frame);
			if (!r) {
				auto* s = new_images.at(index).at(frame).get();
				if (!s) return nullptr;
				r = flip_image(s);
			}
			return r.get();
		} else {
			return new_images.at(index).at(frame).get();
		}
	}

	std::unique_ptr<native_window_drawing::surface> tmp_surface;

	void draw_new_image(const image_t* image) {
		xy map_pos = get_image_center_map_position(image);

		int screen_x = map_pos.x - screen_pos.x;
		int screen_y = map_pos.y - screen_pos.y;

		auto* surface = get_new_image_surface(image, i_flag(image, image_t::flag_horizontally_flipped));
		if (!surface) {
			ui::log("ERROR: new image %d (grp %d) frame %d does not exist\n", (int)image->image_type->id, image->grp - global_st.grps.data(), image->frame_index);
			return;
		}

		auto scale = 114_fp8;

		size_t w = (fp8::integer(surface->w) * scale).integer_part();
		size_t h = (fp8::integer(surface->w) * scale).integer_part();

		size_t orig_w = w;
		size_t orig_h = h;

		screen_x -= w / 2;
		screen_y -= w / 2;

		if (screen_x >= (int)screen_width || screen_y >= (int)screen_height) return;
		if (screen_x + (int)w <= 0 || screen_y + (int)h <= 0) return;

		size_t offset_x = 0;
		size_t offset_y = 0;
		if (screen_x < 0) {
			offset_x = -screen_x;
			w += screen_x;
			screen_x = 0;
		}
		if (screen_y < 0) {
			offset_y = -screen_y;
			h += screen_y;
			screen_y = 0;
		}

		w = std::min(w, screen_width - screen_x);
		h = std::min(h, screen_height - screen_y);

		if (image->modifier == 10) {
			if (!tmp_surface || (size_t)tmp_surface->w < orig_w || (size_t)tmp_surface->h < orig_h) {
				tmp_surface = native_window_drawing::create_rgba_surface(orig_w, orig_h);
			}
			surface->set_blend_mode(native_window_drawing::blend_mode::none);
			surface->blit_scaled(&*tmp_surface, 0, 0, orig_w, orig_h);

			size_t src_pitch = tmp_surface->pitch / 4;
			size_t dst_pitch = rgba_surface->pitch / 4;
			uint32_t* src = (uint32_t*)tmp_surface->lock();
			uint32_t* dst = (uint32_t*)rgba_surface->lock();

			src += src_pitch * offset_y + offset_x;
			dst += dst_pitch * screen_y + screen_x;

			size_t src_skip = src_pitch - w;
			size_t dst_skip = dst_pitch - w;

			for (size_t y = h; y--;) {

				for (size_t x = w; x--;) {
					uint32_t s = *src;
					uint32_t d = *dst;
					if (s >> 24 >= 16) *dst = (d & 0xfefefe) / 2 | 0xff000000;
					++src;
					++dst;
				}

				src += src_skip;
				dst += dst_skip;
			}

			tmp_surface->unlock();
			rgba_surface->unlock();

		} else {
			surface->set_blend_mode(native_window_drawing::blend_mode::alpha);
			surface->blit_scaled(&*rgba_surface, screen_x - offset_x, screen_y - offset_y, orig_w, orig_h);
		}
	}

	void draw_image_queue() {
		for (auto* image : image_draw_queue) {
			draw_new_image(image);
		}
	}

	fp8 game_speed = fp8::integer(1);

	std::unique_ptr<native_window_drawing::surface> indexed_surface;
	std::unique_ptr<native_window_drawing::surface> rgba_surface;
	native_window_drawing::palette* palette = nullptr;
	std::chrono::high_resolution_clock clock;
	std::chrono::high_resolution_clock::time_point last_draw;
	std::chrono::high_resolution_clock::time_point last_input_poll;
	std::chrono::high_resolution_clock::time_point last_fps;
	int fps_counter = 0;
	int fps_display = 0;
	size_t scroll_speed_n = 0;

	// Faze 4.5: FULL HD (world_scale>1) auto-fallback. If FULL HD can't sustain a
	// playable frame rate on this device (e.g. an S7+), fire perf_downgrade_cb
	// ONCE after perf_low_secs_needed consecutive seconds of actual gameplay below
	// perf_fps_threshold. The callback persists a marker the launcher reads to
	// force HD2 on the next launch (no risky mid-game buffer surgery). Disabled
	// (threshold 0) unless android_main enables it for the FULL HD tier.
	int perf_fps_threshold = 0;      // <this sustained => too slow (0 = off)
	int perf_low_secs_needed = 15;   // consecutive low seconds required
	int perf_low_secs = 0;
	int perf_warmup_secs = 6;        // ignore the first seconds (map load hitches)
	bool perf_downgrade_fired = false;
	std::function<void()> perf_downgrade_cb;

	void resize(int width, int height) {
		if (!wnd && create_window) wnd.create("OpenBW", 0, 0, width, height);
		screen_width = width;
		screen_height = height;
		//view_scale = fp16::integer(1) - (fp16::integer(1) / 4);
		int ws = world_scale < 1 ? 1 : world_scale;
		view_width = screen_width / ws;   // the map area shown (constant vs. buffer)
		view_height = screen_height / ws;
		view_scale = (ufp16::integer(screen_width) / view_width).as_signed(); // == ws
		indexed_surface.reset();
		rgba_surface.reset();
	}

	// --- Pinch zoom (v0.9) ----------------------------------------------
	// Zoom is done by changing the render resolution (screen_width/height): a
	// smaller render buffer is upscaled more by SDL, so the world (32px/tile)
	// appears larger = zoomed in. The console-space HUD re-fits automatically.
	// Discrete levels (0 = most zoomed out .. 4 = most zoomed in); level 2 is
	// the default (the settings' render scale, captured on first zoom).
	int zoom_level = 2;
	int base_render_w = 0, base_render_h = 0;

	void set_zoom(int new_level) {
		if (new_level < 0) new_level = 0;
		if (new_level > 4) new_level = 4;
		if (base_render_w == 0) { base_render_w = (int)screen_width; base_render_h = (int)screen_height; }
		if (new_level == zoom_level && base_render_w == (int)screen_width) return;
		xy center = screen_pos + xy((int)view_width / 2, (int)view_height / 2);
		zoom_level = new_level;
		static const float f[5] = {1.5f, 1.2f, 1.0f, 0.78f, 0.6f};
		int nw = (int)(base_render_w * f[new_level]);
		int nh = (int)(base_render_h * f[new_level]);
		resize(nw, nh);
		screen_pos = center - xy((int)view_width / 2, (int)view_height / 2);
		int maxx = (int)game_st.map_width - (int)view_width;
		int maxy = (int)game_st.map_height - playfield_view_height();
		if (maxx < 0) maxx = 0;
		if (maxy < 0) maxy = 0;
		if (screen_pos.x < 0) screen_pos.x = 0;
		if (screen_pos.y < 0) screen_pos.y = 0;
		if (screen_pos.x > maxx) screen_pos.x = maxx;
		if (screen_pos.y > maxy) screen_pos.y = maxy;
	}

	// Center the playfield camera on a map position, clamped to the map.
	void center_camera_on(xy pos) {
		screen_pos = pos - xy((int)view_width / 2, playfield_view_height() / 2);
		int maxx = (int)game_st.map_width - (int)view_width;
		int maxy = (int)game_st.map_height - playfield_view_height();
		if (maxx < 0) maxx = 0;
		if (maxy < 0) maxy = 0;
		if (screen_pos.x < 0) screen_pos.x = 0;
		if (screen_pos.y < 0) screen_pos.y = 0;
		if (screen_pos.x > maxx) screen_pos.x = maxx;
		if (screen_pos.y > maxy) screen_pos.y = maxy;
	}

	// --- Under-attack alert -------------------------------------------
	// No engine on-damage hook exists, so poll own units' HP each frame; a
	// drop flags an attack. Reuses no state from the manual ping.
	a_unordered_map<uint32_t, int> alert_hp;
	int last_alert_frame = -100000;
	xy alert_pos = xy(-1, -1);
	int alert_frame = -100000;

	void poll_under_attack_alert() {
		if (!game_input || game_local_player < 0) return;
		int now = (int)st.current_frame;
		bool dmg = false, dmg_building = false; xy pos;
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player) continue;
			if (!u_completed(u)) continue;
			uint32_t id = get_unit_id(u).raw_value;
			int hp = (int)u->hp.integer_part();
			auto it = alert_hp.find(id);
			// Ignore HP loss from a friendly self-transform (Archon/Dark Archon
			// merge consumes the two templar, a morph/birth interpolates HP, an
			// infest/consume/death removes the unit) - none of these is an attack.
			// This is why merging templar used to raise a false "under attack".
			bool self_transform = false;
			if (u->order_type) {
				switch (u->order_type->id) {
				case Orders::ArchonWarp: case Orders::DarkArchonMeld:
				case Orders::ZergUnitMorph: case Orders::ZergBuildingMorph:
				case Orders::ZergBirth: case Orders::InfestingCommandCenter:
				case Orders::Die:
					self_transform = true; break;
				default: break;
				}
			}
			if (!self_transform && it != alert_hp.end() && hp < it->second) {
				dmg = true; pos = u->position;
				if (u->unit_type && (u->unit_type->group_flags & GroupFlags::Building)) dmg_building = true;
			}
			alert_hp[id] = hp;
		}
		if (dmg && now - last_alert_frame > 24 * 8) { // ~8s cooldown
			last_alert_frame = now;
			alert_pos = pos;
			alert_frame = now;
			show_hud_message(dmg_building ? "Your base is under attack."
			                              : "Your forces are under attack.");
			play_ui_wav(advisor_update_wav(dmg_building ? ADV_UPD_BASE_ATTACK
			                                            : ADV_UPD_FORCES_ATTACK).c_str());
		}
	}

	// --- Nuclear launch alert (v0.83.001) -----------------------------
	// v0.83.000 polled every visible unit each frame looking for a missile in
	// its NukeLaunch order. That was the wrong shape: the engine knows the exact
	// moment and already marked it ("todo: callback for message"), so the poll
	// both duplicated it and could fire a frame or two off from the sound.
	//
	// Split of responsibility, by design: the SOUND is unconditional (retail
	// behaviour -- a launch is heard by everyone), the TEXT only appears when
	// the target is in the local player's vision, so the notice cannot leak an
	// enemy position through the fog.
	virtual void on_nuke_launch(xy target) override {
		if (!game_input) return;
		int vis = fog_vision_owner();
		if (vis >= 0 && !player_position_is_visible(vis, target)) return;
		alert_pos = target;
		alert_frame = (int)st.current_frame;
		show_hud_message("Nuclear launch detected.");
	}

	// --- Research / upgrade complete notice (v0.83) --------------------
	// Same shape as the two alerts above: no engine hook, so snapshot the local
	// player's researched-tech and upgrade-level tables and report transitions.
	// Reading the RESULT rather than watching the building means it fires
	// exactly once and cannot be fooled by the lab being cancelled, destroyed
	// or handed over mid-research.
	bool tech_snapshot_taken = false;
	std::array<bool, 44> seen_tech{};
	std::array<uint8_t, 61> seen_upgrade{};

	void poll_research_complete() {
		if (!game_input || game_local_player < 0) return;
		int p = game_local_player;
		if (!tech_snapshot_taken) {
			// Prime from the current state, so anything a map or a cheat
			// granted before we started looking is not announced.
			tech_snapshot_taken = true;
			for (int i = 0; i != 44; ++i) seen_tech[(size_t)i] = st.tech_researched[p][(TechTypes)i];
			for (int i = 0; i != 61; ++i)
				seen_upgrade[(size_t)i] = (uint8_t)player_upgrade_level(p, (UpgradeTypes)i);
			return;
		}
		char buf[96];
		for (int i = 0; i != 44; ++i) {
			bool now = st.tech_researched[p][(TechTypes)i];
			if (now && !seen_tech[(size_t)i]) {
				const char* n = tech_official_name((TechTypes)i);
				snprintf(buf, sizeof(buf), "%s research complete.", n ? n : "Technology");
				show_hud_message(buf);
			}
			seen_tech[(size_t)i] = now;
		}
		for (int i = 0; i != 61; ++i) {
			uint8_t now = (uint8_t)player_upgrade_level(p, (UpgradeTypes)i);
			if (now > seen_upgrade[(size_t)i]) {
				const char* n = upgrade_official_name((UpgradeTypes)i);
				int maxlv = player_max_upgrade_level(p, (UpgradeTypes)i);
				if (maxlv > 1)
					snprintf(buf, sizeof(buf), "%s level %d complete.", n ? n : "Upgrade", (int)now);
				else
					snprintf(buf, sizeof(buf), "%s complete.", n ? n : "Upgrade");
				show_hud_message(buf);
			}
			seen_upgrade[(size_t)i] = now;
		}
	}

	// v0.83 (#111): show the local player the "couldn't place / couldn't
	// create" error the sim just recorded. Only 60/61/62 ever reach
	// display_last_error_for_player, all placement/creation failures; they are
	// engine sentinels out of the stat_txt error family (843..902, measured),
	// so they map to their real strings here rather than through 843+code.
	int last_error_shown_frame = -1;
	void poll_last_error() {
		if (!game_input || game_local_player < 0) return;
		if (st.last_error_player != game_local_player) return;
		if (st.last_error_frame < 0 || st.last_error_frame == last_error_shown_frame) return;
		if ((int)st.current_frame - st.last_error_frame > 3) return; // fresh only
		last_error_shown_frame = st.last_error_frame;
		const char* msg;
		switch (st.last_error) {
		case 60: msg = "Not enough room for unit."; break;   // stat_txt 891
		case 61: case 62: msg = "Cannot create more units."; break; // engine cap
		default: return;                                      // not a known code
		}
		show_hud_message(L(msg));
	}

	// ---- Field marks (v0.125) ---------------------
	// Our own marks on the field and the minimap -- placement box, pending builds,
	// drag box, rally, order queue, nuke, under-attack, ping -- used to be 1 px
	// lines in buffer pixels: thin on a tablet, half as thin again at the FULL HD
	// tier, and lost on bright terrain. Now one stroke for all of them: 2 px with a
	// 1 px black edge, both through hud_h_px(); "not there yet" is dashed. Colours
	// and meanings are unchanged. Selection circles and health bars are game art
	// and are not touched.
	int mark_t() { int t = hud_h_px(2); return t < 1 ? 1 : t; }
	int mark_e() { int e = hud_h_px(1); return e < 1 ? 1 : e; }
	void mark_box(uint32_t* data, size_t pitch, int x0, int y0, int x1, int y1, uint32_t c, int clip_y) {
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > (int)screen_width) x1 = (int)screen_width;
		if (y1 > clip_y) y1 = clip_y;
		for (int y = y0; y < y1; ++y) {
			uint32_t* row = data + (size_t)y * pitch;
			for (int x = x0; x < x1; ++x) row[x] = c;
		}
	}
	// One pass of a stroke: pass 0 = the black edge, pass 1 = the colour. Shapes
	// made of several strokes run pass 0 for all of them first, so an edge never
	// cuts through a neighbouring stroke.
	void mark_line_pass(uint32_t* data, size_t pitch, int x0, int y0, int x1, int y1, uint32_t color, int pass, bool dashed, int clip_y) {
		const int t = mark_t(), e = mark_e();
		const int size = pass ? t : t + 2 * e;
		const int off = t / 2 + (pass ? 0 : e);
		const uint32_t c = pass ? (0xff000000u | (color & 0xffffffu)) : 0xff000000u;
		int on = hud_h_px(8), gap = hud_h_px(6);
		if (on < 2) on = 2;
		if (gap < 2) gap = 2;
		const int period = on + gap;
		int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
		int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (int step = 0; step < 16384; ++step) {
			if (!dashed || step % period < on) mark_box(data, pitch, x0 - off, y0 - off, x0 - off + size, y0 - off + size, c, clip_y);
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	}
	void draw_line2(uint32_t* data, size_t pitch, int x0, int y0, int x1, int y1, uint32_t color, bool dashed = false, int clip_y = -1) {
		if (clip_y < 0) clip_y = (int)screen_height;
		mark_line_pass(data, pitch, x0, y0, x1, y1, color, 0, dashed, clip_y);
		mark_line_pass(data, pitch, x0, y0, x1, y1, color, 1, dashed, clip_y);
	}
	// Outline whose stroke lies inside r.
	void mark_rect(uint32_t* data, size_t pitch, rect r, uint32_t color, bool dashed = false, int clip_y = -1) {
		if (clip_y < 0) clip_y = (int)screen_height;
		const int t = mark_t();
		const int x0 = r.from.x + t / 2, y0 = r.from.y + t / 2;
		const int x1 = r.to.x - t + t / 2, y1 = r.to.y - t + t / 2;
		if (x1 < x0 || y1 < y0) return;
		for (int pass = 0; pass != 2; ++pass) {
			mark_line_pass(data, pitch, x0, y0, x1, y0, color, pass, dashed, clip_y);
			mark_line_pass(data, pitch, x0, y1, x1, y1, color, pass, dashed, clip_y);
			mark_line_pass(data, pitch, x0, y0, x0, y1, color, pass, dashed, clip_y);
			mark_line_pass(data, pitch, x1, y0, x1, y1, color, pass, dashed, clip_y);
		}
	}
	// Diamond of half-size r: black edge, coloured outline, dark inside.
	void mark_diamond(uint32_t* data, size_t pitch, int cx, int cy, int r, uint32_t line, uint32_t fill) {
		const int t = mark_t(), e = mark_e();
		for (int d = -(r + e); d <= r + e; ++d) {
			const int w = r + e - std::abs(d);
			mark_box(data, pitch, cx - w, cy + d, cx + w + 1, cy + d + 1, 0xff000000u, (int)screen_height);
		}
		for (int d = -r; d <= r; ++d) {
			const int w = r - std::abs(d);
			mark_box(data, pitch, cx - w, cy + d, cx + w + 1, cy + d + 1, 0xff000000u | (line & 0xffffffu), (int)screen_height);
			const int wi = w - t;
			if (wi >= 0 && std::abs(d) <= r - t) mark_box(data, pitch, cx - wi, cy + d, cx + wi + 1, cy + d + 1, 0xff000000u | (fill & 0xffffffu), (int)screen_height);
		}
	}
	// A small flag standing on (sx, sy): the rally point.
	void mark_flag(uint32_t* data, size_t pitch, int sx, int sy, uint32_t color) {
		const int t = mark_t(), e = mark_e();
		const int ph = hud_h_px(16), fh = hud_h_px(8);   // pole height, flag height
		const int px = sx - t / 2;
		for (int pass = 0; pass != 2; ++pass) {
			const int g = pass ? 0 : e;
			const uint32_t c = pass ? (0xff000000u | (color & 0xffffffu)) : 0xff000000u;
			mark_box(data, pitch, px - g, sy - ph - g, px + t + g, sy + g, c, (int)screen_height);
			for (int y = 0; y < fh; ++y) {
				const int w = 2 * (y + 1 < fh - y ? y + 1 : fh - y);      // triangle pointing right
				mark_box(data, pitch, px + t - g, sy - ph + y - g, px + t + w + g, sy - ph + y + 1 + g, c, (int)screen_height);
			}
		}
	}

	// Flash a red frame INSIDE the minimap (it used to be drawn around it, over the
	// panel frame) and drop a red blip at the attacked spot for a couple of
	// seconds after an alert fires.
	void draw_under_attack_alert(uint32_t* data, size_t pitch) {
		if (!game_input || alert_frame <= 0) return;
		int age = (int)st.current_frame - alert_frame;
		if (age < 0 || age > 24 * 3) return; // show ~3s
		rect mm = get_minimap_area();
		if (mm.to.x <= mm.from.x) return;
		const int e = mark_e();
		if ((age / 6) % 2 == 0) { // blink
			uint32_t red = 0xffff3030u;
			for (int t = 0; t < 3 * e; ++t) line_rectangle_rgba(data, pitch, {{mm.from.x + t, mm.from.y + t}, {mm.to.x - t, mm.to.y - t}}, red);
		}
		if (alert_pos.x >= 0 && game_st.map_width > 0) {
			int bx = mm.from.x + (int)((int64_t)alert_pos.x * (mm.to.x - mm.from.x) / game_st.map_width);
			int by = mm.from.y + (int)((int64_t)alert_pos.y * (mm.to.y - mm.from.y) / game_st.map_height);
			mark_box(data, pitch, bx - 3 * e, by - 3 * e, bx + 4 * e, by + 4 * e, 0xff000000u, (int)screen_height);
			mark_box(data, pitch, bx - 2 * e, by - 2 * e, bx + 3 * e, by + 3 * e, 0xffff2020u, (int)screen_height);
		}
	}

	// --- Trigger HUD surfaces (v0.20): text messages, countdown, leaderboard ---

	// How long a trigger message stays up, in frames. The original holds a
	// message of 80 characters or fewer for 4 seconds and adds 50 ms for every
	// character beyond that, so long transmissions stay readable.
	int trigger_text_lifetime(const a_string& s) const {
		int ms = 4000;
		if (s.size() > 80) ms += (int)(s.size() - 80) * 50;
		return ms * 24 / 1000;
	}

	void draw_trigger_texts(uint32_t* data, size_t pitch) {
		while (!trigger_texts.empty()
		       && (int)st.current_frame - trigger_texts.front().frame > trigger_texts.front().lifetime) {
			trigger_texts.erase(trigger_texts.begin());
		}
		if (trigger_texts.empty()) return;
		// Anchored to the HUD rather than the top of the screen: the messages
		// sit just above the ping button, sharing its left margin, and stack
		// upward so the newest line is always the one nearest the HUD.
		// Faze 4.7: in the HUD v2 path the glyphs scale (hud_scale_text) with the
		// tier/zoom, so the line height must too, or the transmission lines overlap
		// at FULL HD. Legacy keeps native metrics. usp_h()/hud_h_px() == native at
		// SD/HD2 default zoom.
		int lh = hud_scale_text ? (usp_h() + hud_h_px(4)) : (ui::unispace_h + 4);
		int gap6 = hud_scale_text ? hud_h_px(6) : 6;
		int x = hud_scale_text ? hud_h_px(12) : 12, bottom = (int)screen_height - (hud_scale_text ? hud_h_px(12) : 12);
		if (hud_v3) {
			// right of the group rail, above IDLE / ARMY / PING
			hud3_layout_t L3 = hud3_layout();
			x = L3.idle.from.x + d3(64 + 12);
			bottom = L3.idle.from.y - gap6;
		} else if (!ui_legacy) {
			rect p = hud2_ping_button();
			x = p.from.x;
			bottom = p.from.y - gap6;
		} else {
			bottom = playfield_bottom_y() - gap6;
		}
		int y = bottom - (int)trigger_texts.size() * lh;
		if (y < 8) y = 8;
		for (auto& t : trigger_texts) {
			draw_text_rgba(data, pitch, x, y, t.text.c_str(), 0xff60ff60u, 2);
			y += lh;
		}
	}

	// In-game chat: a fading history of messages plus, while typing, an input
	// field. v0.125: the field is 720 x 50 dp
	// over the unit panel, just above the bar, framed in the race's "on" colour;
	// the messages sit above it on dark strips with the sender's name in the
	// player's colour and the text in white. Everything is in buffer rows and goes
	// through hud_h_px(): the old bar took its row from playfield_view_height()
	// (WORLD rows) and its sizes were not scaled, so at the FULL HD tier it was
	// half-size and in the wrong place.
	void draw_chat(uint32_t* data, size_t pitch) {
		while (!chat_lines.empty() && (int)st.current_frame - chat_lines.front().frame > 24 * 10)
			chat_lines.erase(chat_lines.begin());
		if (chat_lines.empty() && !chat_active) return;
		bool _prev_hst = hud_scale_text;
		hud_scale_text = true;
		struct g_t { bool* f; bool p; ~g_t() { *f = p; } } _g{&hud_scale_text, _prev_hst};
		const hud_theme_t th = hud_theme();
		// v0.83: SDL keeps its surface at full size when the IME opens, so the
		// field must be lifted clear of the keyboard or you type without seeing it.
		int bottom = hud_v3 ? hud3_layout().card_full.from.y : playfield_bottom_y();   // v3: above the info card
		if (ime_top_y() < bottom) bottom = ime_top_y();
		if (bottom < hud_h_px(120)) bottom = hud_h_px(120);   // never push it off-screen
		int x0 = hud_h_px(8), x1 = (int)screen_width - hud_h_px(8);
		if (!ui_legacy) { rect up = hud2_unit_panel(); x0 = up.from.x; x1 = up.to.x; }
		if (x1 - x0 > hud_h_px(720)) x1 = x0 + hud_h_px(720);
		const int gap = hud_h_px(6), in = hud_h_px(6);
		const int text_h = menu_text_h(2), adv = menu_text_adv(2);
		rect field = {{x0, bottom - gap - hud_h_px(50)}, {x1, bottom - gap}};
		const int strip_h = text_h + hud_h_px(6);
		int y = (chat_active ? field.from.y : bottom) - gap;
		for (auto it = chat_lines.rbegin(); it != chat_lines.rend() && y - strip_h > hud_h_px(34); ++it) {
			y -= strip_h;
			const a_string& t = it->text;
			rect strip = {{x0, y}, {x0 + (int)t.size() * adv + 2 * in, y + strip_h - hud_h_px(2)}};
			if (strip.to.x > (int)screen_width - hud_h_px(8)) strip.to.x = (int)screen_width - hud_h_px(8);
			blend_rect_rgba(data, pitch, strip, 0x05070au, 208);
			const int ty = (strip.from.y + strip.to.y) / 2 - text_h / 2;
			size_t sep = t.find(": ");
			if (sep == a_string::npos) {
				draw_text_rgba(data, pitch, x0 + in, ty, t.c_str(), it->color, 2);
			} else {
				draw_text_rgba(data, pitch, x0 + in, ty, t.substr(0, sep + 1).c_str(), it->color, 2);
				draw_text_rgba(data, pitch, x0 + in + (int)(sep + 1) * adv, ty, t.c_str() + sep + 1, 0xffffffffu, 2);
			}
		}
		if (chat_active) {
			fill_rect_rgba(data, pitch, field, th.portrait_fill);
			line_rectangle_rgba(data, pitch, field, th.on_line);
			const int pad = hud_h_px(12);
			const int ty = (field.from.y + field.to.y) / 2 - text_h / 2;
			const char* lbl = "Message:";
			draw_text_rgba(data, pitch, field.from.x + pad, ty, lbl, th.amber, 2);
			const int tx = field.from.x + pad + menu_text_w(lbl, 2) + adv;
			// a line longer than the field shows its end -- where the caret is
			int room = (field.to.x - pad - tx) / adv - 1;
			if (room < 1) room = 1;
			a_string shown = (int)chat_input.size() > room ? chat_input.substr(chat_input.size() - (size_t)room) : chat_input;
			if (((int)st.current_frame / 12) & 1) shown += "_"; // ~1 Hz caret
			draw_text_rgba(data, pitch, tx, ty, shown.c_str(), 0xffffffffu, 2);
		}
	}

	void draw_trigger_countdown(uint32_t* data, size_t pitch) {
		if (st.countdown_timer_ms <= 0) return;
		int secs = st.countdown_timer_ms / 1000;
		char buf[16];
		snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
		draw_text_rgba(data, pitch, (int)screen_width / 2 - 20, 22, buf, 0xff4040ffu, 2);
	}

	uint32_t player_colors_rgba(int p) {
		// Standard BW player colors (ARGB), indexed by the player's color slot.
		// v1.0.22: all 16 rows of game/tunit.pcx (measured through the game palette).
		static const uint32_t colors[16] = {
			0xfff40404u, 0xff0c48ccu, 0xff2cb494u, 0xff88409cu,
			0xfff88c14u, 0xff703014u, 0xffcce0d0u, 0xfffcfc38u,
			0xff088008u, 0xfffcfc7cu, 0xffecc4b0u, 0xff4068d4u,
			0xff74a47cu, 0xff9090b8u, 0xfffcfc7cu, 0xff00e4fcu,
		};
		size_t ci = (size_t)st.players.at(p).color;
		return colors[ci < 16 ? ci : 0];
	}

	int trigger_leaderboard_value(int p) {
		auto& lb = st.leaderboard;
		switch (lb.type) {
		case 1: return trigger_command_count(st, p, 13, lb.unit_id, true);
		case 2:
			if (lb.location < 1 || (size_t)lb.location > st.locations.size()) return 0;
			return trigger_command_count(trigger_bring_count(st.locations[lb.location - 1]), p, 13, lb.unit_id, true);
		case 3: case 6: return trigger_player_resources(p, lb.resource_type);
		case 4: return trigger_table_count(st.unit_kills, p, 13, lb.unit_id);
		case 5: return trigger_player_score(p, lb.score_type);
		default: return 0;
		}
	}

	void draw_trigger_leaderboard(uint32_t* data, size_t pitch) {
		auto& lb = st.leaderboard;
		if (lb.type == 0) return;
		// Recompute standings only a few times per second — control-at-location
		// scans units and would be wasteful per frame.
		if (leaderboard_cache_frame < 0 || (int)st.current_frame - leaderboard_cache_frame >= 12) {
			leaderboard_cache_frame = (int)st.current_frame;
			leaderboard_cache.clear();
			for (int p = 0; p != 8; ++p) {
				auto c = st.players[p].controller;
				bool human = c == player_t::controller_occupied;
				bool computer = c == player_t::controller_computer_game;
				if (!human && !(computer && lb.show_computers)) continue;
				leaderboard_cache.push_back({p, trigger_leaderboard_value(p)});
			}
			std::sort(leaderboard_cache.begin(), leaderboard_cache.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
				if (a.second != b.second) return a.second > b.second;
				return a.first < b.first;
			});
		}
		int x = (int)screen_width - 170;
		int y = 40;
		a_string label = lb.type == 6 ? a_string("Greed") : trigger_sanitize(trigger_string(lb.string_index));
		if (lb.goal >= 0) {
			char gb[64];
			snprintf(gb, sizeof(gb), "%s (goal %d)", label.c_str(), lb.goal);
			draw_text_rgba(data, pitch, x, y, gb, 0xff00e0e0u, 2);
		} else {
			draw_text_rgba(data, pitch, x, y, label.c_str(), 0xff00e0e0u, 2);
		}
		y += 18;
		for (auto& v : leaderboard_cache) {
			char buf[64];
			const char* name = st.players[v.first].controller == player_t::controller_occupied ? "Player" : "Computer";
			snprintf(buf, sizeof(buf), "%s %d: %d", name, v.first + 1, v.second);
			draw_text_rgba(data, pitch, x, y, buf, player_colors_rgba(v.first), 2);
			y += 16;
		}
	}

	a_vector<std::pair<int, int>> leaderboard_cache;
	int leaderboard_cache_frame = -1;

	void draw_trigger_hud(uint32_t* data, size_t pitch) {
		draw_trigger_texts(data, pitch);
		// Countdown is now drawn by draw_game_timer (replaces the elapsed clock in
		// timed modes) so the two opposite clocks no longer overlap.
		draw_trigger_leaderboard(data, pitch);
	}

	// Select all of the local player's combat units (not workers, not
	// buildings) — a quick "select army".
	void select_army() {
		if (game_local_player < 0) return;
		current_selection_clear();
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player) continue;
			if (!unit_can_be_selected(u)) continue;
			if (u->unit_type->flags & unit_type_t::flag_building) continue;
			auto id = u->unit_type->id;
			bool worker = id == UnitTypes::Terran_SCV || id == UnitTypes::Protoss_Probe || id == UnitTypes::Zerg_Drone;
			if (worker) continue;
			if (u->unit_type->flags & unit_type_t::flag_worker) continue;
			current_selection_add(u);
			if (current_selection.size() >= 12) break;
		}
	}

	// Select the next idle worker (no active order) and center the camera on it,
	// cycling on repeated taps.
	int idle_worker_cycle = 0;
	void select_next_idle_worker() {
		if (game_local_player < 0) return;
		a_vector<unit_t*> idle;
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player) continue;
			auto id = u->unit_type->id;
			bool worker = id == UnitTypes::Terran_SCV || id == UnitTypes::Protoss_Probe || id == UnitTypes::Zerg_Drone;
			if (!worker) continue;
			if (!u_completed(u)) continue;
			Orders o = u->order_type->id;
			bool is_idle = o == Orders::PlayerGuard || o == Orders::Guard || o == Orders::Nothing || o == Orders::Stop;
			if (is_idle) idle.push_back(u);
		}
		if (idle.empty()) return;
		if (idle_worker_cycle >= (int)idle.size()) idle_worker_cycle = 0;
		unit_t* u = idle[idle_worker_cycle];
		++idle_worker_cycle;
		current_selection_clear();
		current_selection_add(u);
		center_camera_on(u->position);
	}

	// How many completed workers are sitting idle. Shown as a badge on the IDLE
	// button so the player can see there is something to collect without
	// tapping to find out.
	int idle_worker_count() const {
		if (game_local_player < 0) return 0;
		int n = 0;
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player) continue;
			auto id = u->unit_type->id;
			if (id != UnitTypes::Terran_SCV && id != UnitTypes::Protoss_Probe && id != UnitTypes::Zerg_Drone) continue;
			if (!u_completed(u)) continue;
			Orders o = u->order_type->id;
			if (o == Orders::PlayerGuard || o == Orders::Guard || o == Orders::Nothing || o == Orders::Stop) ++n;
		}
		return n;
	}

	// --- Unit clustering (v0.72) ---------------------------------------
	// Shared by the ARMY button (item 9) and the mineral-patch worker counts
	// (item 19). Single-link clustering: a unit joins a cluster if it is within
	// `radius` of any member. O(n^2) over the units passed in, which is fine at
	// StarCraft scale (a few hundred at most) and is only run on demand.
	template<typename Pred>
	void cluster_units(const a_vector<unit_t*>& in, int radius, Pred&&,
	                   a_vector<a_vector<unit_t*>>& out) {
		out.clear();
		a_vector<bool> taken(in.size(), false);
		int r2 = radius * radius;
		for (size_t i = 0; i != in.size(); ++i) {
			if (taken[i]) continue;
			taken[i] = true;
			a_vector<unit_t*> group;
			group.push_back(in[i]);
			for (size_t g = 0; g != group.size(); ++g) {
				xy p = group[g]->position;
				for (size_t j = 0; j != in.size(); ++j) {
					if (taken[j]) continue;
					xy d = in[j]->position - p;
					if (d.x * d.x + d.y * d.y > r2) continue;
					taken[j] = true;
					group.push_back(in[j]);
				}
			}
			out.push_back(std::move(group));
		}
	}

	// "Army" = everything that is not a worker, a building or a larva/egg.
	// Transports and support casters are included: they belong to the army, and
	// excluding them would make the button lie about where your forces are.
	bool unit_is_army(unit_t* u) const {
		if (!u || u->owner != game_local_player) return false;
		if (!u_completed(u)) return false;
		if (u->unit_type->flags & unit_type_t::flag_building) return false;
		auto id = u->unit_type->id;
		if (id == UnitTypes::Terran_SCV || id == UnitTypes::Protoss_Probe || id == UnitTypes::Zerg_Drone) return false;
		if (id == UnitTypes::Zerg_Larva || id == UnitTypes::Zerg_Egg
		    || id == UnitTypes::Zerg_Lurker_Egg || id == UnitTypes::Zerg_Cocoon) return false;
		return true;
	}

	a_vector<a_vector<unit_t*>> army_clusters() {
		a_vector<unit_t*> army;
		a_vector<a_vector<unit_t*>> out;
		if (game_local_player < 0) return out;
		for (unit_t* u : ptr(st.player_units[game_local_player]))
			if (unit_is_army(u)) army.push_back(u);
		cluster_units(army, 32 * 12, [](unit_t*) { return true; }, out);
		return out;
	}
	int army_cycle = 0;

	// v0.72 item 9: BW can only select 12 units, so "select the whole army" is
	// not a command the engine can express. Instead the button walks the army
	// cluster by cluster, newest tap moving to the next one, which is both
	// honest about the limit and closer to what the player actually wants
	// ("where are my troops?"). Long-press assigns the cluster to a free
	// control group, turning the button into an organising tool.
	void select_army_cluster(bool assign_group) {
		auto clusters = army_clusters();
		if (clusters.empty()) return;
		if (army_cycle >= (int)clusters.size()) army_cycle = 0;
		auto& c = clusters[army_cycle];
		current_selection_clear();
		int n = 0;
		int cap = unlimited_sel_on() ? (int)wide_selection_cap : 12;
		for (unit_t* u : c) { if (n++ >= cap) break; current_selection_add(u); }
		if (!c.empty()) center_camera_on(c.front()->position);
		if (assign_group) {
			for (int g = 1; g <= 9; ++g) {
				if (!control_groups[g].empty()) continue;
				control_groups[g].clear();
				const a_vector<unit_id>& src = unlimited_sel_on() && !wide_selection.empty() ? wide_selection : current_selection;
				for (auto uid : src) control_groups[g].push_back(uid);
				show_hud_message(L("Group assigned"));
				break;
			}
		}
		army_cycle = (army_cycle + 1) % (int)clusters.size();
	}

	// v0.64 item 7: IDLE / ARMY sit in one row to the right of the ping button
	// instead of stacked under the zoom buttons, so the three touch targets read
	// as one toolbar. Legacy HUD keeps the old stacked placement.
	rect util_button_area(int which) {
		if (!ui_legacy) return hud2_util_cell(1 + which);   // 0 is the ping button
		int sz = console_len(26.0f);
		if (sz <= 0) return {};
		rect z = zoom_button_area(0);
		int top = z.to.x > z.from.x ? z.to.y + 6 : 46; // zoom buttons may be off
		int w = sz * 2;
		int x = 12;
		int y = top + which * (sz + 6);
		return {{x, y}, {x + w, y + sz}};
	}

	void draw_util_buttons(uint32_t* data, size_t pitch) {
		for (int i = 0; i < 2; ++i) {
			rect r = util_button_area(i);
			if (r.to.x <= r.from.x) continue;
			// v0.64 item 8: IDLE is an icon-plus-count badge (the Repair
			// cmdicon reads as "a worker with nothing to do"), dimmed rather
			// than hidden at zero so the toolbar never reflows under the
			// player's thumb.
			int n = i == 0 ? idle_worker_count() : (int)army_clusters().size();
			bool dim = n == 0;
			fill_rect_rgba(data, pitch, r, dim ? 0x80141a20u : (hud_pressed(r) ? hud_theme().btn_pressed : hud_theme().util_fill));
			line_rectangle_rgba(data, pitch, r, dim ? hud_theme().util_line_zero : hud_theme().util_line);
			uint32_t fg = dim ? 0xff707880u : 0xffc0c8d0u;
			if (i == 0) {
				int h = r.to.y - r.from.y;
				rect icon = {{r.from.x + 2, r.from.y + 2}, {r.from.x + h - 2, r.to.y - 2}};
				draw_cmdicon_fit(data, pitch, cmdicon_repair, icon);
				char buf[16];
				snprintf(buf, sizeof(buf), "%d", n);
				draw_text_rgba(data, pitch, r.from.x + h + 4 * hud_us(),
					(r.from.y + r.to.y) / 2 - usp_h() / 2, buf, fg);
			} else {
				// v0.75 item 6: the Attack cmdicon instead of the word ARMY, to
				// match IDLE beside it. The badge is the number of army
				// clusters, so the player can see at a glance whether their
				// forces are together or split before tapping.
				int groups = n;
				int h = r.to.y - r.from.y;
				rect icon = {{r.from.x + 2, r.from.y + 2}, {r.from.x + h - 2, r.to.y - 2}};
				draw_cmdicon_fit(data, pitch, cmdicon_attack, icon);
				char buf[16];
				snprintf(buf, sizeof(buf), "%d", groups);
				draw_text_rgba(data, pitch, r.from.x + h + 4 * hud_us(),
					(r.from.y + r.to.y) / 2 - usp_h() / 2, buf,
					groups > 0 ? fg : 0xff707880u);
			}
		}
	}

	bool util_button_hit(int mx, int my, int& which) {
		for (int i = 0; i < 2; ++i) {
			rect r = util_button_area(i);
			if (r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) {
				which = i;
				return true;
			}
		}
		return false;
	}

	a_vector<unit_id> current_selection;

	// SCBW2 unlimited_selection (bit3): a UI-side WIDE selection that may exceed BW's
	// 12-unit cap. The engine, command card and wireframe still read current_selection
	// (always <=12 = the active SC2-style "sub-card"); wide_selection holds the full
	// set, grouped by unit type into sub-cards paged by sel_page (the 1/2/3 tabs).
	// Orders fan over the whole wide set as successive legal <=12 select+order batches,
	// so nothing ever emits a >12 select opcode (the sim errors at 13, actions.h:283).
	// FLAG OFF: wide_selection stays empty, sel_page 0, and every path below is
	// byte-identical to vanilla (this is the determinism-safe default the gate runs).
	a_vector<unit_id> wide_selection;
	int sel_page = 0;
	static const size_t wide_selection_cap = 72;   // Enhanced selection: 6 pages x 12

	bool unlimited_sel_on() const { return st.game->qol.unlimited_selection; }

	// Sub-cards are pages of 12: the wireframe grid shows the active page's <=12 units,
	// the 1/2/3 selectors on the left of the Unit Display switch pages. Selectors only
	// appear when there are MORE than 12 units selected (page count > 1).
	int wide_page_count() { int n = (int)wide_selection.size(); return n <= 0 ? 0 : (n + 11) / 12; }

	// Recompute current_selection = the sel_page'th page of 12 from wide_selection,
	// pruning dead units first. current_selection therefore stays <=12 (the engine /
	// card / wireframe cap) while wide_selection carries the full set.
	void rebuild_active_page() {
		a_vector<unit_id> live;
		for (auto uid : wide_selection) { unit_t* u = get_unit(uid); if (u && u->sprite) live.push_back(uid); }
		wide_selection.swap(live);
		current_selection.clear();
		if (wide_selection.empty()) { sel_page = 0; return; }
		int pages = wide_page_count();
		if (sel_page >= pages) sel_page = pages - 1;
		if (sel_page < 0) sel_page = 0;
		size_t begin = (size_t)sel_page * 12;
		size_t end = begin + 12 < wide_selection.size() ? begin + 12 : wide_selection.size();
		for (size_t i = begin; i < end; ++i) current_selection.push_back(wide_selection[i]);
	}

	// Issue an order to the WHOLE selection. With unlimited_selection and a wide set
	// bigger than the active sub-card, fan the order over successive <=12 batches (each
	// a legal recorded select+order pair), then restore the active sub-card in the
	// engine; otherwise select the active page and emit once. `writer` fills the
	// game_action_writer with the order (opcode + payload). Returns true if it emitted.
	template<class F>
	bool emit_order_to_selection(F writer) {
		if (unlimited_sel_on() && wide_selection.size() > current_selection.size()) {
			bool any = false;
			a_vector<unit_id> saved; saved.swap(current_selection);
			for (size_t i = 0; i < wide_selection.size(); i += 12) {
				size_t j = i + 12 < wide_selection.size() ? i + 12 : wide_selection.size();
				current_selection.assign(wide_selection.begin() + i, wide_selection.begin() + j);
				if (!submit_select_action()) continue;
				game_action_writer w; writer(w); submit_game_action(w); any = true;
			}
			current_selection.swap(saved);
			submit_select_action();   // restore engine selection to the active sub-card
			return any;
		}
		if (!submit_select_action()) return false;
		game_action_writer w; writer(w); submit_game_action(w);
		return true;
	}

	// DIAG (bug-catcher) rally detector: group-rally intent is captured at submit
	// and verified a few sim frames later against committed unit->building.rally,
	// so a "group rally applied to a subset" bug becomes a structured OBDIAG event.
	a_vector<unit_id> diag_rally_units;
	xy diag_rally_pos;
	int diag_rally_target_id = -1;   // >=0 = unit-rally target id, -1 = tile rally
	int diag_rally_check_frame = -1;

	// smart-cast self-test detector: how many of the selected casters actually took the
	// spell ORDER (smart_cast should route it to exactly one, not the group). Order-based
	// (not energy) because an area spell like EMP drains the clustered casters' energy
	// regardless of who cast it -- the order is the clean signal, set on issue and held
	// while the caster flies into range.
	a_vector<unit_id> diag_cast_units;
	int diag_cast_order = -1;
	int diag_cast_check_frame = -1;

	// unlimited_selection self-test detector: did the order fan reach the WHOLE wide
	// set (all should hold the fanned order), not just the active <=12 sub-card.
	a_vector<unit_id> diag_wide_units;
	int diag_wide_order = -1;
	int diag_wide_check_frame = -1;

	// De-dup for the general invariant detectors: a persistent bad state (an off-map unit,
	// a supply overflow) reports ONCE per (kind,id) per session instead of every frame.
	std::unordered_set<uint64_t> diag_seen;
	uint32_t diag_prev_frame = 0xffffffffu; // detect a new-game frame reset to clear diag_seen
	bool diag_once(uint32_t kind, uint32_t id) { return diag_seen.insert(((uint64_t)kind << 32) | id).second; }
	// perf_hitch state: frame-to-frame wall-clock EMA + last-hitch throttle.
	int64_t diag_last_tick_us = 0, diag_avg_us = 0; int diag_ticks = 0; uint32_t diag_last_hitch_f = 0;
	// mem_pressure state: last RSS sample (KB) for the coarse-cadence delta/high-water check.
	int64_t diag_last_rss_kb = 0; int64_t diag_peak_rss_kb = 0;
	// Read this process's resident set size (KB) from /proc/self/statm. Cheap; main-thread.
	int64_t diag_read_rss_kb() {
		FILE* f = std::fopen("/proc/self/statm", "r");
		if (!f) return 0;
		long total_pages = 0, resident_pages = 0;
		int n = std::fscanf(f, "%ld %ld", &total_pages, &resident_pages);
		std::fclose(f);
		if (n < 2) return 0;
		return (int64_t)resident_pages * (sysconf(_SC_PAGESIZE) / 1024);
	}
	// trig_stall (Cat 1a): per (player,trigger) frame the trigger first became "one condition
	// away" (all conditions pass but one). A campaign soft-lock is a trigger stuck one-away
	// forever. Keyed by (rt.t ^ player); cleared on new-game + pruned each poll.
	std::unordered_map<uint64_t, uint32_t> diag_trig_since;
	// Movement-anomaly detectors (Cat 2): harness-owned per-unit state, keyed by unit id
	// raw_value (carries a 5-bit generation, so a recycled pool slot does not alias). All
	// read-only. Pruned on new-game reset + a periodic dead-entry GC so it can't grow forever.
	struct diag_mv_t {
		xy prev{};               // last-frame committed position
		uint32_t epoch = 0;      // st.current_frame+1 of last touch (0 == never seen -> no prev)
		uint16_t stuck = 0;      // consecutive ordered-but-not-moving frames
		int16_t last_cx = -32768, last_cy = 0; // last DISTINCT tile pushed to the ring
		int16_t ring_x[6] = {0}, ring_y[6] = {0}; uint8_t rhead = 0, rcnt = 0; // distinct-cell trail
		uint8_t to1 = 255, to2 = 255, flips = 0; // order_thrash: last two distinct orders + flip count
	};
	std::unordered_map<uint32_t, diag_mv_t> diag_mv;
	// order_thrash (B7): many orders legitimately alternate fast and are NOT indecision --
	// combat (attack family flips AttackUnit<->MoveToAttack every engagement), harvest (workers
	// cycle MoveToMinerals/Mining/Return), the medic heal cycle (HealMove<->MedicHeal<->
	// MedicHealToIdle), carrier interceptors (InterceptorAttack<->InterceptorReturn), and the
	// idle-revert states (Guard/PlayerGuard/Nothing a unit drops into between commands -- the
	// same set diag_bot_idle_tick treats as idle). Excluding these keeps the detector quiet on
	// a normal melee; what survives is genuine A<->B command indecision.
	static bool diag_order_thrash_ok(int oid) {
		switch (oid) {
		case (int)Orders::AttackUnit: case (int)Orders::MoveToAttack: case (int)Orders::AttackMove:
		case (int)Orders::AttackDefault: case (int)Orders::AttackFixedRange: case (int)Orders::AttackTile:
		case (int)Orders::Follow: case (int)Orders::Patrol:
		case (int)Orders::MoveToMinerals: case (int)Orders::WaitForMinerals: case (int)Orders::MiningMinerals:
		case (int)Orders::ReturnMinerals: case (int)Orders::MoveToGas: case (int)Orders::WaitForGas:
		case (int)Orders::HarvestGas: case (int)Orders::ReturnGas: case (int)Orders::Harvest1:
		case (int)Orders::Harvest2: case (int)Orders::ResetHarvestCollision: case (int)Orders::ResetCollision:
		case (int)Orders::HealMove: case (int)Orders::MedicHeal: case (int)Orders::MedicHealToIdle:
		case (int)Orders::InterceptorAttack: case (int)Orders::InterceptorReturn:
		case (int)Orders::Guard: case (int)Orders::PlayerGuard: case (int)Orders::Nothing:
			return false; // excluded (legit fast alternation / idle-revert)
		default: return true;
		}
	}

	// Called once per rendered frame (after prof_frame_done). Read-only.
	void diag_frame_tick() {
		if (!bwgame::diag::enabled()) return;
		// (4) group building rally applied to only a subset of selected buildings.
		if (diag_rally_check_frame >= 0 && (int)st.current_frame >= diag_rally_check_frame) {
			int total = 0, applied = 0; std::string miss;
			for (auto uid : diag_rally_units) {
				unit_t* u = get_unit(uid); if (!u) continue; ++total;
				bool ok = diag_rally_target_id >= 0
					? (u->building.rally.unit && (int)get_unit_id(u->building.rally.unit).raw_value == diag_rally_target_id)
					: (u->building.rally.pos.x == diag_rally_pos.x && u->building.rally.pos.y == diag_rally_pos.y);
				if (ok) ++applied; else { if (!miss.empty()) miss += ","; miss += std::to_string(get_unit_id(u).raw_value); }
			}
			if (total > 1) {
				if (applied < total)
					bwgame::diag::emit((long)st.current_frame, "rally", bwgame::diag::Sev::error, "RALLY_GROUP_PARTIAL",
						"group rally applied to " + std::to_string(applied) + "/" + std::to_string(total) + " selected buildings",
						"\"sel\":" + std::to_string(total) + ",\"applied\":" + std::to_string(applied) + ",\"missing\":[" + miss + "]");
				else
					bwgame::diag::emit((long)st.current_frame, "rally", bwgame::diag::Sev::info, "RALLY_GROUP_OK",
						"group rally applied to all " + std::to_string(total) + " buildings", "\"sel\":" + std::to_string(total));
			}
			diag_rally_check_frame = -1; diag_rally_units.clear();
		}
		// (5) smart-cast should route the spell to ONE caster, not the group: count how
		// many of the selected casters took the cast ORDER (set on issue, held while the
		// caster flies into range) -- energy is confounded by EMP's own area drain.
		if (diag_cast_check_frame >= 0 && (int)st.current_frame >= diag_cast_check_frame) {
			int total = 0, casting = 0;
			for (auto uid : diag_cast_units) {
				unit_t* u = get_unit(uid); if (!u) continue; ++total;
				if (u->order_type && (int)u->order_type->id == diag_cast_order) ++casting;
			}
			if (total > 1) {
				std::string d = "\"casters\":" + std::to_string(total) + ",\"casting\":" + std::to_string(casting);
				if (casting == 1)
					bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::info, "SMART_CAST_OK",
						"spell order routed to exactly 1 of " + std::to_string(total) + " casters", d);
				else if (casting >= 2)
					bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::error, "SMART_CAST_ALL",
						std::to_string(casting) + "/" + std::to_string(total) + " casters took the order (group cast, not smart)", d);
				else // casting == 0: order already ended / never set -> inconclusive, must NOT read as OK
					bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::error, "SMART_CAST_NOCAST",
						"no caster is executing the cast order in the check window (inconclusive)", d);
			}
			diag_cast_check_frame = -1; diag_cast_units.clear();
		}
		// unlimited_selection fan self-test: how many of the wide set took the fanned order.
		if (diag_wide_check_frame >= 0 && (int)st.current_frame >= diag_wide_check_frame) {
			int total = 0, got = 0;
			for (auto uid : diag_wide_units) {
				unit_t* u = get_unit(uid); if (!u) continue; ++total;
				if (u->order_type && (int)u->order_type->id == diag_wide_order) ++got;
			}
			if (total > 0) {
				std::string d = "\"total\":" + std::to_string(total) + ",\"got\":" + std::to_string(got);
				if (got == total)
					bwgame::diag::emit((long)st.current_frame, "selection", bwgame::diag::Sev::info, "WIDE_ORDER_ALL",
						"order fan reached all " + std::to_string(total) + " units", d);
				else
					bwgame::diag::emit((long)st.current_frame, "selection", bwgame::diag::Sev::error, "WIDE_ORDER_PARTIAL",
						"order fan reached only " + std::to_string(got) + "/" + std::to_string(total) + " (wide fan broken)", d);
			}
			diag_wide_check_frame = -1; diag_wide_units.clear();
		}
		// (extra) selection-cap safety net: the UI selection must never exceed 12.
		if (current_selection.size() > 12)
			bwgame::diag::emit((long)st.current_frame, "selection", bwgame::diag::Sev::error, "SEL_CAP_EXCEEDED",
				"UI selection exceeds the 12-unit engine cap", "\"size\":" + std::to_string(current_selection.size()));
		// (6) unlimited_selection invariants -- bug-catcher for the wide-selection system
		// (bake the new system into the in-engine detector so a regression is caught over
		// adb without touch). The engine page must always be <=12 (checked above); with
		// the flag OFF the wide set must be empty; with it ON the active page must be a
		// same-type subset of wide and sel_page must be a valid sub-card index.
		if (!st.game->qol.unlimited_selection) {
			if (!wide_selection.empty())
				bwgame::diag::emit((long)st.current_frame, "selection", bwgame::diag::Sev::error, "WIDE_SEL_LEAK",
					"wide_selection is non-empty while unlimited_selection is off",
					"\"wide\":" + std::to_string(wide_selection.size()));
		} else if (!wide_selection.empty()) {
			int pages = wide_page_count();
			bool page_ok = sel_page >= 0 && sel_page < pages;
			bool subset = true;
			for (auto uid : current_selection)
				if (std::find(wide_selection.begin(), wide_selection.end(), uid) == wide_selection.end()) subset = false;
			bool cap_ok = current_selection.size() <= 12;
			if (!page_ok || !subset || !cap_ok)
				bwgame::diag::emit((long)st.current_frame, "selection", bwgame::diag::Sev::error, "WIDE_SEL_INVARIANT",
					"unlimited_selection invariant broken",
					"\"wide\":" + std::to_string(wide_selection.size()) + ",\"page\":" + std::to_string(sel_page) +
					",\"pages\":" + std::to_string(pages) + ",\"page_ok\":" + std::to_string((int)page_ok) +
					",\"subset\":" + std::to_string((int)subset) + ",\"cap_ok\":" + std::to_string((int)cap_ok));
		}
		// ---- general read-only invariant sweep (DIAG-DETECTORS PART A/B) --------------
		// Catalog detectors that observe committed st and never mutate it. De-duped per
		// (kind,id) so a persistent fault reports once, not every frame. Runs only under
		// --diag (whole function is gated at the top), so normal play pays nothing.
		// Clear the de-dup set when current_frame goes backwards (a new game/match in the
		// same process) so a game-2 fault isn't silently swallowed by a game-1 key.
		if ((uint32_t)st.current_frame < diag_prev_frame) { diag_seen.clear(); diag_mv.clear(); diag_last_rss_kb = 0; diag_trig_since.clear(); }
		diag_prev_frame = (uint32_t)st.current_frame;
		using DSev = bwgame::diag::Sev;
		long cf = (long)st.current_frame;
		// perf_hitch (B2): a single-frame wall-clock spike the 256-frame-averaged prof line
		// hides (decode stalls, GC-like pauses). Conservative -- >3x the running average AND
		// >50 ms (a sub-20fps frame), after a 120-frame warmup, throttled to once/60 frames --
		// so ordinary mobile jitter/vsync doesn't flood it.
		{
			int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
				prof_clock::now().time_since_epoch()).count();
			if (diag_last_tick_us) {
				int64_t dt = now_us - diag_last_tick_us;
				diag_avg_us = diag_avg_us ? (diag_avg_us * 15 + dt) / 16 : dt;
				if (diag_ticks > 120 && dt > diag_avg_us * 3 && dt > 50000
				    && (uint32_t)st.current_frame - diag_last_hitch_f > 60) {
					diag_last_hitch_f = (uint32_t)st.current_frame;
					bwgame::diag::emit(cf, "perf", DSev::warn, "PERF_HITCH", "single-frame time spike",
						"\"frame_us\":" + std::to_string(dt) + ",\"avg_us\":" + std::to_string(diag_avg_us));
				}
			}
			diag_last_tick_us = now_us; ++diag_ticks;
				// mem_pressure (B5): sample RSS every 256 frames. A big JUMP is an allocation
				// spike, a big DROP is a native-allocator (scudo/jemalloc) page purge -- the
				// leading suspect for the ~1s PERF_HITCH. Coarse cadence + delta hysteresis.
				if (((uint32_t)st.current_frame & 255u) == 0) {
					int64_t rss = diag_read_rss_kb();
					if (rss > 0) {
						if (rss > diag_peak_rss_kb) diag_peak_rss_kb = rss;
						int64_t delta = diag_last_rss_kb ? rss - diag_last_rss_kb : 0;
						std::string d = "\"rss_mb\":" + std::to_string(rss / 1024) +
							",\"delta_mb\":" + std::to_string(delta / 1024) +
							",\"peak_mb\":" + std::to_string(diag_peak_rss_kb / 1024);
						if (rss > (int64_t)1800 * 1024)   // repeats every 256f as a persistent-high-mem heartbeat
							bwgame::diag::emit(cf, "mem", DSev::warn, "MEM_PRESSURE", "resident set above budget", d);
						else if (diag_last_rss_kb && (delta > 200 * 1024 || delta < -200 * 1024))
							bwgame::diag::emit(cf, "mem", DSev::info, "MEM_STEP",
								delta > 0 ? "large RSS jump (allocation spike)" : "large RSS drop (allocator purge)", d);
						diag_last_rss_kb = rss;
					}
				}
		}
		for (int p = 0; p < 12; ++p) {
			// econ_invariant (B3): negative resources / supply above the legal cap. A small
			// tolerance over 200 absorbs the transient Zerg morph-completion overshoot.
			if (st.current_minerals[p] < 0 && diag_once(1, (uint32_t)p))
				bwgame::diag::emit(cf, "econ", DSev::error, "ECON_MINERALS_NEG",
					"player minerals went negative", "\"player\":" + std::to_string(p) + ",\"minerals\":" + std::to_string(st.current_minerals[p]));
			if (st.current_gas[p] < 0 && diag_once(2, (uint32_t)p))
				bwgame::diag::emit(cf, "econ", DSev::error, "ECON_GAS_NEG",
					"player gas went negative", "\"player\":" + std::to_string(p) + ",\"gas\":" + std::to_string(st.current_gas[p]));
			for (int rc = 0; rc < 3; ++rc)
				if (st.supply_used[p][rc].integer_part() > 216 && diag_once(3, (uint32_t)(p * 4 + rc)))
					bwgame::diag::emit(cf, "econ", DSev::error, "ECON_SUPPLY_OVERFLOW",
						"supply_used above the legal cap", "\"player\":" + std::to_string(p) + ",\"race\":" + std::to_string(rc) +
						",\"used\":" + std::to_string(st.supply_used[p][rc].integer_part()) +
						",\"avail\":" + std::to_string(st.supply_available[p][rc].integer_part()));
			// unit sweep: off-map position + hp/energy over the unit's own maximum.
			for (unit_t* u : ptr(st.player_units[p])) {
				uint32_t id = (uint32_t)get_unit_id(u).raw_value;
				if (!is_in_map_bounds(u->position) && diag_once(4, id))
					bwgame::diag::emit(cf, "unit", DSev::error, "UNIT_OFFMAP",
						"unit position is outside the map", "\"unit\":" + std::to_string(id) +
						",\"pos\":[" + std::to_string(u->position.x) + "," + std::to_string(u->position.y) + "]");
				if (u->unit_type->hitpoints.integer_part() > 0
				    && u->hp.integer_part() > u->unit_type->hitpoints.integer_part() + 1 && diag_once(5, id))
					bwgame::diag::emit(cf, "unit", DSev::error, "UNIT_HP_OVER_MAX",
						"unit hp exceeds its type maximum", "\"unit\":" + std::to_string(id) +
						",\"hp\":" + std::to_string(u->hp.integer_part()) + ",\"max\":" + std::to_string(u->unit_type->hitpoints.integer_part()));
				if (u->energy.integer_part() > unit_max_energy(u).integer_part() + 1 && diag_once(6, id))
					bwgame::diag::emit(cf, "unit", DSev::warn, "UNIT_ENERGY_OVER_MAX",
						"unit energy exceeds its maximum", "\"unit\":" + std::to_string(id) +
						",\"energy\":" + std::to_string(u->energy.integer_part()) + ",\"max\":" + std::to_string(unit_max_energy(u).integer_part()));
				// ---- movement-anomaly detectors (Cat 2): teleport / stuck / oscillate ------
				// Compare the committed position against the harness-owned previous frame.
				// Read-only; each fault de-duped per unit. Detectors gate on top_speed>0 so
				// immobile buildings never fire (their entries still exist but GC reclaims them).
				{
					diag_mv_t& mv = diag_mv[id];
					bool had_prev = mv.epoch != 0;
					xy pos = u->position;
					int oid = u->order_type ? (int)u->order_type->id : -1;
					int top = u->flingy_top_speed.integer_part();
					// one-frame relocations that are legitimate, not a teleport bug.
					bool warp_ok = u_loaded(u)
						|| oid == (int)Orders::CastRecall || oid == (int)Orders::Teleport
						|| oid == (int)Orders::WarpIn || oid == (int)Orders::ArchonWarp
						|| oid == (int)Orders::DarkArchonMeld || oid == (int)Orders::EnterNydusCanal
						|| oid == (int)Orders::EnterTransport || oid == (int)Orders::Unload
						|| oid == (int)Orders::MoveUnload || oid == (int)Orders::Pickup4
						|| oid == (int)Orders::PickupTransport || oid == (int)Orders::PickupBunker
						|| oid == (int)Orders::Burrowing || oid == (int)Orders::Unburrowing
						|| oid == (int)Orders::BuildingLiftoff || oid == (int)Orders::LiftingOff
						|| oid == (int)Orders::BuildingLand || oid == (int)Orders::DroneLand
						|| oid == (int)Orders::DroneLiftOff;
					// 2a teleport: single-frame jump beyond 4x top speed AND beyond a 128px (4-tile)
					// absolute floor. The floor kills the benign micro-snaps a normal melee makes
					// -- larva wiggle, worker mineral/harvest collision resets (orders Larva /
					// MoveToMinerals / MiningMinerals / ResetHarvestCollision, all measured <=92px on
					// a 5-min bot-vs-AI run) -- while any real teleport (Recall, blink, a placement
					// bug) moves hundreds of pixels and still trips.
					if (had_prev && top > 0 && !warp_ok) {
						int dist = xy_length(pos - mv.prev);
						if (dist > top * 4 && dist > 128 && diag_once(7, id))
							bwgame::diag::emit(cf, "unit", DSev::warn, "UNIT_TELEPORT",
								"single-frame displacement exceeds 4x top speed",
								"\"unit\":" + std::to_string(id) + ",\"dist\":" + std::to_string(dist) +
								",\"top\":" + std::to_string(top) + ",\"order\":" + std::to_string(oid) +
								",\"from\":[" + std::to_string(mv.prev.x) + "," + std::to_string(mv.prev.y) +
								"],\"to\":[" + std::to_string(pos.x) + "," + std::to_string(pos.y) + "]");
					}
					// 2c stuck: an explicit Move/AttackMove order (Follow excluded -- a follower
					// legitimately parks next to a stationary leader) with a distinct target, yet
					// zero displacement for STUCK_FRAMES. Any 1px collision-jostle resets the
					// counter, so brief chokepoint congestion (which micro-jitters) never reaches
					// the threshold -- only a truly frozen unit does. 120 frames ~= 5 s at fastest.
					// Split by path: no route == UNIT_UNREACHABLE (info, expected on a walled goal);
					// a real path yet no progress == UNIT_STUCK (warn, the engine-fault signal).
					bool stuck_order = oid == (int)Orders::Move || oid == (int)Orders::AttackMove
						|| oid == (int)Orders::MoveToAttack;
					if (had_prev && top > 0 && stuck_order && u->move_target.pos != pos && pos == mv.prev) {
						if (++mv.stuck == 120) {
							bool no_path = u->path == nullptr;
							std::string d = "\"unit\":" + std::to_string(id) + ",\"pos\":[" + std::to_string(pos.x) + "," +
								std::to_string(pos.y) + "],\"target\":[" + std::to_string(u->move_target.pos.x) + "," +
								std::to_string(u->move_target.pos.y) + "],\"order\":" + std::to_string(oid);
							if (no_path && diag_once(10, id))
								bwgame::diag::emit(cf, "unit", DSev::info, "UNIT_UNREACHABLE",
									"unit ordered to move but has no path to the target", d);
							else if (!no_path && diag_once(8, id))
								bwgame::diag::emit(cf, "unit", DSev::warn, "UNIT_STUCK",
									"unit has a path yet has not advanced for 120 frames", d);
						}
					} else mv.stuck = 0;
					// 2d oscillate: under a persistent move/attack/follow order, the unit re-enters
					// a tile it already visited >=2 times within the last 6 DISTINCT-cell
					// transitions (net-zero pathing thrash). Distinct-cell trail avoids counting a
					// slow crossing. Follow is kept here (orbit thrash is a real anomaly) but the
					// >=3-visit threshold makes normal leader-following a non-issue.
					bool moving_order = stuck_order || oid == (int)Orders::Follow;
					if (moving_order) {
						int16_t cx = (int16_t)(pos.x / 32), cy = (int16_t)(pos.y / 32);
						if (cx != mv.last_cx || cy != mv.last_cy) {
							int revisits = 0;
							for (int i = 0; i < mv.rcnt; ++i)
								if (mv.ring_x[i] == cx && mv.ring_y[i] == cy) ++revisits;
							mv.ring_x[mv.rhead] = cx; mv.ring_y[mv.rhead] = cy;
							mv.rhead = (uint8_t)((mv.rhead + 1) % 6); if (mv.rcnt < 6) ++mv.rcnt;
							mv.last_cx = cx; mv.last_cy = cy;
							if (revisits >= 2 && diag_once(9, id))
								bwgame::diag::emit(cf, "unit", DSev::warn, "UNIT_OSCILLATE",
									"unit thrashing between tiles under a move order",
									"\"unit\":" + std::to_string(id) + ",\"cell\":[" + std::to_string(cx) + "," +
									std::to_string(cy) + "],\"revisits\":" + std::to_string(revisits + 1) +
									",\"order\":" + std::to_string(oid));
						}
					} else { mv.rcnt = 0; mv.rhead = 0; mv.last_cx = -32768; }
					// order_thrash (B7): the unit's order strictly alternates A<->B for many frames
					// = AI/bot indecision (re-issuing conflicting commands). Only counts when BOTH
					// alternating orders are non-combat/non-harvest (those flip legitimately).
					if (oid != mv.to1) {
						if (oid == mv.to2 && mv.to1 != 255) { if (mv.flips < 255) ++mv.flips; }
						else mv.flips = 0;             // a third order breaks the A<->B cycle
						mv.to2 = mv.to1; mv.to1 = (uint8_t)oid;
						if (mv.flips >= 12 && diag_order_thrash_ok(mv.to1) && diag_order_thrash_ok(mv.to2)
						    && diag_once(17, id))
							bwgame::diag::emit(cf, "unit", DSev::warn, "ORDER_THRASH",
								"unit order alternates between two commands (AI/bot indecision)",
								"\"unit\":" + std::to_string(id) + ",\"order_a\":" + std::to_string(mv.to1) +
								",\"order_b\":" + std::to_string(mv.to2) + ",\"flips\":" + std::to_string(mv.flips));
					}
					mv.prev = pos; mv.epoch = (uint32_t)st.current_frame + 1;
				}
			}
		}
		// aiscript_stall (Cat 1b): an AI thread parked on the SAME wait_build/wait_train gate
		// for too long -- the "town making no progress" fault. Rides the engine's OWN
		// block_since tracking (bwgame.h ai_thread_t: block_pc + block_since, added for the
		// campaign terran08 Control-Tower-stuck case) instead of re-deriving. In melee the AI's
		// production manager satisfies its waits within a few seconds, so a >900-frame (30 s)
		// block is a genuine stall. De-duped per (owner, block_pc) so one stuck gate reports once.
		for (int owner = 0; owner < 8; ++owner) {
			auto& ai = st.ai_players[owner];
			if (!ai.active) continue;
			for (size_t ti = 0; ti < ai.threads.size(); ++ti) {
				auto& th = ai.threads[ti];
				// The engine's live "currently blocked" sentinel is block_pc != 0 -- on gate
				// clear it sets block_pc = 0 but NEVER resets block_since, so guarding on
				// block_since would keep firing 900f after any already-cleared wait. Guard on
				// block_pc; block_since is only meaningful while block_pc != 0.
				if (!th.active || th.block_pc == 0) continue;
				int frames_stuck = (int)st.current_frame - th.block_since;
				if (frames_stuck > 900 && diag_once(11, (uint32_t)(owner * 8 + ti) ^ (th.block_pc << 6)))
					bwgame::diag::emit(cf, "ai", DSev::warn, "AISCRIPT_STALL",
						"AI thread parked on the same wait gate for over 900 frames",
						"\"player\":" + std::to_string(owner) + ",\"thread\":" + std::to_string(ti) +
						",\"block_pc\":" + std::to_string(th.block_pc) + ",\"frames_stuck\":" + std::to_string(frames_stuck) +
						",\"town\":[" + std::to_string(th.town.x) + "," + std::to_string(th.town.y) + "]");
			}
		}
		// trig_stall (Cat 1a): a trigger stuck "one condition away" (all conditions pass but
			// exactly one) for a long time -- the campaign soft-lock. test_trigger_condition is
			// const (read-only) so re-evaluating it here cannot affect the sim (determinism-safe).
			// 30-frame cadence = the engine's own trigger poll. Keyed by (rt.t ^ player), pruned
			// each poll (only triggers still one-away survive) + on new-game. Emits the blocking
			// condition (type/group/unit/count/loc) so a stuck mission is diagnosable over adb.
			if (((uint32_t)st.current_frame % 30) == 0) {
				std::unordered_map<uint64_t, uint32_t> seen;
				for (int i : active_players()) {
					for (auto& rt : st.running_triggers[i]) {
						if ((rt.flags & 8) || (rt.flags & 1) || !rt.t) continue; // disabled / waiting mid-exec
						// Skip end-condition triggers: a Victory(1)/Defeat(2) trigger is legitimately
						// "one condition away" for the WHOLE game (waiting for the win/lose state), so
						// it is NOT a soft-lock. A real soft-lock is a PROGRESSION trigger (transmission,
						// create-unit, set-next-scenario, ...) stuck one-away. This is what keeps melee
						// (whose triggers are just victory conditions) from false-firing every game.
						bool end_cond = false;
						for (size_t ai = 0; ai < rt.t->actions.size(); ++ai) {
							int at = rt.t->actions[ai].type;
							if (at == 0) break;                 // end sentinel
							if (at == 1 || at == 2) { end_cond = true; break; }
						}
						if (end_cond) continue;
						int fails = 0, total = 0, blk = -1, blk_type = 0, blk_group = 0, blk_unit = 0, blk_count = 0, blk_loc = 0;
						for (size_t ci = 0; ci < rt.t->conditions.size(); ++ci) {
							const auto& c = rt.t->conditions[ci];
							if (c.type == 0) break;          // end sentinel
							if (c.flags & 2) continue;       // disabled in the editor
							++total;
							if (!test_trigger_condition(c, i)) {
								++fails;
								if (blk < 0) { blk = (int)ci; blk_type = c.type; blk_group = c.group;
									blk_unit = c.unit_id; blk_count = c.count_n; blk_loc = c.location; }
							}
						}
						if (total == 0 || fails != 1) continue;   // only the "one condition away" case
						uint64_t key = (uint64_t)(uintptr_t)rt.t ^ ((uint64_t)i * 0x9E3779B97F4A7C15ull);
						auto it = diag_trig_since.find(key);
						uint32_t since = (it == diag_trig_since.end()) ? (uint32_t)st.current_frame : it->second;
						seen[key] = since;
						int frames_stuck = (int)st.current_frame - (int)since;
						if (frames_stuck > 1800 && diag_once(16, (uint32_t)(key ^ (key >> 32))))
							bwgame::diag::emit(cf, "trigger", DSev::warn, "TRIG_STALL",
								"trigger one condition away, blocked >1800 frames (soft-lock candidate)",
								"\"player\":" + std::to_string(i) + ",\"blocking_cond\":" + std::to_string(blk) +
								",\"cond_type\":" + std::to_string(blk_type) + ",\"frames_stuck\":" + std::to_string(frames_stuck) +
								",\"group\":" + std::to_string(blk_group) + ",\"unit\":" + std::to_string(blk_unit) +
								",\"count\":" + std::to_string(blk_count) + ",\"loc\":" + std::to_string(blk_loc));
					}
				}
				diag_trig_since.swap(seen);   // prune triggers that fired or left the one-away state
			}
			// Prune dead-unit movement entries every 1024 frames so the map can't grow forever
		// (living units refresh epoch every frame; anything >2 frames stale is gone).
		if (((uint32_t)st.current_frame & 1023u) == 0 && !diag_mv.empty()) {
			uint32_t now = (uint32_t)st.current_frame + 1;
			for (auto it = diag_mv.begin(); it != diag_mv.end();)
				it = (now - it->second.epoch > 2) ? diag_mv.erase(it) : std::next(it);
		}
	}

	// DIAG self-test: reproduce a scenario programmatically so a behaviour can be
	// verified over adb WITHOUT a touch gesture. Flag-gated (--diag-selftest), runs
	// once at a chosen frame, never in normal play -> not determinism-relevant. It
	// creates test buildings, drives the REAL UI submit path, then the existing
	// detectors report (RALLY_GROUP_OK vs RALLY_GROUP_PARTIAL). Lets the SCBW2
	// group-rally rule be confirmed autonomously: run with --qol 0 (expect PARTIAL
	// 1/3, vanilla) vs --qol 2 (bit1 = group_rally UI-fan; expect OK 3/3).
	void run_diag_selftest(const std::string& name) {
		if (!bwgame::diag::enabled()) return;
		// Crash-handler self-test: lay down a few breadcrumb events (so the ring is
		// non-empty in the dump) then trigger a real SIGSEGV. Verifies openbw-crash.jsonl
		// gets a crash_header + backtrace + the event ring, autonomously over adb. Only
		// reachable via --diag-selftest crash; never in normal play.
		if (name == "crash") {
			for (int i = 0; i < 3; ++i)
				bwgame::diag::emit((long)st.current_frame, "selftest", bwgame::diag::Sev::warn,
					"CRASH_BREADCRUMB", "pre-crash breadcrumb " + std::to_string(i), "\"i\":" + std::to_string(i));
			bwgame::diag::emit((long)st.current_frame, "selftest", bwgame::diag::Sev::fatal,
				"CRASH_SELFTEST", "about to dereference null", "");
			volatile int* p = nullptr;
			*p = 0x1234;   // SIGSEGV -> crash_handler writes the dump, then chains to OS
			return;
		}
		xy start = game_st.start_locations[game_local_player];
		if (start == xy()) start = xy((int)game_st.map_width / 2, (int)game_st.map_height / 2);
		current_selection.clear();
		if (name == "smartcast") {
			// 3 hero Science Vessels (Magellan bypasses tech research, so it can EMP
			// with no research building), full energy, selected; then cast EMP on a
			// nearby point. smart_cast on -> only the best-energy caster spends energy;
			// off -> all of them do (SMART_CAST_ALL). Verified over adb, no touch.
			const unit_type_t* vt = get_unit_type(UnitTypes::Hero_Magellan);
			// Place the casters EQUIDISTANT (~100px) from the cast point and well inside
			// EMP range, so a GROUP cast (broken/off smart_cast) fires all three
			// simultaneously -- otherwise a staggered fly-in could leave farther vessels
			// not-yet-cast at the check and fake a "smart" 1-of-3 result.
			xy center = start + xy(0, -192);
			const xy off3[3] = { xy(0, -100), xy(-87, 50), xy(87, 50) };
			for (int i = 0; i < 3; ++i) {
				unit_t* u = trigger_create_unit(vt, center + off3[i], game_local_player);
				if (u) { u->energy = unit_max_energy(u); current_selection.push_back(get_unit_id(u)); }
			}
			bwgame::diag::emit((long)st.current_frame, "selftest", bwgame::diag::Sev::info, "SELFTEST_SETUP",
				"created " + std::to_string(current_selection.size()) + " test casters for 'smartcast'",
				"\"scenario\":\"smartcast\",\"casters\":" + std::to_string(current_selection.size()) +
				",\"smart_cast\":" + std::to_string((int)st.game->qol.smart_cast));
			if (current_selection.size() < 2) return;
			// Cast toward the MAP CENTER (far from the clustered casters + guaranteed
			// on-map): the caster flies out to cast so it HOLDS the cast order for many
			// frames, and the EMP blast lands far from the other casters so it can't
			// muddy anything. The detector reads the ORDER, not energy, so completion
			// timing / area-drain don't matter -- only which casters got the order.
			xy mapc((int)game_st.map_width / 2, (int)game_st.map_height / 2);
			int ddx = mapc.x - center.x, ddy = mapc.y - center.y;
			double dlen = std::sqrt((double)ddx * ddx + (double)ddy * ddy); if (dlen < 1) dlen = 1;
			xy pos = center + xy((int)(ddx * 384 / dlen), (int)(ddy * 384 / dlen));
			int order_id = (int)Orders::CastEMPShockwave;
			// smart_cast routes to one caster; otherwise fall back to the whole-group cast.
			if (!try_smart_cast(order_id, pos, nullptr) && submit_select_action()) {
				game_action_writer w;
				w.u8(21); w.i16((int16_t)pos.x); w.i16((int16_t)pos.y); w.u16(0);
				w.u16((uint16_t)(int)UnitTypes::None);
				w.u8((uint8_t)order_id); w.u8(0);
				submit_game_action(w);
			}
			diag_cast_units.clear();
			for (auto uid : current_selection) diag_cast_units.push_back(uid);
			diag_cast_order = order_id;
			diag_cast_check_frame = (int)st.current_frame + 6;   // order is set on issue, held while flying
			return;
		}
		if (name == "wideselect") {
			// Spawn 12 Marines + 8 Firebats (20 units, 2 types), select them all through
			// current_selection_add, then fan a move over the whole set. Verifies (a) the
			// wide set holds all 20 with 2 sub-cards while current_selection stays <=12,
			// and (b) the order fan reaches all 20 (WIDE_ORDER_ALL), not just the active
			// sub-card. Requires --qol 8 (unlimited_selection); with the flag off the
			// adders cap at 12 and the WIDE_SEL_LEAK/invariant detectors stay quiet.
			const unit_type_t* mt = get_unit_type(UnitTypes::Terran_Marine);
			const unit_type_t* ft = get_unit_type(UnitTypes::Terran_Firebat);
			int made = 0;
			for (int i = 0; i < 20; ++i) {
				const unit_type_t* t = i < 12 ? mt : ft;
				xy p = start + xy(((i % 5) - 2) * 48, ((i / 5) - 2) * 48 - 64);
				unit_t* u = trigger_create_unit(t, p, game_local_player);
				if (u) { current_selection_add(u); ++made; }
			}
			bwgame::diag::emit((long)st.current_frame, "selftest", bwgame::diag::Sev::info, "SELFTEST_SETUP",
				"created " + std::to_string(made) + " units for 'wideselect'",
				"\"scenario\":\"wideselect\",\"made\":" + std::to_string(made) +
				",\"unlimited\":" + std::to_string((int)st.game->qol.unlimited_selection) +
				",\"wide\":" + std::to_string(wide_selection.size()) +
				",\"page_sel\":" + std::to_string(current_selection.size()) +
				",\"pages\":" + std::to_string(wide_page_count()));
			// Fan a move to a far point over the whole selection (active page + the rest).
			xy movepos = start + xy(480, 480);
			diag_wide_units.clear();
			for (auto uid : wide_selection) diag_wide_units.push_back(uid);
			if (diag_wide_units.empty())
				for (auto uid : current_selection) diag_wide_units.push_back(uid);
			emit_order_to_selection([&](game_action_writer& w) {
				w.u8(20); w.i16((int16_t)movepos.x); w.i16((int16_t)movepos.y);
				w.u16(0); w.u16((uint16_t)(int)UnitTypes::None); w.u8(0);
			});
			diag_wide_order = (int)Orders::Move;
			diag_wide_check_frame = (int)st.current_frame + 8;
			return;
		}
		// trigger_create_unit checks can_place_building, so barracks (4x3 tiles) need
		// non-overlapping buildable tiles. Search a grid around the start (192px steps)
		// and keep the first 3 that place, so the scenario is map-robust.
		const unit_type_t* rax = get_unit_type(UnitTypes::Terran_Barracks);
		for (int gy = -3; gy <= 3 && current_selection.size() < 3; ++gy) {
			for (int gx = -4; gx <= 4 && current_selection.size() < 3; ++gx) {
				if (gx == 0 && gy == 0) continue; // leave the start depot spot clear
				unit_t* u = trigger_create_unit(rax, start + xy(gx * 192, gy * 192), game_local_player);
				if (u) current_selection.push_back(get_unit_id(u));
			}
		}
		bwgame::diag::emit((long)st.current_frame, "selftest", bwgame::diag::Sev::info, "SELFTEST_SETUP",
			"created " + std::to_string(current_selection.size()) + " test barracks for '" + name + "'",
			"\"scenario\":" + bwgame::diag::jval(name) + ",\"buildings\":" + std::to_string(current_selection.size()) +
			",\"multiselect\":" + std::to_string((int)st.game->qol.multi_building_select));
		if (current_selection.size() < 2) return;
		if (name == "grouprally") {
			xy pos = start + xy(0, 96);
			// group_rally (UI fan) handles it if on; else fall back to the single
			// opcode-21 path, which rallies all only when multi_building_select is on.
			if (!try_group_rally(pos, nullptr) && submit_select_action()) {
				game_action_writer w;
				w.u8(21); w.i16((int16_t)pos.x); w.i16((int16_t)pos.y); w.u16(0);
				w.u16((uint16_t)(int)UnitTypes::None);
				w.u8((uint8_t)(int)Orders::RallyPointTile); w.u8(0);
				submit_game_action(w);
				diag_rally_units.clear();
				for (auto uid : current_selection) diag_rally_units.push_back(uid);
				diag_rally_pos = pos; diag_rally_target_id = -1;
				diag_rally_check_frame = (int)st.current_frame + 4;
			}
		}
	}

	bool current_selection_is_selected(unit_t* u) {
		auto uid = get_unit_id(u);
		return std::find(current_selection.begin(), current_selection.end(), uid) != current_selection.end();
	}

	void current_selection_add(unit_t* u) {
		// SCBW2 unlimited_selection: grow the wide set (dedup, capped) and recompute the
		// active <=12 sub-card. current_selection still ends up <=12.
		if (unlimited_sel_on()) {
			auto uid = get_unit_id(u);
			if (wide_selection.size() >= wide_selection_cap) return;
			if (std::find(wide_selection.begin(), wide_selection.end(), uid) != wide_selection.end()) return;
			wide_selection.push_back(uid);
			rebuild_active_page();
			return;
		}
		// BW hard-caps a selection at 12 (the sim errors past it, actions.h:283).
		// The UI adders (rect/band-select, double-tap same-type, add-to-group) must
		// honour that cap here, or the visual selection exceeds what the engine will
		// control -- you could box-select every visible unit but command only 12.
		if (current_selection.size() >= 12) return;
		auto uid = get_unit_id(u);
		if (std::find(current_selection.begin(), current_selection.end(), uid) != current_selection.end()) return;
		current_selection.push_back(uid);
	}

	void current_selection_clear() {
		current_selection.clear();
		wide_selection.clear();   // SCBW2 unlimited_selection: drop the wide set + reset paging
		sel_page = 0;
		focused_unit_id = unit_id{}; // SCBW2 group-focus: drop the card/portrait focus
		// A changed selection cancels any armed cursor and returns the command
		// card to its context page.
		attack_armed = false;
		ability_armed_order = -1;
		card_menu = 0;
		card_hover_index = -1;
		// Landing / Nydus-exit placement belongs to a specific building; drop it
		// on reselect.
		if (placing_land || placing_self) { placing_type = nullptr; placing_order = nullptr; placing_land = false; placing_self = false; }
	}

	void current_selection_remove(const unit_t* u) {
		auto uid = get_unit_id(u);
		if (unlimited_sel_on()) {
			auto wi = std::find(wide_selection.begin(), wide_selection.end(), uid);
			if (wi != wide_selection.end()) wide_selection.erase(wi);
			rebuild_active_page();
			return;
		}
		auto i = std::find(current_selection.begin(), current_selection.end(), uid);
		if (i != current_selection.end()) current_selection.erase(i);
	}

	// Interactive (melee) mode: when set, a touch long-press issues the
	// contextual order (move/attack/gather - what right-click does in the
	// original game) for the currently selected own units.
	bool game_input = false;
	int game_local_player = -1;

	// HUD text is drawn from the Unispace Bold coverage atlas (font_unispace.h,
	// rasterized offline). The atlas is sized for text_scale 2 (drawn 1:1);
	// other scales nearest-neighbor upscale by text_scale/2. Coverage acts as
	// alpha, blended over the game with a drop shadow for readability.
	void draw_text_rgba(uint32_t* data, size_t pitch, int x, int y, const char* s, uint32_t color, int text_scale = 2) {
		draw_text_rgba_scaled(data, pitch, x, y, s, color, text_scale * 50);
	}
	// Percentage scale: 100 = the atlas's native size (text_scale 2). Needed
	// because text_scale is an integer and the Unit Display wants 80%.
	void draw_text_rgba_scaled(uint32_t* data, size_t pitch, int x, int y, const char* s, uint32_t color, int pct) {
		const int W = ui::unispace_w, H = ui::unispace_h;
		// Faze 4.3: in-game HUD text is authored for the HD2 buffer; at FULL HD the
		// buffer is world_scale x bigger so the glyphs read as tiny. hud_scale_text
		// (set only while drawing the in-game HUD) scales them to match; menus stay
		// as-is. No-op at SD/HD2 (hud_us()==1). Layout math in the HUD uses
		// usp_w()/usp_h() so centering/right-anchoring stays consistent.
		if (hud_scale_text) pct = hud_text_pct(pct); // hud_us() + pinch-zoom factor
		float f = pct / 100.0f;
		// open1161HD: draw the glyph BODY at an INTEGER scale so the bitmap font
		// upsamples 1:1 (crisp). At a fractional scale the old `ox * W / ow` nearest
		// tap duplicates/drops source rows and columns unevenly -- that stepping is
		// the "blurry, low-quality" in-game HUD text. The advance stays at the
		// fractional target, so the text block keeps its intended width/centering
		// (the per-glyph body/advance difference is at most ~1px). Menu text is drawn
		// at pct=100 (f=1) so it was already crisp and is unchanged.
		int gs = (int)(f + 0.5f); if (gs < 1) gs = 1; // integer glyph scale
		int ow = W * gs, oh = H * gs;
		int adv = (int)((W + 1) * f + 0.5f);
		int cr = (color >> 16) & 0xff, cg = (color >> 8) & 0xff, cb = color & 0xff;
		int cx = x;
		for (; *s; ++s) {
			unsigned char ch = (unsigned char)*s;
			if (ch < ui::unispace_first || ch >= ui::unispace_first + ui::unispace_count) { cx += adv; continue; }
			const unsigned char* gl = ui::unispace_data + (size_t)(ch - ui::unispace_first) * H * W;
			for (int oy = 0; oy != oh; ++oy) {
				int row = oy / gs;
				int py = y + oy;
				if (py < 0 || py + 1 >= (int)screen_height) continue;
				const unsigned char* grow = gl + row * W;
				for (int ox = 0; ox != ow; ++ox) {
					int cov = grow[ox / gs];
					if (cov < 24) continue;
					int px = cx + ox;
					if (px < 0 || px + 1 >= (int)screen_width) continue;
					if (cov >= 96) data[(size_t)(py + 1) * pitch + (px + 1)] = 0xff000000u;
					uint32_t bg = data[(size_t)py * pitch + px];
					int br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff, bb = bg & 0xff;
					int rr = (cr * cov + br * (255 - cov)) / 255;
					int rg = (cg * cov + bgc * (255 - cov)) / 255;
					int rb = (cb * cov + bb * (255 - cov)) / 255;
					data[(size_t)py * pitch + px] = 0xff000000u | (rr << 16) | (rg << 8) | (uint32_t)rb;
				}
			}
			cx += adv;
		}
		// hud_text_clip (Cat 3a, DIAG): flag in-game HUD text that renders past the render
		// surface (start on-screen, end clipped off). Scoped to diag_in_hud -- set ONLY inside
		// draw_hud2 proper, NOT the broader hud_scale_text (which is also on for briefing/pause/
		// tooltip screens that legitimately pin text to the edge). Read-only; de-duped per
		// (position bucket, side) so a persistent clip reports once. right edge = the LAST
		// glyph cell's right (cx-adv+ow), not the post-advance pen -- on the FULL-HD scaled path
		// (f>1) the trailing inter-glyph advance overshoots by ~f+1 px and would false-clip.
		if (diag_in_hud && cx != x && bwgame::diag::enabled()) {
			int sw = (int)screen_width, sh = (int)screen_height;
			const int EPS = 2;                       // antialias/rounding bleed tolerance
			int right = cx - adv + ow, bottom = y + oh;
			bool clip_r = x >= 0 && right > sw + EPS;
			bool clip_b = y >= 0 && bottom > sh + EPS;
			bool clip_l = x < -EPS, clip_t = y < -EPS;
			if (clip_r || clip_b || clip_l || clip_t) {
				int side_i = clip_l ? 0 : clip_t ? 1 : clip_b ? 2 : 3;
				uint32_t key = ((uint32_t)(y & 0x3fff) << 16) | ((uint32_t)(x & 0x3fff) << 2) | (uint32_t)side_i;
				if (diag_once(12, key)) {
					const char* side = clip_l ? "left" : clip_t ? "top" : clip_b ? "bottom" : "right";
					bwgame::diag::emit((long)st.current_frame, "hud", bwgame::diag::Sev::warn,
						"HUD_TEXT_CLIP", "in-game HUD text renders past the surface edge",
						"\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
						",\"right\":" + std::to_string(right) + ",\"bottom\":" + std::to_string(bottom) +
						",\"surf\":[" + std::to_string(sw) + "," + std::to_string(sh) + "],\"side\":\"" + side + "\"");
				}
			}
		}
	}

	void fill_rect_rgba(uint32_t* data, size_t pitch, rect r, uint32_t color) {
		if (r.from.x < 0) r.from.x = 0;
		if (r.from.y < 0) r.from.y = 0;
		if (r.to.x > (int)screen_width) r.to.x = (int)screen_width;
		if (r.to.y > (int)screen_height) r.to.y = (int)screen_height;
		for (int y = r.from.y; y < r.to.y; ++y) {
			uint32_t* row = data + (size_t)y * pitch;
			for (int x = r.from.x; x < r.to.x; ++x) row[x] = color;
		}
	}

	// Alpha-blended rectangle fill (fill_rect_rgba writes the color straight,
	// ignoring alpha). alpha 0..255 = source weight. Used for translucent
	// overlays like the Protoss psi field so the terrain stays visible.
	void blend_rect_rgba(uint32_t* data, size_t pitch, rect r, uint32_t color, int alpha) {
		if (r.from.x < 0) r.from.x = 0;
		if (r.from.y < 0) r.from.y = 0;
		if (r.to.x > (int)screen_width) r.to.x = (int)screen_width;
		if (r.to.y > (int)screen_height) r.to.y = (int)screen_height;
		if (alpha < 0) alpha = 0; if (alpha > 255) alpha = 255;
		int ia = 255 - alpha;
		uint32_t sr = (color >> 16) & 0xff, sg = (color >> 8) & 0xff, sb = color & 0xff;
		for (int y = r.from.y; y < r.to.y; ++y) {
			uint32_t* row = data + (size_t)y * pitch;
			for (int x = r.from.x; x < r.to.x; ++x) {
				uint32_t d = row[x];
				uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
				row[x] = 0xff000000u
					| (((sr * alpha + dr * ia) / 255) << 16)
					| (((sg * alpha + dg * ia) / 255) << 8)
					| ((sb * alpha + db * ia) / 255);
			}
		}
	}

	// LAN lobby: while set, the game is not rendered - just a dark screen
	// with the status line, so the host waits in a lobby instead of on the
	// loaded map.
	a_string lobby_status;
	bool lobby_mode = false;
	// v0.48: when the launcher already ran the whole lobby (net-fast), the native
	// side skips its slot-table lobby and shows a plain loading screen instead:
	// a progress bar bottom-centre that fills while the peers connect + the sim
	// negotiates, right before the game appears at 00:00.
	bool loading_mode = false;
	float loading_progress = 0.0f;
	rect loading_cancel_btn{};

	// --- Loading screen (melee: minimap + the two sides; campaign: mission title) ---
	// One renderer (ui/loading_screen.h, standalone so the host harness
	// tools/loadscreen_check.cpp renders the very same code) serves every loading
	// path: the asset prewarm at game start (hd_prewarm, all tiers) and the LAN
	// net-fast wait (draw_loading_screen). The fields below are the few things the
	// engine state cannot tell on its own; the glue (android_main) sets them, and
	// everything else -- players, races, colours, teams, start locations, the
	// minimap -- is read from the loaded game. Pure presentation: nothing here
	// writes sim state.
	a_string loading_map_label;                   // melee: the map's file name / path (display name)
	a_string loading_campaign_dir;                // campaign: mission dir -> the title layout
	std::array<a_string, 12> loading_slot_name;   // per-slot display-name override ("Computer", a bot's name)
	std::array<a_string, 12> loading_slot_role;   // per-slot second-line note ("Medium", "Bot")
	std::array<bool, 12> loading_slot_random{};   // the slot picked Random (its race was rolled at load)
	int loading_local_color = -1;                 // LAN: the local player's lobby colour (0..7), -1 = slot default
	// Player identity portraits (launcher redesign F4; pure presentation). The
	// launcher hands over the local profile's portdata id; LAN peers' ids arrive as
	// a name -> id list from the launcher lobby (nothing rides the lockstep stream).
	// AI slots get a unit by difficulty (same table as the launcher's player list).
	int local_portrait = -1;                              // --player-portrait
	int ai_portrait_level = -1;                           // -1 original aiscript AI / other bots, 0..3 built-in AI level
	a_vector<std::pair<a_string, int>> name_portraits;    // --name-portraits (LAN)
	struct loading_portrait_t { int w = 0, h = 0; std::shared_ptr<const std::vector<uint32_t>> px; };
	std::map<int, loading_portrait_t> loading_portrait_cache;

	// race: 0 zerg, 1 terran, 2 protoss. Returns -1 when the race is unknown.
	static int ai_portrait_for(int level, int race) {
		static const int tbl[5][3] = {
			{38, 17, 59},   // original AI: the race advisor (Zerg advisor, Adjutant, Aldaris)
			{24, 7, 39},    // very easy: Drone, SCV, Probe
			{20, 0, 40},    // easy: Zergling, Marine, Zealot
			{21, 1, 41},    // medium: Hydralisk, Ghost, Dragoon
			{22, 11, 47},   // hard: Ultralisk, Battlecruiser, Carrier
		};
		if (race < 0 || race > 2) return -1;
		return tbl[(level < 0 || level > 3) ? 0 : level + 1][race];
	}

	// The portdata id shown for a player. `race` is what may be SHOWN (-1 = hidden /
	// random): a portrait picked by the race must not give a hidden race away.
	int player_portrait_id(bool is_local, bool is_ai, const a_string& name, int race) const {
		if (is_local && local_portrait >= 0) return local_portrait;
		if (!is_ai) for (auto& v : name_portraits) if (v.first == name && v.second >= 0) return v.second;
		return ai_portrait_for(is_ai ? ai_portrait_level : -1, race);
	}

	// First frame of the idle clip, 0x00RRGGBB. Decoded once per id (a few 100 us).
	const loading_portrait_t& loading_portrait(int id) {
		auto it = loading_portrait_cache.find(id);
		if (it != loading_portrait_cache.end()) return it->second;
		loading_portrait_t& r = loading_portrait_cache[id];
		try {
			if (!portraits_loaded) load_portraits();
			if (id < 0 || (size_t)id >= portrait_record_count()) return r;
			a_string fn = portraits_tbl[portrait_smk_index[(size_t)id]];
			if (fn.empty()) return r;
			a_string path = "portrait\\" + fn + "0.smk";
			if (data_file_exists && !data_file_exists(path)) path = "portrait\\" + fn + ".smk";
			a_vector<uint8_t> data;
			load_data_file(data, path);
			smk s = smk_open_memory(data.data(), (unsigned long)data.size());
			if (!s) return r;
			unsigned long w = 0, h = 0; unsigned char ys = 0;
			smk_info_video(s, &w, &h, &ys);
			smk_enable_video(s, 1);
			smk_first(s);
			const unsigned char* pal = smk_get_palette(s);
			const unsigned char* vid = smk_get_video(s);
			if (pal && vid && w > 0 && h > 0 && w <= 512 && h <= 512) {
				auto px = std::make_shared<std::vector<uint32_t>>((size_t)w * h);
				for (size_t i = 0; i != (size_t)w * h; ++i) {
					const unsigned char* c = pal + (size_t)vid[i] * 3;
					(*px)[i] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
				}
				r.w = (int)w; r.h = (int)h; r.px = std::move(px);
			}
			smk_close(s);
		} catch (...) {
			r = loading_portrait_t();
		}
		return r;
	}

	void loading_attach_portrait(loading_screen::player_t& r, int id) {
		if (!portraits_loaded) { try { load_portraits(); } catch (...) {} }
		if (portrait_smk_index.empty()) return;        // no portrait data at all: keep the colour badge
		r.portrait_tile = true;
		r.portrait_id = id;
		if (id < 0) return;                            // hidden / unknown race: "no signal" tile
		const loading_portrait_t& lp = loading_portrait(id);
		r.portrait_w = lp.w; r.portrait_h = lp.h; r.portrait = lp.px;
	}
	loading_screen::info_t loading_info;
	loading_screen::cache_t loading_cache;
	bool loading_shown_once = false;              // first loading screen of this game already displayed
	bool loading_first_frame_done = false;        // see loading_cover_first_frame
	float loading_last_progress = 0.0f;

	static std::string loading_str(const a_string& s) { return std::string(s.data(), s.size()); }

	// Backdrop shared by the loading screen and the mission briefing: the same
	// picture the launcher shows behind its panels (glue\palmm\backgnd.pcx, the
	// main-menu space scene, from the user's own MPQ -- PortraitStore.background on
	// the Java side). Decoded once; a missing file or any error leaves it null and
	// both screens fall back to the renderer's dark gradient. Pure presentation.
	bool ui_backdrop_enabled = true;   // harness switch: render the no-game-art fallback
	bool ui_backdrop_tried = false;
	std::shared_ptr<const std::vector<uint32_t>> ui_backdrop_px;
	int ui_backdrop_w = 0, ui_backdrop_h = 0;
	void ui_backdrop_load() {
		if (ui_backdrop_tried) return;
		ui_backdrop_tried = true;
		if (!load_data_file) return;
		try {
			a_string path = "glue\\palmm\\backgnd.pcx";
			if (data_file_exists && !data_file_exists(path)) return;
			a_vector<uint8_t> bytes;
			load_data_file(bytes, path);
			auto px = std::make_shared<std::vector<uint32_t>>();
			int w = 0, h = 0;
			if (loading_screen::decode_pcx8(bytes.data(), bytes.size(), *px, w, h)) {
				ui_backdrop_w = w; ui_backdrop_h = h;
				ui_backdrop_px = std::move(px);
			}
		} catch (...) {
			ui_backdrop_px.reset(); ui_backdrop_w = ui_backdrop_h = 0;
		}
	}
	void ui_backdrop_apply(loading_screen::style_t& s) {
		if (!ui_backdrop_enabled) return;
		ui_backdrop_load();
		s.backdrop = ui_backdrop_px; s.backdrop_w = ui_backdrop_w; s.backdrop_h = ui_backdrop_h;
	}

	// The loading screen wears the launcher's palette (loading_screen::pal), not a
	// race tint: one look from the launcher through loading into the briefing.
	loading_screen::style_t loading_style() {
		loading_screen::style_t s;
		ui_backdrop_apply(s);
		s.anim_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
		s.s_ffa = L("FREE FOR ALL"); s.s_team = L("TEAM"); s.s_random = L("Random");
		s.cancel_label = L("CANCEL");
		return s;
	}

	uint32_t loading_player_rgb(int color_index) const {
		if (color_index >= 0 && color_index < 8) return loading_screen::bw_player_color(color_index);
		// Map-defined colours past the eight standard ones: through the minimap palette.
		if (color_index >= 0 && (size_t)color_index < img.player_minimap_colors.size())
			return game_palette[img.player_minimap_colors[(size_t)color_index]];
		return 0x808080u;
	}

	// Terrain minimap (one pixel per tile) + resources, straight from the loaded
	// map. Same per-tile colour lookup as draw_minimap_into, minus fog/creep/units:
	// this is the map, not the game in progress. Any missing data just leaves the
	// minimap empty (the renderer then draws a blank plate).
	void loading_build_minimap(loading_screen::info_t& info) {
		// minimap_rev keys the renderer's cached composition: bump it only when the
		// pixels really change, so a rebuild of the same map stays a cache hit.
		std::vector<uint32_t> old = std::move(info.minimap);
		info.minimap.clear(); info.minimap_w = info.minimap_h = 0;
		if (!old.empty()) ++info.minimap_rev;
		size_t mtw = game_st.map_tile_width, mth = game_st.map_tile_height;
		if (mtw == 0 || mth == 0 || st.tiles_mega_tile_index.size() < mtw * mth) return;
		try {
			std::vector<uint32_t> px(mtw * mth, 0);
			for (size_t i = 0; i != mtw * mth; ++i) {
				size_t index = st.tiles_mega_tile_index[i];
				auto* images = &tileset_img.vx4.at(index).images[0];
				auto* bitmap = &tileset_img.vr4.at(*images / 2).bitmap[0];
				auto val = bitmap[55 / sizeof(vr4_entry::bitmap_t)];
				val >>= 8 * (55 % sizeof(vr4_entry::bitmap_t));
				px[i] = game_palette[(uint8_t)val];
			}
			uint32_t res = game_palette[(uint8_t)tileset_img.resource_minimap_color];
			for (size_t i = 0; i != 12; ++i) {
				for (unit_t* u : ptr(st.player_units[i])) {
					if (!u->unit_type || !u->sprite) continue;
					if (!unit_is_mineral_field(u) && !unit_is(u, UnitTypes::Resource_Vespene_Geyser)) continue;
					int w = (int)u->unit_type->placement_size.x / 32, h = (int)u->unit_type->placement_size.y / 32;
					if (w < 2) w = 2;
					if (h < 1) h = 1;
					xy from = (u->sprite->position - u->unit_type->placement_size / 2) / 32;
					for (int y = from.y; y < from.y + h; ++y) for (int x = from.x; x < from.x + w; ++x) {
						if (x < 0 || y < 0 || (size_t)x >= mtw || (size_t)y >= mth) continue;
						px[(size_t)y * mtw + (size_t)x] = res;
					}
				}
			}
			if (!old.empty() && px == old) --info.minimap_rev; // unchanged
			else if (old.empty()) ++info.minimap_rev;
			info.minimap = std::move(px);
			info.minimap_w = (int)mtw; info.minimap_h = (int)mth;
		} catch (const std::exception&) {
			info.minimap.clear(); info.minimap_w = info.minimap_h = 0;
		}
	}

	void loading_fill_map_text(loading_screen::info_t& info, const a_string& label_path) {
		namespace ls = loading_screen;
		std::string fallback = ls::ascii_text(loading_str(game_st.scenario_name), L("Unknown map"));
		info.map_name = label_path.empty() ? fallback : ls::ascii_text(ls::map_label_from_path(loading_str(label_path)), fallback);
		info.map_detail.clear();
		if (game_st.map_tile_width && game_st.map_tile_height) {
			info.map_detail = std::to_string(game_st.map_tile_width) + " x " + std::to_string(game_st.map_tile_height);
			const char* ts = ls::tileset_label((int)game_st.tileset_index);
			if (ts[0]) info.map_detail += std::string("  -  ") + ts;
		}
	}

	// A slot the engine plays: an aiscript computer, or (single player) any occupied
	// slot that is not ours -- that is where a bot .so sits.
	bool player_is_ai(size_t p, int local_player) const {
		if (st.players[p].controller == player_t::controller_computer_game
			|| st.players[p].controller == player_t::controller_computer) return true;
		return !is_net_game && local_player >= 0 && (int)p != local_player;
	}

	// Display name of a slot outside the loading screen (score file): empty when
	// nothing better than "Player N" is known.
	a_string player_display_name(size_t p, int local_player) const {
		a_string nm = loading_slot_name[p];
		if (nm.empty() && (int)p == local_player) nm = local_player_name;
		if (nm.empty()) nm = slot_player_names[p];
		return nm;
	}

	// Single-player (melee / UMS / campaign) info, from the loaded game.
	void loading_build_info_from_state(int local_player) {
		namespace ls = loading_screen;
		{
			// Start from a clean info, but hand the previous minimap pixels to
			// loading_build_minimap so an unchanged map keeps its cache key.
			ls::info_t fresh;
			fresh.minimap_rev = loading_info.minimap_rev;
			fresh.minimap = std::move(loading_info.minimap);
			loading_info = std::move(fresh);
		}
		auto& info = loading_info;
		if (!loading_campaign_dir.empty()) {
			info.minimap.clear();
			ls::campaign_words_t w;
			w.episode = L("EPISODE"); w.mission = L("MISSION"); w.campaign = L("CAMPAIGN");
			w.tutorial = L("TUTORIAL"); w.secret = L("SECRET MISSION");
			ls::build_campaign_info(loading_str(loading_campaign_dir), loading_str(game_st.scenario_name), info, w);
			return;
		}
		info.kind = ls::info_t::melee;
		loading_build_minimap(info);
		loading_fill_map_text(info, loading_map_label);
		// Listed players: every occupied / computer slot. Team = allies by MUTUAL alliance.
		std::array<int, 12> team_of; team_of.fill(-1);
		auto listed = [&](size_t p) {
			auto c = st.players[p].controller;
			return c == player_t::controller_occupied || c == player_t::controller_computer_game;
		};
		for (size_t p = 0; p != 8; ++p) {
			if (!listed(p)) continue;
			team_of[p] = (int)p;
			for (size_t q = 0; q != p; ++q) {
				if (team_of[q] < 0) continue;
				if (st.alliances[p][q] && st.alliances[q][p]) { team_of[p] = team_of[q]; break; }
			}
		}
		int local_team = (local_player >= 0 && local_player < 8) ? team_of[(size_t)local_player] : -1;
		// Design decision: the minimap shows the MAP only -- no start-location markers,
		// not even our own (a marker on a 2-start map gives the enemy away too).
		info.markers.clear();
		auto add = [&](size_t p) {
			ls::player_t r;
			const bool is_local = (int)p == local_player;
			const bool computer = st.players[p].controller == player_t::controller_computer_game;
			std::string fallback = is_local ? std::string(L("Player"))
				: computer ? std::string(L("Computer")) : std::string(L("Player")) + " " + std::to_string(p + 1);
			a_string nm = loading_slot_name[p];
			if (nm.empty() && is_local) nm = local_player_name;
			if (nm.empty()) nm = slot_player_names[p];
			if (nm.empty() && local_player < 0) nm = replay_st.player_name[p];   // replay: names from the .rep header
			r.name = ls::ascii_text(loading_str(nm), fallback);
			r.role = ls::ascii_text(loading_str(loading_slot_role[p]));
			r.race = (int)st.players[p].race;
			if (r.race < 0 || r.race > 2) r.race = -1;
			if (loading_slot_random[p]) {
				// Random: your own side sees what was rolled; the enemy's stays hidden.
				if (team_of[p] == local_team) r.race_random = true;
				else r.race = -1;
			}
			r.color = loading_player_rgb(st.players[p].color);
			r.team = team_of[p];
			r.is_local = is_local;
			// r.race is already -1 for an enemy that picked Random, so its portrait hides too.
			loading_attach_portrait(r, player_portrait_id(is_local, player_is_ai(p, local_player), nm, r.race));
			info.players.push_back(std::move(r));
		};
		if (local_player >= 0 && local_player < 8 && listed((size_t)local_player)) add((size_t)local_player);
		for (size_t p = 0; p != 8; ++p) if (listed(p) && (int)p != local_player) add(p);
	}

	// LAN net-fast wait: the roster is the lobby slot table (it fills in as peers
	// connect). Slots are still provisional here, so no start marker is coloured.
	void loading_build_info_from_lobby() {
		namespace ls = loading_screen;
		auto& info = loading_info;
		const bool fresh = info.kind != ls::info_t::melee
			|| info.minimap_w != (int)game_st.map_tile_width || info.minimap_h != (int)game_st.map_tile_height;
		if (fresh) {
			uint32_t rev = info.minimap_rev;
			info = ls::info_t();
			info.minimap_rev = rev;
			info.kind = ls::info_t::melee;
			loading_build_minimap(info);
			loading_fill_map_text(info, loading_map_label.empty() ? lobby_map_name : loading_map_label);
		}
		info.players.clear();
		auto add = [&](const lobby_slot_t& s) {
			ls::player_t r;
			r.name = ls::ascii_text(loading_str(s.name), std::string(L("Player")) + " " + std::to_string(s.slot + 1));
			r.race = (s.race >= 0 && s.race <= 2) ? s.race : -1;
			int col = (s.slot >= 0 && s.slot < 12) ? st.players[(size_t)s.slot].color : -1;
			if (s.is_local && loading_local_color >= 0 && loading_local_color < 16) col = loading_local_color;
			r.color = loading_player_rgb(col);
			r.team = s.slot; // LAN games are free-for-all (teams are not synced yet)
			r.is_local = s.is_local;
			loading_attach_portrait(r, player_portrait_id(s.is_local, false, s.name, r.race));
			info.players.push_back(std::move(r));
		};
		for (auto& s : lobby_slots) if (s.is_local) add(s);
		for (auto& s : lobby_slots) if (!s.is_local) add(s);
	}

	// LAN net-fast loading screen: the shared layout + the connect status line and
	// the cancel guard (a stuck wait -- host nobody joins, wrong Wi-Fi -- can always
	// be aborted back to the launcher instead of killing the app).
	void draw_loading_screen(uint32_t* data, size_t pitch) {
		loading_build_info_from_lobby();
		loading_screen::style_t s = loading_style();
		s.cancel = true;
		s.status = loading_screen::ascii_text(loading_str(lobby_status));
		loading_screen::rect_t cr;
		loading_screen::draw(data, pitch, (int)screen_width, (int)screen_height, loading_info, loading_progress, s, &loading_cache, &cr);
		loading_cancel_btn = {{cr.x0, cr.y0}, {cr.x1, cr.y1}};
	}

	// Game-start loading screen (all tiers), shown by hd_prewarm(). Everything
	// sizes off screen_height so it reads the same on SD/HD2/FULL HD. `status` is
	// deliberately not drawn (user request: no "Loading ..." text, just the bar).
	void draw_hd_loading_screen(uint32_t* data, size_t pitch, float progress, const char* status) {
		(void)status;
		loading_last_progress = progress;
		loading_screen::draw(data, pitch, (int)screen_width, (int)screen_height, loading_info, progress, loading_style(), &loading_cache);
	}

	// The very first rendered frame of a single-player game would otherwise be one
	// frame of the map / briefing backdrop BEFORE the loading screen (the glue can
	// only start the prewarm after the first update created the surfaces). Cover
	// exactly that one frame with the loading screen. One frame only, by
	// construction -- it can never get stuck over a running game.
	void loading_cover_first_frame(uint32_t* data, size_t pitch) {
		if (loading_first_frame_done) return;
		loading_first_frame_done = true;
		const bool replay = !game_input && replay_st.end_frame != 0;
		if (lobby_mode || is_net_game) return;
		if (!replay && (!game_input || game_local_player < 0)) return;
		loading_build_info_from_state(replay ? -1 : game_local_player);
		draw_hd_loading_screen(data, pitch, 0.0f, "");
	}

	// Faze 4.7 (user request): warm the HD assets the first gameplay frames need --
	// the local player's starting-unit portraits (the ~250 ms bg VP9 decode that
	// otherwise flashes a black slot on first selection) plus the race console
	// frames -- behind the loading screen, so the game opens already decoded.
	// Bounded (per-portrait and total time caps) so it can never hang the launch.
	// No-op without CASC HD, a window, or a local player (replays/observers).
	// Faze 4.7: kick a portrait clip decode on its OWN dedicated slot, so it is NOT
	// cancelled by the next warm (the shared hd_portrait_slot + close_portrait would
	// cancel a still-running idle decode the instant the talk warm started -- which is
	// why briefing idle clips kept coming back uncached and the heads froze). Returns
	// the slot to poll, or null if there is no webm for this clip.
	std::shared_ptr<hd_portrait_slot_t> hd_kick_warm(int portrait_id, bool talking) {
		size_t half = portrait_record_count();
		size_t record = talking ? (size_t)portrait_id + half : (size_t)portrait_id;
		if (record >= portrait_smk_index.size()) return nullptr;
		a_string fn = portraits_tbl[portrait_smk_index[record]];
		if (fn.empty()) return nullptr;
		a_string name = hd_portrait_webm_path(fn);
		std::vector<uint8_t> bytes;
		if (!load_hd_bytes || !load_hd_bytes(name, bytes) || bytes.empty()) return nullptr;
		auto slot = std::make_shared<hd_portrait_slot_t>();
		std::thread([slot, bytes = std::move(bytes)]() {
			webm::file_t doc = webm::parse(bytes.data(), bytes.size());
			if (!doc.ok || doc.frames.empty()) return;
			auto clip = std::make_shared<hd_portrait_clip>();
			int shown = 0, w = 0, h = 0;
			vp9::decode_sequence_hash(bytes.data(), bytes.size(), doc.frames, &shown,
				/*as_rgb=*/true, &clip->frames, &w, &h, &slot->cancel);
			if (slot->cancel.load() || clip->frames.empty() || w <= 0 || h <= 0) return;
			clip->w = w; clip->h = h;
			std::lock_guard<std::mutex> lk(slot->mtx);
			slot->clip = std::move(clip);
		}).detach();
		return slot;
	}

	// Game-start loading screen, every tier, replays included. The glue calls this
	// once after the first update (surfaces exist) and again when a briefing hands
	// off to the mission. It shows the loading screen (minimap + sides, or the
	// mission title) and preloads behind it:
	//   * every tier: the command-icon sheet and the portrait index (first select /
	//     first command card otherwise load them mid-game);
	//   * HD tiers: the background HD-anim prefetch is kicked for every sprite
	//     already on the map (so units do not open as SD sprites that pop to HD a
	//     few frames later), then the CASC warm (console, wireframes, portraits).
	//
	// Design decision: a SHORT minimum display time, so those preloads -- the HD-anim
	// worker in particular runs in the background and has nothing to block on --
	// get a head start, and the screen is not a one-frame flash on the SD tier.
	// 800 ms the first time, 300 ms for the briefing -> mission hand-off (already
	// warm), 400 ms for a replay; counted from first appearance, so a warm that
	// already took longer adds NO delay. Never called in a LAN game (it would stall
	// lockstep; the LAN wait has its own link-settle hold in the glue).
	void hd_prewarm(int local_player) {
		const bool replay = local_player < 0 && !game_input && replay_st.end_frame != 0;
		if (!rgba_surface || !wnd || (local_player < 0 && !replay)) { ui::log("prewarm: SKIP (no surface/window/local player)\n"); return; }
		loading_build_info_from_state(local_player);
		auto shown_at = clock.now();
		auto present = [&](float pr) {
			uint32_t* d = (uint32_t*)rgba_surface->lock();
			if (d) { draw_hd_loading_screen(d, rgba_surface->pitch / 4, pr, ""); rgba_surface->unlock(); }
			if (wnd) { wnd.update_surface(); native_window::event_t e; while (wnd.peek_message(e)) {} }
		};
		present(0.0f);
		loading_kick_anim_prefetch();
		load_cmdicons();
		if (!portraits_loaded) load_portraits();
		hd_prewarm_assets(local_player);
		const int min_ms = replay ? 400 : (loading_shown_once ? 300 : 800);
		const float from = loading_last_progress;
		const int64_t hold_from = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - shown_at).count();
		while (true) {
			int64_t el = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - shown_at).count();
			if (el >= min_ms) break;
			// ease the bar from wherever the warm left it to full over the remaining hold
			float t = min_ms > hold_from ? (float)(el - hold_from) / (float)(min_ms - hold_from) : 1.0f;
			present(from + (1.0f - from) * t);
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		present(1.0f);
		loading_shown_once = true;
		loading_cache.release(); // the cached composition is a full-screen buffer; drop it once in game
	}

	// Ask the background HD-anim worker for every sprite image that is already on
	// the map (players' starting units first, then neutrals), so they are decoded
	// while the loading screen is up. get_hd_anim only ENQUEUES (it never blocks on
	// CASC); the results are drained on the first real frame. No-op on the SD tier.
	void loading_kick_anim_prefetch() {
		if (hd_anim_dir.empty() && !hd_casc) return;
		int kicked = 0;
		for (size_t p = 0; p != 12; ++p) {
			for (unit_t* u : ptr(st.player_units[p])) {
				if (!u->sprite) continue;
				for (image_t* img : ptr(u->sprite->images)) {
					if (!img->image_type || (img->modifier != 0 && img->modifier != 1)) continue;
					get_hd_anim((int)img->image_type->id, (int)img->image_type->grp_filename_index);
					if (++kicked >= 512) return;
				}
			}
		}
	}

	void hd_prewarm_assets(int local_player) {
		ui::log("prewarm: enter lp=%d casc=%d surf=%d wnd=%d load=%d\n", local_player,
			(int)hd_casc, (int)(bool)rgba_surface, (int)(bool)wnd, (int)(bool)load_hd_bytes);
		if (!hd_casc || !rgba_surface || !wnd || !load_hd_bytes || local_player < 0) { ui::log("prewarm: SKIP (guard)\n"); return; }
		if (!portraits_loaded) load_portraits();
		if (portrait_smk_index.empty()) { ui::log("prewarm: SKIP (no portrait index)\n"); return; }
		// Distinct starting-unit portrait ids and unit-type ids (workers, town hall).
		a_vector<int> pids, utids;
		for (unit_t* u : ptr(st.player_units[local_player])) {
			if (!u->unit_type) continue;
			int id = (int)u->unit_type->id;
			bool huid = false; for (int q : utids) if (q == id) { huid = true; break; }
			if (!huid && (int)utids.size() < 24) utids.push_back(id); // ALL starting unit types
			int p = u->unit_type->portrait;
			if (p < 0 || (size_t)p >= portrait_record_count()) continue;
			bool have = false; for (int q : pids) if (q == p) { have = true; break; }
			if (!have && (int)pids.size() < 16) pids.push_back(p);
		}
		race_t race = (local_player < 12) ? st.players[local_player].race : race_t::terran;
		auto present = [&](float pr, const char* status) {
			uint32_t* d = (uint32_t*)rgba_surface->lock();
			if (d) { draw_hd_loading_screen(d, rgba_surface->pitch / 4, pr, status); rgba_surface->unlock(); }
			if (wnd) { wnd.update_surface(); native_window::event_t e; while (wnd.peek_message(e)) {} }
		};
		present(0.05f, "Loading interface");
		get_console_frame(race, 12); get_console_frame(race, 13); get_console_frame(race, 14);
		// Unit frames: force the HD wireframe + cmdicons containers to load from CASC
		// (the ~30 MB wireframe pull is the real first-selection stall) and decode the
		// starting unit types, so the first unit select / command card don't flash.
		present(0.10f, "Loading unit frames");
		load_cmdicons();
		int wbox = hud2_wireframe_box();
		for (size_t i = 0; i < utids.size(); ++i) {
			get_hd_wireframe_cells(utids[i], wbox);
			get_icon_override(utids[i]);
			present(0.10f + 0.05f * (float)(i + 1) / (utids.size() ? utids.size() : 1), "Loading unit frames");
		}
		// Warm worklist: every campaign briefing speaker's idle AND talking clip
		// FIRST (the briefing plays before gameplay, so it must not pop), then the
		// starting-unit portraits (idle) for the mission itself.
		struct warm_t { int p; bool talk; };
		a_vector<warm_t> work;
		auto add_work = [&](int p, bool tk) {
			if (p < 0 || (size_t)p >= portrait_record_count()) return;
			for (auto& w : work) if (w.p == p && w.talk == tk) return;
			work.push_back({p, tk});
		};
		for (auto& a : briefing_actions) {
			if (a.unit <= 0) continue;
			auto* ut = get_unit_type((UnitTypes)a.unit);
			if (!ut) continue;
			add_work(ut->portrait, false);
			add_work(ut->portrait, true);
		}
		// Starting units: warm IDLE for every one first (so selection is always
		// instant), then TALK (so the unit's voice response doesn't flash black).
		for (int p : pids) add_work(p, false);
		for (int p : pids) add_work(p, true);
		present(0.15f, "Loading portraits");
		int n = (int)work.size();
		ui::log("prewarm: worklist n=%d (utids=%d pids=%d)\n", n, (int)utids.size(), (int)pids.size());
		auto warm_start = clock.now();
		for (int i = 0; i < n; ++i) {
			if (std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - warm_start).count() > 8000) {
				ui::log("prewarm: TIME CAP hit at item %d/%d\n", i, n); break;
			}
			uint64_t key = (uint64_t)work[i].p * 2 + (work[i].talk ? 1 : 0);
			if (hd_portrait_cache.find(key) != hd_portrait_cache.end()) { ui::log("warm[%d] p=%d t=%d already-cached\n", i, work[i].p, (int)work[i].talk); continue; }
			auto slot = hd_kick_warm(work[i].p, work[i].talk); // dedicated slot -> no cancellation
			if (!slot) { ui::log("warm[%d] p=%d t=%d NO-WEBM\n", i, work[i].p, (int)work[i].talk); continue; }
			auto t0 = clock.now();
			std::shared_ptr<hd_portrait_clip> clip;
			while (true) {
				{ std::lock_guard<std::mutex> lk(slot->mtx); clip = slot->clip; }
				if (clip) break;
				int64_t el = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - t0).count();
				if (el > 2000) { slot->cancel.store(true); break; } // per-clip cap
				present(0.15f + 0.8f * ((float)i + (float)el / 2000.0f) / (n > 0 ? n : 1), "Loading portraits");
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
			if (clip && !clip->frames.empty()) {
				hd_portrait_cache[key] = clip;
				hd_portrait_cache_bytes += hd_clip_bytes(*clip);
				ui::log("warm[%d] p=%d t=%d CACHED %dfr\n", i, work[i].p, (int)work[i].talk, (int)clip->frames.size());
			} else {
				ui::log("warm[%d] p=%d t=%d TIMEOUT/FAIL\n", i, work[i].p, (int)work[i].talk);
			}
		}
		ui::log("prewarm: done (%dms)\n", (int)std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - warm_start).count());
		present(1.0f, "Ready");
	}
	// v0.39 LAN lobby slot table: android_main fills these each lobby frame
	// from the sync layer; the lobby screen renders them (host-authoritative
	// view — the UI never edits, only projects the current snapshot).
	struct lobby_slot_t { a_string name; int race = -1; int slot = -1; bool is_local = false; bool ready = false; };
	a_vector<lobby_slot_t> lobby_slots;
	a_string lobby_map_name;
	bool lobby_is_host = false;
	bool lobby_local_ready = false;   // v0.40 local ready toggle
	bool lobby_ready_dirty = false;   // android_main consumes -> send to sync layer
	bool lobby_leave_request = false; // Cancel/Back in lobby -> leave
	rect lobby_ready_btn{};
	rect lobby_leave_btn{};

	void draw_lobby_screen(uint32_t* data, size_t pitch) {
		int sw = (int)screen_width, sh = (int)screen_height;
		blend_rect_rgba(data, pitch, {{0, 0}, {sw, sh}}, 0x000610u, 150);
		int x = sw / 12, y = sh / 10;
		draw_text_rgba(data, pitch, x, y, lobby_is_host ? "LAN LOBBY  (HOST)" : "LAN LOBBY", 0xffffd040u, 5);
		y += ui::unispace_h * 5 + sh / 30;
		if (!lobby_map_name.empty()) {
			draw_text_rgba(data, pitch, x, y, (a_string("Zemelapis:  ") + lobby_map_name).c_str(), 0xffc8d8e8u, 2);
			y += ui::unispace_h * 2 + sh / 50;
		}
		if (!lobby_status.empty()) {
			draw_text_rgba(data, pitch, x, y, lobby_status.c_str(), 0xff70c8ffu, 2);
			y += ui::unispace_h * 2 + sh / 30;
		}
		static const char* rn[] = {"Zerg", "Terran", "Protoss", "?", "?", "Random"};
		draw_text_rgba(data, pitch, x, y, "--- Zaidejai ---", 0xff90c8e0u, 2);
		y += ui::unispace_h * 2 + 10;
		for (auto& s : lobby_slots) {
			uint32_t col = s.is_local ? 0xffffe090u : 0xffe0e0e0u;
			const char* race = (s.race >= 0 && s.race < 6) ? rn[s.race] : "?";
			char buf[192];
			snprintf(buf, sizeof(buf), "Slot %d   %s   [%s]%s%s",
				s.slot + 1, s.name.c_str(), race, s.is_local ? "  (tu)" : "", s.ready ? "   * READY *" : "");
			draw_text_rgba(data, pitch, x, y, buf, col, 3);
			y += ui::unispace_h * 3 + 10;
		}
		// READY toggle + LEAVE buttons along the bottom.
		int bw = sw / 4, bh = sh / 12, by = sh - bh - sh / 12;
		lobby_ready_btn = {{x, by}, {x + bw, by + bh}};
		fill_rect_rgba(data, pitch, lobby_ready_btn, lobby_local_ready ? 0xff206030u : 0xff403018u);
		line_rectangle_rgba(data, pitch, lobby_ready_btn, lobby_local_ready ? 0xff60d080u : 0xffb0a040u);
		{
			const char* rt = lobby_local_ready ? "PASIRUOSES" : "PASIRUOSTI";
			int tw = (int)strlen(rt) * (ui::unispace_w + 1) * 3;
			draw_text_rgba(data, pitch, lobby_ready_btn.from.x + (bw - tw) / 2, by + (bh - ui::unispace_h * 3) / 2, rt, 0xffffffffu, 3);
		}
		lobby_leave_btn = {{x + bw + sw / 30, by}, {x + bw + sw / 30 + bw, by + bh}};
		fill_rect_rgba(data, pitch, lobby_leave_btn, 0xff4a2020u);
		line_rectangle_rgba(data, pitch, lobby_leave_btn, 0xffc06060u);
		{
			const char* lt = "PALIKTI";
			int tw = (int)strlen(lt) * (ui::unispace_w + 1) * 3;
			draw_text_rgba(data, pitch, lobby_leave_btn.from.x + (bw - tw) / 2, by + (bh - ui::unispace_h * 3) / 2, lt, 0xffffd0d0u, 3);
		}
	}

	// Handle a tap on the lobby screen (ready toggle / leave). Returns true if
	// the tap hit a lobby control.
	bool lobby_tap(int mx, int my) {
		auto hit = [&](const rect& r) { return mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y; };
		if (loading_mode) {
			if (hit(loading_cancel_btn)) { lobby_leave_request = true; return true; }
			return false;
		}
		if (hit(lobby_ready_btn)) {
			lobby_local_ready = !lobby_local_ready;
			lobby_ready_dirty = true;
			return true;
		}
		if (hit(lobby_leave_btn)) {
			lobby_leave_request = true;
			return true;
		}
		return false;
	}

	// --- Command-card icons (v0.7) --------------------------------------
	// cmdicons.grp holds the authentic 3x3 command-button icons. It uses its
	// own palette (ticon.pcx), not the tileset palette, so we decode it once
	// and blit frames straight into the RGBA HUD layer. Loaded lazily on the
	// first icon draw; if the assets are missing we silently fall back to the
	// text labels, so this never breaks the game.
	grp_t cmdicons_grp;
	uint32_t cmdicon_rgb[256] = {};   // 0x00RRGGBB per palette index
	// v0.124: the real SD icon colours. ticon.pcx is not a palette for the icons --
	// it is a 96x1 image holding six 16-entry colour tables (measured: yellow,
	// grey, white, dim yellow, blue, white-grey), and a cmdicons.grp pixel value
	// 1..15 is a COLUMN in such a table: 1 = the brightest, 15 = the near-black
	// outline. We used the PCX's own palette entry `value` instead, where 1..7 are
	// a placeholder blue and 8..15 were patched to an ascending grey ramp -- so the
	// outline came out as the brightest part of every SD icon and the body pale:
	// the "border that washes the icons out". HD icons are ARGB and never hit this.
	uint32_t cmdicon_tbl[6][16] = {};
	bool cmdicon_tbl_ok = false;
	uint32_t game_palette[256] = {};  // 0x00RRGGBB game palette (from tileset WPE)
	a_vector<uint8_t> cmdicon_scratch; // reused decode buffer (avoid per-frame alloc)
	bool cmdicons_tried = false;
	bool cmdicons_ok = false;

	// --- Unit wireframe portrait (v0.7) ---------------------------------
	// wirefram.grp holds one schematic per unit type (frame index == unit id).
	// Each pixel's game-palette luminance is tinted, so it reads as the classic
	// monochrome wireframe.
	//
	// v0.82: the MPQ carries three wireframe sets, not one (verified with
	// `aidump wire` against the user's own MPQ):
	//   wirefram.grp  228 frames  64x64  single selected unit
	//   grpwire.grp   131 frames  32x32  the multi-unit group grid
	//   tranwire.grp  106 frames  64x64  units loaded in a transport
	// Until now only the first was loaded and the group grid downscaled it,
	// which is why that grid looked mushy: wrong asset, not wrong scaling.
	// Hit rects for the equipment icons, rebuilt each frame. E2 (tap-hold ->
	// full values) reads this; the icons and their level badges (E1) do not
	// need it, so the list is populated even before that is wired.
	// weapon != null for a weapon slot: a worker's weapon (Fusion Cutter, Particle
	// Beam, Spines) has damage_upgrade == None, so the tooltip must fall back to the
	// weapon's own name instead of showing a bogus "Upgrade / Not researched".
	struct equip_hit_t { rect r; int upgrade; const weapon_type_t* weapon = nullptr; };
	a_vector<equip_hit_t> equip_hit;
	// v0.83.016 (C1): which equipment upgrade a tap-hold is hovering, or -1.
	// The zones (equip_hit) existed since v0.82 but nothing read them, so the
	// hold showed nothing. Stored as the UpgradeTypes id (stable across the
	// per-frame rebuild), not an index.
	int equip_hover_upgrade = -1;
	int equip_hit_test(int mx, int my) {
		for (auto& e : equip_hit)
			if (mx >= e.r.from.x && mx < e.r.to.x && my >= e.r.from.y && my < e.r.to.y) return e.upgrade;
		return -1;
	}

	grp_t wireframe_grp;
	grp_t grpwire_grp;    // group-selection set (32x32)
	grp_t tranwire_grp;   // transport-cargo set
	a_vector<uint8_t> wireframe_scratch;
	bool wireframe_tried = false;
	bool wireframe_ok = false;

	// --- Resource/status icons (v0.8): game\icons.grp, drawn in the game
	// palette. Mineral / gas / supply icons for the resource readout.
	grp_t icons_grp;
	a_vector<uint8_t> icons_scratch;
	bool icons_tried = false;
	bool icons_ok = false;

	// v0.82.034: game\icons.grp is NOT RLE. Measured with `aidump icondump`:
	// its frame offsets are spaced exactly fw*fh apart (196 bytes for a 14x14)
	// and the last frame runs 53*14 to the file's end -- the frames are flat
	// uncompressed bitmaps, one byte per pixel, with no row table at all.
	// Decoding them as RLE yields nothing, which is exactly what the sanity
	// gate below then rejected, and why the SD resource icons have always
	// fallen back to drawing letters. The old comment blaming out-of-range
	// offsets was wrong: the offsets are fine, the format is different.
	struct raw_icon_t { int w = 0, h = 0, left = 0, top = 0; a_vector<uint8_t> px; };
	a_vector<raw_icon_t> icons_raw;

	bool load_icons_raw(const a_vector<uint8_t>& b) {
		if (b.size() < 6) return false;
		int frames = b[0] | (b[1] << 8);
		if (frames <= 0 || frames > 64) return false;
		a_vector<raw_icon_t> out;
		for (int i = 0; i != frames; ++i) {
			size_t o = 6 + (size_t)i * 8;
			if (o + 8 > b.size()) return false;
			raw_icon_t f;
			f.left = b[o]; f.top = b[o + 1]; f.w = b[o + 2]; f.h = b[o + 3];
			uint32_t off = b[o + 4] | (b[o + 5] << 8) | (b[o + 6] << 16) | ((uint32_t)b[o + 7] << 24);
			if (f.w <= 0 || f.h <= 0) return false;
			if (off + (size_t)f.w * f.h > b.size()) return false;
			f.px.assign(b.begin() + off, b.begin() + off + (size_t)f.w * f.h);
			out.push_back(std::move(f));
		}
		// Only accept the raw reading if the layout really is contiguous: the
		// first frame's data must end where the second begins. Otherwise this
		// is a normal RLE GRP and must not be read this way.
		if (out.size() >= 2) {
			size_t o0 = 6 + 4, o1 = 6 + 8 + 4;
			uint32_t a0 = b[o0] | (b[o0 + 1] << 8) | (b[o0 + 2] << 16) | ((uint32_t)b[o0 + 3] << 24);
			uint32_t a1 = b[o1] | (b[o1 + 1] << 8) | (b[o1 + 2] << 16) | ((uint32_t)b[o1 + 3] << 24);
			if (a1 != a0 + (uint32_t)(out[0].w * out[0].h)) return false;
		}
		icons_raw = std::move(out);
		return true;
	}

	void load_icons() {
		if (icons_tried) return;
		icons_tried = true;
		if (!load_data_file) return;
		try {
			a_vector<uint8_t> buf;
			load_data_file(buf, "game\\icons.grp");
			if (buf.empty()) return;
			if (load_icons_raw(buf)) {
				icons_ok = icons_raw.size() >= 3;
				ui::log("icons.grp: raw format, %d frames (ok=%d)\n",
					(int)icons_raw.size(), (int)icons_ok);
				return;
			}
			ui::log("icons.grp: %d bytes, not raw -- trying RLE\n", (int)buf.size());
			// A mod may ship a normal RLE icons.grp; keep the old path for that.
			icons_grp = read_grp_tolerant(buf.data(), buf.size());
			// Sanity gate: only trust the parse if the first few resource frames
			// have plausible dimensions AND actually decoded pixel data. The
			// latter is critical: icons.grp's frame offsets point past its own
			// buffer, so read_grp_tolerant yields structurally-valid frames with
			// EMPTY data_container - and draw_frame would null-deref on those
			// (data_container.data() == nullptr). Requiring non-empty data +
			// a full line table keeps such undecodable GRPs out (-> letter
			// fallback) instead of crashing.
			icons_ok = icons_grp.frames.size() >= 3;
			for (size_t i = 0; icons_ok && i < 3; ++i) {
				auto& f = icons_grp.frames[i];
				if (f.size.x < 6 || f.size.x > 48 || f.size.y < 6 || f.size.y > 48) icons_ok = false;
				else if (f.data_container.empty() || f.line_data_offset.size() < f.size.y) icons_ok = false;
			}
		} catch (std::exception& e) {
			ui::log("icons.grp: load failed (%s)\n", e.what());
		} catch (...) {
			ui::log("icons.grp: load failed\n");
		}
	}

	// Raw (uncompressed) icon in the game palette, index 0 transparent.
	void blit_raw_icon(uint32_t* data, size_t pitch, const raw_icon_t& f, int x0, int y0, int scale = 1) {
		if (scale < 1) scale = 1; // Faze 4.3: nearest-scale for the FULL HD HUD
		for (int y = 0; y != f.h * scale; ++y) {
			int py = y0 + f.top * scale + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			int sy = y / scale;
			for (int x = 0; x != f.w * scale; ++x) {
				uint8_t idx = f.px[(size_t)sy * f.w + (x / scale)];
				if (!idx) continue;
				int px = x0 + f.left * scale + x;
				if (px < 0 || px >= (int)screen_width) continue;
				row[px] = 0xff000000u | game_palette[idx];
			}
		}
	}

	// Blit a GRP frame in the game palette (index 0 transparent), top-left (x0,y0).
	void blit_grp_gamepal(uint32_t* data, size_t pitch, grp_t& grp, a_vector<uint8_t>& scratch, int frame_index, int x0, int y0) {
		if (frame_index < 0 || frame_index >= (int)grp.frames.size()) return;
		auto& f = grp.frames[frame_index];
		size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return;
		// draw_frame reads data_container via line_data_offset[y]; bail on any
		// under-decoded frame (empty data or short line table) to avoid a
		// null/out-of-bounds deref inside draw_frame.
		if (f.data_container.empty() || f.line_data_offset.size() < fh) return;
		scratch.assign(fw * fh, 0);
		draw_frame(f, false, scratch.data(), fw, 0, 0, fw, fh);
		for (size_t y = 0; y != fh; ++y) {
			int py = y0 + (int)y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = scratch.data() + y * fw;
			for (size_t x = 0; x != fw; ++x) {
				uint8_t idx = srow[x];
				if (idx == 0) continue;
				int px = x0 + (int)x;
				if (px < 0 || px >= (int)screen_width) continue;
				row[px] = 0xff000000u | game_palette[idx];
			}
		}
	}

	// Draw one resource segment: optional game\icons.grp frame (native size,
	// vertically centered on the text) followed by a colored value. Advances
	// left-to-right and returns the x past the segment.
	int draw_res_segment(uint32_t* data, size_t pitch, int x, int y, int icon_frame, const char* value, uint32_t color) {
		if (icon_frame >= 0 && icons_ok && icon_frame < (int)icons_raw.size()) {
			auto& f = icons_raw[icon_frame];
			int iy = y + ((int)ui::unispace_h - f.h) / 2;
			if (iy < 0) iy = 0;
			blit_raw_icon(data, pitch, f, x, iy);
			x += f.w + 4;
		} else if (icon_frame >= 0 && icons_ok && icon_frame < (int)icons_grp.frames.size()) {
			auto& f = icons_grp.frames[icon_frame];
			int iy = y + ((int)ui::unispace_h - (int)f.size.y) / 2;
			if (iy < 0) iy = 0;
			blit_grp_gamepal(data, pitch, icons_grp, icons_scratch, icon_frame, x, iy);
			x += (int)f.size.x + 4;
		}
		draw_text_rgba(data, pitch, x, y, value, color);
		x += (int)strlen(value) * (ui::unispace_w + 1);
		return x;
	}

	// Unit names from rez\stat_txt.tbl. In retail the string index for a unit's
	// name is (unit id + 1) (1-based table). Loaded lazily; also the base for
	// later UI text (tooltips, status/error messages, launcher lists). Index
	// convention is provisional pending tablet verification.
	string_table_data unit_names_tbl;
	bool unit_names_tried = false;
	bool unit_names_ok = false;

	void load_unit_names() {
		if (unit_names_tried) return;
		unit_names_tried = true;
		if (!load_data_file) return;
		try {
			load_data_file(unit_names_tbl.data, "rez\\stat_txt.tbl");
			unit_names_ok = unit_names_tbl.data.size() > 2;
		} catch (...) {
		}
	}

	// Selected-unit display name; empty if the table is unavailable. Strips
	// leading/trailing non-printable control bytes some .tbl entries carry.
	a_string unit_name(const unit_type_t* ut) {
		a_string out;
		if (!unit_names_ok || !ut) return out;
		a_string s;
		try {
			s = unit_names_tbl[(size_t)ut->id + 1];
		} catch (...) {
			return out;
		}
		size_t a = 0, b = s.size();
		while (a < b && (unsigned char)s[a] < 0x20) ++a;
		while (b > a && (unsigned char)s[b - 1] < 0x20) --b;
		for (size_t i = a; i < b; ++i) out += s[i];
		return out;
	}

	// Military rank, from stat_txt.tbl 550-572 (1-based).
	//
	// v0.83.005, after the user asked the right question -- "did you invent
	// this, or is it from the files?" Jim Raynor was reading "Recruit", which
	// he never does in the original. What is measured and what is not:
	//
	//   MEASURED: the ladder text (550 Recruit .. 568 General) and, at the end
	//   of it, four HERO TITLES that name their own units: 569 "Captain
	//   Raynor", 570 "General Duke", 571 "Admiral", 572 "Tassadar". Those four
	//   map to units with no guesswork -- the string says whose it is.
	//
	//   NOT MEASURED: which rung a plain unit starts on, and how many kills
	//   promote it. `aidump sublabel` settled one candidate: units.dat's
	//   sublabel field is NOT a rank -- it points at another unit's NAME (the
	//   Marine's sublabel resolves to "Terran Ghost"). The thresholds live in
	//   the retail executable, not in any .dat we can read. So rank_increase
	//   stays 0 and a plain unit reads the first rung. That is a known gap,
	//   written down rather than papered over with a plausible ladder.
	const char* unit_rank_name(const unit_t* u) {
		if (!u) return nullptr;
		load_unit_names();
		if (!unit_names_ok) return nullptr;
		int index = -1;
		switch (u->unit_type->id) {
		case UnitTypes::Hero_Jim_Raynor_Marine:
		case UnitTypes::Hero_Jim_Raynor_Vulture:      index = 569; break;
		case UnitTypes::Hero_Edmund_Duke_Tank_Mode:
		case UnitTypes::Hero_Edmund_Duke_Siege_Mode:  index = 570; break;
		case UnitTypes::Hero_Gerard_DuGalle:          index = 571; break;
		case UnitTypes::Hero_Tassadar:                index = 572; break;
		default: break;
		}
		if (index >= 0) {
			try {
				rank_name_cache = unit_names_tbl[(size_t)index];
			} catch (...) {
				return nullptr;
			}
			size_t ha = 0, hb = rank_name_cache.size();
			while (ha < hb && (unsigned char)rank_name_cache[ha] < 0x20) ++ha;
			while (hb > ha && (unsigned char)rank_name_cache[hb - 1] < 0x20) --hb;
			rank_name_cache = rank_name_cache.substr(ha, hb - ha);
			return rank_name_cache.empty() ? nullptr : rank_name_cache.c_str();
		}
		if (st.players[u->owner].race != race_t::terran) return nullptr;
		int step = (int)u->rank_increase;
		if (step < 0) step = 0;
		if (step > 18) step = 18;              // 550..568, above that are hero titles
		try {
			rank_name_cache = unit_names_tbl[(size_t)550 + step];
		} catch (...) {
			return nullptr;
		}
		size_t a = 0, b = rank_name_cache.size();
		while (a < b && (unsigned char)rank_name_cache[a] < 0x20) ++a;
		while (b > a && (unsigned char)rank_name_cache[b - 1] < 0x20) --b;
		rank_name_cache = rank_name_cache.substr(a, b - a);
		return rank_name_cache.empty() ? nullptr : rank_name_cache.c_str();
	}
	a_string rank_name_cache;

	void load_wireframe() {
		if (wireframe_tried) return;
		wireframe_tried = true;
		if (!load_data_file) return;
		try {
			a_vector<uint8_t> buf;
			load_data_file(buf, "unit\\wirefram\\wirefram.grp");
			if (buf.empty()) return;
			data_loading::data_reader_le r(buf.data(), buf.data() + buf.size());
			wireframe_grp = read_grp(r);
			wireframe_ok = !wireframe_grp.frames.empty();
		} catch (...) {
		}
		// The other two sets are optional: a mod may ship only wirefram.grp,
		// and every caller falls back to it when the frame is missing.
		for (auto pair : {std::make_pair(&grpwire_grp, "unit\\wirefram\\grpwire.grp"),
		                  std::make_pair(&tranwire_grp, "unit\\wirefram\\tranwire.grp")}) {
			try {
				a_vector<uint8_t> b2;
				load_data_file(b2, pair.second);
				if (b2.empty()) continue;
				data_loading::data_reader_le r2(b2.data(), b2.data() + b2.size());
				*pair.first = read_grp(r2);
			} catch (...) {
			}
		}
	}

	// v0.82.001 (design D2, corrected). The wireframe is drawn as two colours
	// lerped by the art's luminance: `edge` for the dark structural lines and
	// `fill` for the lit interior.
	//
	// Terran and Protoss share the same scheme -- the shape itself goes green
	// -> orange -> red with HP. Protoss differs ONLY by a cyan rim, and that
	// rim tracks SHIELDS, not HP: at zero shields it is simply gone. HP and
	// shields are independent inputs, which the first attempt got wrong by
	// painting the whole Protoss wireframe cyan.
	//
	// Zerg is the organic one: a rich carrot outline with a yellow-green fill
	// that turns red-violet as it takes damage.
	// A single colour converts implicitly, so the callers that only ever wanted
	// one tint (production thumbnails, cargo slots) keep reading as they did.
	// v0.82.004: wirefram.grp is FULL-COLOUR art -- 128 distinct palette
	// indices, measured with `aidump wire`. Nobody authors 128 colours for
	// something that gets flattened to one tint, and that is exactly what this
	// port was doing: take the drawing, reduce it to luminance, repaint it.
	// That is why no amount of colour-picking made the Zerg set look right --
	// the organic reds and greens are IN the art. So the base is now drawn as
	// authored, and HP only mixes a damage colour over it.
	//
	// blend: 0 = the art untouched, 100 = fully the tint (used for the rim).
	struct wire_colors_t {
		uint32_t tint; int blend;
		// literal = draw the art's own palette colours (Zerg: its bands 216-219
		// already are yellow/green/orange/red and reproduce BW exactly).
		// Otherwise tint by luminance, which is what Terran and Protoss need:
		// their art is dominated by index 192, and drawing that literally as
		// dark navy turns the whole wireframe into an unreadable blob. That is
		// how v0.82.004 broke both races while fixing Zerg.
		bool literal;
		// v0.82.008: Protoss shields. Indices 192/193 are almost certainly the
		// shield zone in the drawing -- twire.pcx's blue ramp ends on 042064 /
		// 001034, right next to the art's 0c1424 / 243c64. So those two pixels
		// are lit by the shield pool instead of drawn as dead navy, and the
		// separate outline is dropped: the glow IS the shield readout.
		uint32_t shield_lo, shield_hi;   // 0 = not a shielded unit
		// v0.82.010: the HD path. SC:R's wirefram.dds.grp is greyscale art --
		// measured with `aidump hdwire` on the user's own HD assets: a Drone
		// averages saturation 19/255 and a Probe 2/255, with #e0e0e0 / #a0a0a0 /
		// #606060 / #202020 dominating. So it is a luminance mask meant to be
		// tinted, and the SD index families have a direct analogue in it:
		// bright = the line, mid = the fill, dark = the 192/193 shield zone.
		// Before this the HD path threw the whole colour model away and used one
		// flat green, which is why HD Zerg never matched SD Zerg.
		uint32_t hd_line, hd_fill;
		// v0.82.026: one colour per HD mask cell, in file order. 0 = do not draw
		// that layer at all (the Zerg contour is meant to be transparent).
		// Layers beyond this table fall back to hd_line.
		uint32_t hd_layer[6];
		bool hd_layered;            // false = the old single-tint behaviour
		wire_colors_t(uint32_t c = 0, int b = 100, bool lit = false)
			: tint(c), blend(b), literal(lit), shield_lo(0), shield_hi(0),
			  hd_line(0x30f030u), hd_fill(0x186018u), hd_layered(false) {
			for (int i = 0; i != 6; ++i) hd_layer[i] = 0;
		}
	};
	// Wireframe art does NOT live in the tileset palette. Measured from the
	// user's own MPQ (`aidump wire`) and from grpwire frames exported as 8-bit
	// BMPs: the drawings use just six indices, and which six depends on the
	// race, which is why Zerg never looked right no matter what tint was
	// applied.
	//
	//   192, 193      dark navy structure -- Terran and Protoss only.
	//                 Confirmed byte-identical in game\twire.pcx's own palette.
	//   208..211      state bands, Terran and Protoss
	//   216..219      state bands, Zerg (and Zerg has no 192/193 base at all)
	//
	// twire.pcx leaves 208..219 at a placeholder blue, so those four are the
	// ones BW remaps at runtime; its 24 pixels are that remap's data. The exact
	// (band, HP) -> colour rule is NOT established yet, so this draws the bands
	// with the colours the exported frames show and leaves the rule alone.
	uint32_t wireframe_palette_entry(uint8_t idx) const {
		switch (idx) {
		case 192: return 0x0c1424u;
		case 193: return 0x243c64u;
		case 208: case 216: return 0xfcfc3au;   // yellow
		case 209: case 217: return 0x10fa1au;   // green
		case 210: case 218: return 0xf88c16u;   // orange
		case 211: case 219: return 0xc81a18u;   // red
		default: return game_palette[idx];
		}
	}

	// hpp_override lets a caller ask for the colours a unit WOULD have at some
	// other health -- the HD path needs the full-health set for its base layer,
	// because damage there is carried by individual parts, not by recolouring
	// the whole drawing.
	wire_colors_t wire_palette(const unit_t* u, int hpp_override = -1) const {
		int hpp = hpp_override >= 0 ? hpp_override : unit_hp_percent(u);
		if (u && u->unit_type && unit_race(u) == race_t::zerg) {
			// The art already carries the organic colouring; damage only mixes
			// over it.
			wire_colors_t z(hpp > 66 ? 0x000000u : (hpp > 33 ? 0xf0f030u : 0xf03030u),
			                hpp > 66 ? 0 : (hpp > 33 ? 35 : 60), true);
			// HD has no palette to be literal about, so state the same look
			// explicitly: green line over yellow fill, decaying to red over
			// near-black. These are the SD band colours (209/217 green,
			// 208/216 yellow, 211/219 red) the greyscale art cannot carry.
			// v0.82.016, by request after seeing it in game: what read as green
			// becomes red and what read as yellow becomes green, and the rim
			// goes black (see draw_wire_rim). SD is untouched -- the user
			// confirmed the SD Drone already matches BW.
			z.hd_line = hpp > 66 ? 0xc81a18u : (hpp > 33 ? 0xfcfc3au : 0x901010u);
			z.hd_fill = hpp > 66 ? 0x10fa1au : (hpp > 33 ? 0x8020a0u : 0x201020u);
			// v0.82.028: a HEATMAP, not four fixed colours. Zerg wireframes run
			// a green -> yellow -> orange -> red ramp and the whole ramp SLIDES
			// with remaining health, which is why a healthy Drone is not simply
			// all green: it keeps a dark organic base with green highlights and
			// yellow-green midtones, and those same zones turn yellow, orange
			// and red as it dies.
			// v0.82.030: every value below is READ OUT OF game\twire.pcx, the
			// 24-entry remap strip BW keeps beside tunit.pcx and tselect.pcx.
			// It is not one ramp but grouped runs -- dumped with `aidump wire`:
			//   0..2   fcfc38 yellow, 10fc18 green, 080808 near black
			//   3..9   0c48cc .. 001034   the blue run (Protoss shields)
			//   10..13 c81818 a80808 a80808 840404   reds
			//   14..17 88409c 683078 481c50          violets
			//   18..20 f88c14 e85014 a0541c          orange, red-orange, brown
			// Sorted by heat those runs ARE the Zerg spectrum, and they match
			// the four SD band colours (208..211) that the exported wireframe
			// BMPs show, to within a bit of rounding. No invented hex here:
			// the previous fluorescent set was mine, and it had no business
			// standing in for data that was in the file all along.
			// v0.82.033: the same hues as twire.pcx, pushed to neon by request.
			// This IS a deliberate departure from the file now, not me guessing
			// at data I had not read -- twire's own values are in the comment
			// beside each step so the authentic set stays one edit away.
			static const uint32_t ramp[] = {
				0x000000u,   // 0 backing
				0x1a0a0au,   // 1 near black       (twire 080808)
				0xff0040u,   // 2 crimson          (twire 840404)
				0xff2000u,   // 3 red              (twire a80808)
				0xff4000u,   // 4 red-orange       (twire c81818)
				0xff8000u,   // 5 orange           (twire e85014)
				0xffc400u,   // 6 amber            (twire f88c14)
				0xffff00u,   // 7 yellow           (twire fcfc38)
				0x00ff40u,   // 8 green            (twire 10fc18)
			};
			// v0.82.033: the sliding ramp is back -- v0.82.032 replaced it with
			// fixed colours to trial an arrangement, and dropping the health
			// behaviour along with it was not what had been asked for.
			// The mask cells arrive ordered by coverage: the smallest is the
			// highlight, the largest is the fill. So layer i sits i steps below
			// the top of the ramp, and the top slides from 8 (full health) down
			// to 4 (dead) -- a four-wide window walking the whole spectrum.
			z.hd_layered = true;
			int top = 4 + hpp * 4 / 100;
			for (int i = 0; i != 4; ++i) {
				int idx = top - i;
				if (idx < 0) idx = 0; if (idx > 8) idx = 8;
				z.hd_layer[i] = ramp[idx];
			}
			z.hd_layer[4] = 0;           // contours off, by request
			z.hd_layer[5] = 0;
			z.hd_fill = ramp[0];         // backing shadow
			return z;
		}
		// Terran and Protoss keep the luminance tint: green -> yellow -> red.
		wire_colors_t wc(hpp > 66 ? 0x30f030u : (hpp > 33 ? 0xf0f030u : 0xf03030u), 100, false);
		wc.hd_line = wc.tint;
		wc.hd_fill = hpp > 66 ? 0x186018u : (hpp > 33 ? 0x605018u : 0x601818u);
		if (u && u->unit_type && u->unit_type->has_shield && u->unit_type->shield_points > 0) {
			int sp = (int)u->shield_points.integer_part() * 100 / u->unit_type->shield_points;
			if (sp > 100) sp = 100;
			if (sp > 0) {
				// The authentic ramp out of game\twire.pcx, dark end first.
				static const uint32_t ramp[] = {
					0x001034u, 0x042064u, 0x042064u, 0x083498u, 0x083498u, 0x0c48ccu, 0x0c48ccu
				};
				int n = (int)(sizeof(ramp) / sizeof(ramp[0]));
				int hi = sp * (n - 1) / 100;
				wc.shield_hi = ramp[hi];
				wc.shield_lo = ramp[hi / 2];
			}
		}
		return wc;
	}

	// A flag at each selected building's rally point (if set) with a dashed line
	// from the building, so the player can see where trained units will gather.
	// #6: the engine already rallies ALL selected factories (action_order), so
	// every own selected building draws its own marker.
	void draw_rally_indicator(uint32_t* data, size_t pitch) {
		auto to_screen = [&](xy p, int& sx, int& sy) {
			sx = (int)((fp16::integer(p.x - screen_pos.x) * view_scale).integer_part());
			sy = (int)((fp16::integer(p.y - screen_pos.y) * view_scale).integer_part());
		};
		// open1161HD: draws ONE rally marker (line + flag). Called twice per
		// building -- the unit/army rally in GREEN and the SC2-style worker-only
		// rally (a resource) in RED, so a split rally is legible at a glance.
		auto marker = [&](xy rp, xy bpos, uint32_t line_c, uint32_t flag_c) {
			if (rp == xy() || rp == bpos) return;
			int sx, sy; to_screen(rp, sx, sy);
			if (sx < 8 || sy < 8 || sx >= (int)screen_width - 8 || sy >= (int)screen_height - 8) return;
			int bx, by; to_screen(bpos, bx, by);
			draw_line2(data, pitch, bx, by, sx, sy, line_c, true);
			mark_flag(data, pitch, sx, sy, flag_c);
		};
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player) continue;
			if (!(u->unit_type->flags & unit_type_t::flag_building)) continue;
			marker(u->building.rally.pos, u->sprite->position, 0x30c030u, 0x30f030u);        // army = green
			marker(u->building.worker_rally.pos, u->sprite->position, 0xc03030u, 0xf03030u); // workers = red
		}
	}

	// Pulsing red reticle at every active nuke aim point (user request). The sim
	// marks a painted/incoming nuke with a Ghost's ghost.nuke_dot thingy
	// (order_NukeTrack creates it, detonation kills it); the vanilla dot sprite is
	// easy to miss, so draw a bold breathing reticle there: a core, a ring and
	// four ticks. Gated on the local player's vision (visibility_flags) -- warns
	// of nukes on ground you can see, same rule as the vanilla dot, and never
	// reveals a fogged enemy target.
	void draw_nuke_indicator(uint32_t* data, size_t pitch) {
		auto to_screen = [&](xy p, int& sx, int& sy) {
			sx = (int)((fp16::integer(p.x - screen_pos.x) * view_scale).integer_part());
			sy = (int)((fp16::integer(p.y - screen_pos.y) * view_scale).integer_part());
		};
		const int e = mark_e();
		int ph = (int)(st.current_frame & 31);
		int r = (7 + (ph < 16 ? ph : 31 - ph) / 3) * e;   // ~7..12 px breathing ring
		for (int p = 0; p < 12; ++p) {
			for (unit_t* u : ptr(st.player_units[p])) {
				if (!unit_is_ghost(u) || !u->ghost.nuke_dot || !u->ghost.nuke_dot->sprite) continue;
				sprite_t* d = u->ghost.nuke_dot->sprite;
				if (game_local_player >= 0 && game_local_player < 8
				    && !(d->visibility_flags & (1 << game_local_player))) continue;
				int sx, sy; to_screen(d->position, sx, sy);
				if (sx < -32 || sy < -32 || sx >= (int)screen_width + 32 || sy >= (int)screen_height + 32) continue;
				mark_box(data, pitch, sx - 3 * e, sy - 3 * e, sx + 4 * e, sy + 4 * e, 0xff000000u, (int)screen_height);
				mark_box(data, pitch, sx - 2 * e, sy - 2 * e, sx + 3 * e, sy + 3 * e, 0xffff2020u, (int)screen_height);   // solid core
				mark_rect(data, pitch, {{sx - r, sy - r}, {sx + r + 1, sy + r + 1}}, 0xff5050u);                        // breathing ring
				const int t0 = r + 2 * e, t1 = r + 6 * e;                                                              // ticks
				draw_line2(data, pitch, sx, sy - t0, sx, sy - t1, 0xff5050u);
				draw_line2(data, pitch, sx, sy + t0, sx, sy + t1, 0xff5050u);
				draw_line2(data, pitch, sx - t0, sy, sx - t1, sy, 0xff5050u);
				draw_line2(data, pitch, sx + t0, sy, sx + t1, sy, 0xff5050u);
			}
		}
	}

	// Draw the primary selected unit's queued movement/attack waypoints: a line
	// from the unit through its current order target and each queued order's
	// target, with a diamond at every waypoint -- numbered in order when there is
	// more than one. Reads the live selection each frame, so it shows for
	// whichever unit is selected and vanishes when the selection changes or clears.
	void draw_order_queue_lines(uint32_t* data, size_t pitch) {
		unit_t* u = first_own_selected_unit();
		if (!u || !u->sprite) return;
		auto to_screen = [&](xy p) {
			return xy((int)((fp16::integer(p.x - screen_pos.x) * view_scale).integer_part()),
			          (int)((fp16::integer(p.y - screen_pos.y) * view_scale).integer_part()));
		};
		// Collect the waypoint chain: current order target, then the queue.
		static thread_local a_vector<xy> pts;
		pts.clear();
		auto push_target = [&](const order_target_t& t) {
			xy p = t.unit && t.unit->sprite ? t.unit->sprite->position : t.position;
			if (p != xy()) pts.push_back(p);
		};
		// u->order_target is a target_t (.pos); queued orders carry order_target_t.
		{
			xy p = u->order_target.unit && u->order_target.unit->sprite
				? u->order_target.unit->sprite->position : u->order_target.pos;
			if (p != xy()) pts.push_back(p);
		}
		for (auto& o : u->order_queue) push_target(o.target);
		if (pts.empty()) return;
		for (auto& p : pts) p = to_screen(p);
		for (int pass = 0; pass != 2; ++pass) {
			xy from = to_screen(u->sprite->position);
			for (auto& p : pts) {
				mark_line_pass(data, pitch, from.x, from.y, p.x, p.y, 0x20b040u, pass, false, (int)screen_height);
				from = p;
			}
		}
		const bool numbered = pts.size() > 1;
		bool _prev_hst = hud_scale_text;
		hud_scale_text = true;
		struct g_t { bool* f; bool p; ~g_t() { *f = p; } } _g{&hud_scale_text, _prev_hst};
		for (size_t i = 0; i < pts.size(); ++i) {
			if (!numbered) { mark_diamond(data, pitch, pts[i].x, pts[i].y, hud_h_px(5), 0x40ff70u, 0x40ff70u); continue; }
			mark_diamond(data, pitch, pts[i].x, pts[i].y, hud_h_px(11), 0x40ff70u, 0x06140au);
			char nb[4];
			if (i < 9) snprintf(nb, sizeof(nb), "%d", (int)i + 1); else snprintf(nb, sizeof(nb), "%s", "+");
			draw_text_rgba(data, pitch, pts[i].x - usp_w() / 2, pts[i].y - usp_h() / 2, nb, 0xff40ff70u);
		}
	}

	// HD wireframes (v0.23): wirefram.dds.grp frames are 8-cell strips of
	// square cells. NOT "cell 0 is the complete outline" -- that claim cost a
	// regression in v0.73; v0.82 measured the real layout (cells 0..N-2 are
	// white masks, N-1 is the finished artwork, see hd_wireframe_t below).
	// This older path unions the cells; it is kept for the flat-tint callers.
	// Scaled per (unit, box) and cached.
	ddsgrp::file_t hd_wireframe_dds;
	bool hd_wireframe_tried = false;
	std::map<int, cached_frame_t> hd_wireframe_cache;
	size_t hd_wireframe_cache_bytes = 0;
	bool hd_wireframe_cache_trim_logged = false;

	// v0.73 item 18.1: the individual cells, kept apart instead of unioned.
	// SC:R stores each wireframe as a strip whose cells are PARTS of the unit
	// and tints them per damage state; the engine even keeps a field for it
	// (unit_t::wireframe_randomizer, set from lcg_rand at spawn) whose only
	// purpose in retail is deciding WHICH parts darken, so two identical units
	// at the same HP don't show the same damage pattern. We were collapsing the
	// strip into one silhouette and applying a single tint, which threw that
	// away. Cached per (unit type, box) like the unioned version was.
	// outline = every cell composited by max alpha (what pre-v0.73 produced and
	// what actually looks like a wireframe); parts = the individual cells.
	// v0.82.026: what the strip really holds, measured from SC:R's own
	// wirefram.dds.grp with `aidump hdcells` (sheets in _wireframe-bands/).
	// A frame is 2048x256 -- eight 256x256 cells -- laid out as:
	//   cells 0..N-2  MASKS, white or near-white, ordered by coverage
	//   cell  N-1     the finished COLOUR artwork
	//   trailing      empty
	// and the masks reproduce the SD index families one for one:
	//   SCV    0..3 = four disjoint body parts (SD 208..211)
	//          4..5 = navy structure, #202060 / #000020 (SD 192/193, byte exact)
	//   Drone  0..5 = layers, not parts: highlights, line work, fill, contour
	// Until now every cell -- masks AND artwork -- was unioned into one
	// silhouette and tinted by luminance, which is why no choice of colour
	// could make HD match SD: the masks were being drawn as if they were the
	// picture.
	struct hd_wireframe_t {
		ddsgrp::frame_t outline;              // union of the masks: the silhouette
		a_vector<ddsgrp::frame_t> parts;      // the mask cells, in file order
		ddsgrp::frame_t art;                  // the finished artwork cell
		bool has_art = false;
		size_t bytes = 0;
		uint64_t used = 0;
		int frame = -1;
	};
	std::map<int, hd_wireframe_t> hd_wireframe_cells;
	size_t hd_wireframe_cells_bytes = 0;
	bool hd_wireframe_cells_logged = false;
	bool hd_wireframe_cells_trim_logged = false;

	hd_wireframe_t* get_hd_wireframe_cells(int fi, int box) {
		if (hd_ui_dir.empty() && !hd_casc) return nullptr;
		if (!hd_wireframe_tried) {
			hd_wireframe_tried = true;
			if (!hd_ui_dir.empty()) {
				hd_wireframe_dds = ddsgrp::load_file((hd_ui_dir + "/wirefram.dds.grp").c_str());
			} else {
				// open1161HD Faze 4.2d: HD unit wireframes from CASC.
				std::vector<uint8_t> tmp;
				if (load_hd_bytes && load_hd_bytes(hd_casc_prefix + "unit/wirefram/wirefram.dds.grp", tmp) && !tmp.empty())
					hd_wireframe_dds = ddsgrp::parse(tmp.data(), tmp.size());
			}
			ui::log("hd wireframes: %d frames (ok=%d)\n", (int)hd_wireframe_dds.frame_count(), (int)hd_wireframe_dds.ok);
		}
		if (!hd_wireframe_dds.ok || fi < 0 || fi >= (int)hd_wireframe_dds.frame_count()) return nullptr;
		int key = fi * 4096 + box;
		auto it = hd_wireframe_cells.find(key);
		if (it == hd_wireframe_cells.end()) {
			// Decoded here and released at the end of the block: the cells below
			// are what gets cached.
			ddsgrp::frame_t src = hd_wireframe_dds.decode((size_t)fi);
			// Faze 4.7c: a partial decode (undersized rgba, e.g. under FULL HD
			// memory pressure) must not reach the cell-copy loops below, which read
			// src.rgba up to w*h-1 -- an out-of-bounds heap read otherwise.
			if (src.rgba.size() < (size_t)src.w * (size_t)src.h) src = ddsgrp::frame_t();
			a_vector<ddsgrp::frame_t> cells;
			if (src.h > 0 && src.w >= src.h) {
				int n = src.w / src.h;
				for (int c = 0; c != n; ++c) {
					ddsgrp::frame_t cell;
					cell.w = src.h;
					cell.h = src.h;
					cell.rgba.assign((size_t)cell.w * cell.h, 0);
					for (int y = 0; y != cell.h; ++y) {
						const uint32_t* srow = &src.rgba[(size_t)y * src.w + (size_t)c * cell.w];
						uint32_t* drow = &cell.rgba[(size_t)y * cell.w];
						for (int x = 0; x != cell.w; ++x) drow[x] = srow[x];
					}
					cells.push_back(ddsgrp::scale_box(cell, box, box));
				}
			}
			hd_wireframe_t w;
			// Drop trailing empty cells, then split off the last one: that is
			// the finished artwork, everything before it is a mask.
			while (!cells.empty()) {
				bool any = false;
				for (auto px : cells.back().rgba) if ((px >> 24) >= 16) { any = true; break; }
				if (any) break;
				cells.pop_back();
			}
			if (!cells.empty()) {
				w.art = std::move(cells.back());
				cells.pop_back();
				w.has_art = true;
			}
			if (!cells.empty()) {
				w.outline.w = cells[0].w;
				w.outline.h = cells[0].h;
				w.outline.rgba.assign((size_t)w.outline.w * w.outline.h, 0);
				for (auto& c : cells) {
					if (c.w != w.outline.w || c.h != w.outline.h) continue;
					for (size_t i = 0; i != w.outline.rgba.size(); ++i)
						if ((c.rgba[i] >> 24) > (w.outline.rgba[i] >> 24)) w.outline.rgba[i] = c.rgba[i];
				}
			} else if (w.has_art) {
				w.outline = w.art;                // no masks: the art is the shape
			}
			w.parts = std::move(cells);
			if (!hd_wireframe_cells_logged) {
				// One line, once, so a device session can confirm the split.
				hd_wireframe_cells_logged = true;
				ui::log("hd wireframe cells: unit %d -> %d masks + art=%d\n",
					fi, (int)w.parts.size(), (int)w.has_art);
			}
			w.bytes = w.outline.rgba.size() * 4 + w.art.rgba.size() * 4;
			for (auto& p : w.parts) w.bytes += p.rgba.size() * 4;
			hd_wireframe_cells_bytes += w.bytes;
			it = hd_wireframe_cells.emplace(key, std::move(w)).first;
		}
		it->second.used = ++hd_cache_tick;
		it->second.frame = hd_cache_frame();
		hd_wireframe_t* r = (it->second.parts.empty() && !it->second.has_art) ? nullptr : &it->second;
		trim_hd_cache(hd_wireframe_cells, hd_wireframe_cells_bytes, (size_t)24 << 20,
			"wireframe cells", hd_wireframe_cells_trim_logged);
		return r;
	}

	// open1161HD Faze 4.7 (user request): race-specific SC:R console bezels from
	// game/consoles/<race>/cmdbtns.dds.grp. Frame 12 = the unit wireframe frame
	// (green, notched bottom-right corner); frames 13/14 = the group-selection cell
	// frames (blue). Decoded through the validated ddsgrp path and cached per
	// (race, frame). CASC only (the SC:R art); a no-op under MPQ/disk-mod.
	ddsgrp::file_t console_dds[3];
	bool console_dds_tried[3] = {false, false, false};
	std::map<int, cached_frame_t> console_frame_cache; // key = race_index*64 + frame
	size_t console_frame_bytes = 0;
	bool console_frame_trim_logged = false;

	int console_race_index(race_t r) {
		if (r == race_t::zerg) return 1;
		if (r == race_t::protoss) return 2;
		return 0; // terran / default
	}
	ddsgrp::frame_t* get_console_frame(race_t race, int idx) {
		if (!hd_casc || !load_hd_bytes) return nullptr;
		int ri = console_race_index(race);
		if (!console_dds_tried[ri]) {
			console_dds_tried[ri] = true;
			static const char* folder[3] = {"terran", "zerg", "protoss"};
			std::vector<uint8_t> tmp;
			if (load_hd_bytes(hd_casc_prefix + "game/consoles/" + folder[ri] + "/cmdbtns.dds.grp", tmp) && !tmp.empty())
				console_dds[ri] = ddsgrp::parse(tmp.data(), tmp.size());
			ui::log("hd console %s: %d frames (ok=%d)\n", folder[ri], (int)console_dds[ri].frame_count(), (int)console_dds[ri].ok);
		}
		if (!console_dds[ri].ok || idx < 0 || idx >= (int)console_dds[ri].frame_count()) return nullptr;
		int key = ri * 64 + idx;
		auto it = console_frame_cache.find(key);
		if (it == console_frame_cache.end()) {
			ddsgrp::frame_t f = console_dds[ri].decode((size_t)idx);
			// Partial decode (undersized rgba under memory pressure) must not reach
			// the blit, which reads rgba up to w*h-1 (see the line_rectangle_rgba OOB
			// lesson): drop it to an empty frame instead.
			if (f.rgba.size() < (size_t)f.w * (size_t)f.h) f = ddsgrp::frame_t();
			cached_frame_t e;
			e.f = std::move(f);
			e.bytes = e.f.rgba.size() * 4;
			console_frame_bytes += e.bytes;
			it = console_frame_cache.emplace(key, std::move(e)).first;
		}
		it->second.used = ++hd_cache_tick;
		it->second.frame = hd_cache_frame();
		ddsgrp::frame_t* r = it->second.f.w ? &it->second.f : nullptr;
		trim_hd_cache(console_frame_cache, console_frame_bytes, (size_t)8 << 20,
			"console frames", console_frame_trim_logged);
		return r;
	}
	// Stretch-blit a decoded console bezel (alpha-composited) to fill dst. The bezel
	// interiors are transparent, so it frames whatever is already/next drawn.
	void blit_console_frame(uint32_t* data, size_t pitch, ddsgrp::frame_t* f, rect dst) {
		if (!f || f->w <= 0 || f->h <= 0) return;
		if (f->rgba.size() < (size_t)f->w * (size_t)f->h) return;
		int dw = dst.to.x - dst.from.x, dh = dst.to.y - dst.from.y;
		if (dw <= 0 || dh <= 0) return;
		for (int y = 0; y < dh; ++y) {
			int py = dst.from.y + y;
			if (py < 0 || py >= (int)screen_height) continue;
			const uint32_t* srow = &f->rgba[(size_t)(y * f->h / dh) * f->w];
			uint32_t* drow = data + (size_t)py * pitch;
			for (int x = 0; x < dw; ++x) {
				int px = dst.from.x + x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c = srow[x * f->w / dw];
				uint32_t a = c >> 24;
				if (a < 8) continue;
				if (a >= 248) { drow[px] = 0xff000000u | (c & 0xffffff); continue; }
				uint32_t old = drow[px], ia = 255 - a;
				uint32_t rr = (((c >> 16) & 0xff) * a + ((old >> 16) & 0xff) * ia) / 255;
				uint32_t gg = (((c >> 8) & 0xff) * a + ((old >> 8) & 0xff) * ia) / 255;
				uint32_t bb = ((c & 0xff) * a + (old & 0xff) * ia) / 255;
				drow[px] = 0xff000000u | (rr << 16) | (gg << 8) | bb;
			}
		}
	}

	// SDL_APP_LOWMEMORY: hand the pixel caches back. Everything in them is
	// reconstructible from the mod files, so the only cost is a stutter the
	// next time each entry is wanted. The hd_tiles cache is left alone -- it
	// is already bounded and it is hit every single frame of terrain drawing.
	void drop_hd_caches_on_low_memory() {
		size_t freed = hd_anims_bytes + hd_wireframe_cells_bytes
			+ hd_wireframe_cache_bytes + icon_override_bytes;
		hd_anims.clear();            hd_anims_bytes = 0;
		hd_wireframe_cells.clear();  hd_wireframe_cells_bytes = 0;
		hd_wireframe_cache.clear();  hd_wireframe_cache_bytes = 0;
		icon_override_cache.clear(); icon_override_bytes = 0;
		// open1161HD Faze 4.2d: when HD UI comes from CASC, ddsgrp::parse keeps
		// the whole container in .raw (wireframe ~30 MB, cmdicons ~0.8 MB). Those
		// re-fetch from CASC on demand, so release them under pressure too and let
		// the next request re-pull (reset the _tried gates). Disk-mod load_file
		// keeps only a path, so this only matters for the CASC path. hd_tiles.raw
		// stays: it is hit every terrain frame.
		if (hd_casc) {
			freed += hd_wireframe_dds.raw.size() + hd_cmdicons_dds.raw.size();
			hd_wireframe_dds = ddsgrp::file_t(); hd_wireframe_tried = false;
			hd_cmdicons_dds = ddsgrp::file_t(); hd_cmdicons_dds_tried = false;
			// Faze 4.7: race console bezels (cmdbtns) re-fetch from CASC on demand.
			for (int i = 0; i < 3; ++i) { freed += console_dds[i].raw.size(); console_dds[i] = ddsgrp::file_t(); console_dds_tried[i] = false; }
		}
		freed += console_frame_bytes;
		console_frame_cache.clear(); console_frame_bytes = 0;
		// Faze 7: decoded GRP index scratches (ws>1 sprite path) re-decode on demand.
		freed += grp_ws_cache_bytes;
		grp_ws_cache.clear(); grp_ws_cache_bytes = 0;
		// Decoded HD portrait clips re-decode from CASC on demand.
		freed += hd_portrait_cache_bytes;
		hd_portrait_cache.clear(); hd_portrait_cache_bytes = 0;
		// Faze 4.5: decoded scaled terrain tiles (32*ws squared each; up to ~64 MB
		// at FULL HD) re-decode from hd_tiles.raw on the next terrain frame.
		if (!hd_tiles.cache.empty()) {
			freed += hd_tiles_cache_bytes;
			hd_tiles.cache.clear();
			hd_tiles_cache_bytes = 0; // Faze 7: keep the LRU byte counter in sync
		}
		// Faze 7: under REAL memory pressure also drop the resident raw tileset --
		// the single largest un-droppable native allocation (~12-82 MB at FULL HD
		// scale-4). The next terrain frame re-fetches + re-parses it (load_hd_tiles
		// runs again because hd_tiles_tried is cleared). A one-time stall accepted
		// only when the OS is actually low on memory; normal frames are untouched.
		if (!hd_tiles.raw.empty()) {
			freed += hd_tiles.raw.size();
			hd_tiles.raw.clear(); hd_tiles.raw.shrink_to_fit();
			hd_tiles.ents.clear(); hd_tiles.ents.shrink_to_fit();
			hd_tiles.ok = false; hd_tiles_tried = false;
		}
		ui::log("low memory: dropped %d kB of HD caches\n", (int)(freed >> 10));
	}

	ddsgrp::frame_t* get_hd_wireframe(int fi, int box) {
		if (hd_ui_dir.empty() && !hd_casc) return nullptr;
		if (!hd_wireframe_tried) {
			hd_wireframe_tried = true;
			if (!hd_ui_dir.empty()) {
				hd_wireframe_dds = ddsgrp::load_file((hd_ui_dir + "/wirefram.dds.grp").c_str());
			} else {
				// open1161HD Faze 4.2d: HD unit wireframes from CASC.
				std::vector<uint8_t> tmp;
				if (load_hd_bytes && load_hd_bytes(hd_casc_prefix + "unit/wirefram/wirefram.dds.grp", tmp) && !tmp.empty())
					hd_wireframe_dds = ddsgrp::parse(tmp.data(), tmp.size());
			}
			ui::log("hd wireframes: %d frames (ok=%d)\n", (int)hd_wireframe_dds.frame_count(), (int)hd_wireframe_dds.ok);
		}
		if (!hd_wireframe_dds.ok || fi < 0 || fi >= (int)hd_wireframe_dds.frame_count()) return nullptr;
		int key = fi * 4096 + box;
		auto it = hd_wireframe_cache.find(key);
		if (it == hd_wireframe_cache.end()) {
			ddsgrp::frame_t src = hd_wireframe_dds.decode((size_t)fi);
			// Faze 4.7c: a partial decode (undersized rgba, e.g. under FULL HD
			// memory pressure) must not reach the cell-copy loops below, which read
			// src.rgba up to w*h-1 -- an out-of-bounds heap read otherwise.
			if (src.rgba.size() < (size_t)src.w * (size_t)src.h) src = ddsgrp::frame_t();
			ddsgrp::frame_t scaled;
			if (src.h > 0 && src.w >= src.h) {
				// The strip's square cells are PARTS of the unit (SC:R tints
				// them per damage state); the full outline is their union.
				ddsgrp::frame_t cell;
				cell.w = src.h;
				cell.h = src.h;
				cell.rgba.assign((size_t)cell.w * cell.h, 0);
				int cells = src.w / src.h;
				for (int c = 0; c != cells; ++c) {
					for (int y = 0; y != cell.h; ++y) {
						const uint32_t* srow = &src.rgba[(size_t)y * src.w + (size_t)c * cell.w];
						uint32_t* drow = &cell.rgba[(size_t)y * cell.w];
						for (int x = 0; x != cell.w; ++x) {
							if ((srow[x] >> 24) > (drow[x] >> 24)) drow[x] = srow[x];
						}
					}
				}
				scaled = ddsgrp::scale_box(cell, box, box);
			}
			cached_frame_t e;
			e.f = std::move(scaled);
			e.bytes = e.f.rgba.size() * 4;
			hd_wireframe_cache_bytes += e.bytes;
			it = hd_wireframe_cache.emplace(key, std::move(e)).first;
		}
		it->second.used = ++hd_cache_tick;
		it->second.frame = hd_cache_frame();
		ddsgrp::frame_t* r = it->second.f.w ? &it->second.f : nullptr;
		trim_hd_cache(hd_wireframe_cache, hd_wireframe_cache_bytes, (size_t)8 << 20,
			"wireframes", hd_wireframe_cache_trim_logged);
		return r;
	}

	// Tint and blit one already-scaled HD wireframe cell.
	// tint != 0 forces one flat colour (the damage overdraw); otherwise the
	// pixel's own luminance selects between wc.hd_line, wc.hd_fill and -- for a
	// shielded unit -- the shield ramp, mirroring how the SD path keys off the
	// palette index. The thresholds follow the measured art: #e0e0e0/#c0c0c0
	// are the line, #a0a0a0..#606060 the fill, #404040 and below the dark
	// structure that is index 192/193 in SD.
	// shade_override >= 0 ignores the source pixel's luminance and paints the
	// tint at that strength. The layered path wants it: the mask cells are
	// near-white (#e0e0e0), so multiplying by their own luminance was quietly
	// costing ~12% of every colour and leaving the result muted.
	void blit_hd_wireframe_cell(uint32_t* data, size_t pitch, const ddsgrp::frame_t& hw,
	                            int dstx, int dsty, uint32_t tint,
	                            const wire_colors_t* wc = nullptr, int shade_override = -1) {
		int tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;
		for (int y = 0; y != hw.h; ++y) {
			int py = dsty + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint32_t* srow = &hw.rgba[(size_t)y * hw.w];
			for (int x = 0; x != hw.w; ++x) {
				int px = dstx + x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c = srow[x];
				uint32_t a = c >> 24;
				if (a < 16) continue;
				uint32_t lum = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
				int cr = tr, cg = tg, cb = tb;
				uint32_t shade = lum;
				if (wc) {
					uint32_t sel;
					if (lum >= 0xb0) { sel = wc->hd_line; shade = 255; }
					else if (lum >= 0x50) { sel = wc->hd_fill; shade = 200; }
					else if (wc->shield_hi) { sel = wc->shield_lo; shade = 255; }
					else { sel = wc->hd_fill; shade = 96; }
					cr = (sel >> 16) & 0xff; cg = (sel >> 8) & 0xff; cb = sel & 0xff;
				}
				if (shade_override >= 0) shade = (uint32_t)shade_override;
				// v0.82.031: `shade = shade * a / 255` used to be here, and the
				// blend below already weights by a -- so alpha was applied
				// TWICE. Every partly-covered pixel came out at a*a instead of
				// a, which dragged all the antialiased edges toward the dark
				// backing. That is what read as grey contours; the masks
				// themselves are white, saturation 0, with no grey in them.
				uint32_t old = row[px];
				uint32_t ia = 255 - a;
				uint32_t r = ((uint32_t)cr * shade / 255 * a + ((old >> 16) & 0xff) * ia) / 255;
				uint32_t g = ((uint32_t)cg * shade / 255 * a + ((old >> 8) & 0xff) * ia) / 255;
				uint32_t b = ((uint32_t)cb * shade / 255 * a + (old & 0xff) * ia) / 255;
				row[px] = 0xff000000u | (r << 16) | (g << 8) | b;
			}
		}
	}

	// Wireframe for a LIVE unit, with per-part damage colour (item 18.1).
	//
	// The union of all cells is the complete outline and is drawn first at the
	// HP-band tint; the individual cells are body parts, and a share of them
	// proportional to missing HP is overdrawn in the damage colour. Degrades to
	// the plain uniform tint on the SD path, where wirefram.grp has one frame
	// per unit and no parts at all.
	//
	// v0.73 drew cell 0 alone instead of the union, on the strength of a comment
	// that claimed cell 0 was the complete outline. It is not -- the wireframe
	// came out as a few disconnected fragments. Fixed in v0.74; the lesson is
	// that the comment contradicted both the adjacent line and the code it
	// described, and that should have stopped the change rather than the
	// inability to test it merely being noted.
	//
	// Which parts: retail uses wireframe_randomizer to scatter them. We use it
	// as a starting offset and take a contiguous run, which is a simplification
	// of whatever retail's exact selection is -- it gives the same property that
	// matters (two identical units at identical HP look different) without
	// pretending to reproduce a table we have not read.
	// v0.82: draw from one of the smaller dedicated sets (grpwire for the group
	// grid, tranwire for transport cargo). They are shorter than wirefram.grp
	// -- 131 and 106 frames against 228 -- so anything absent falls back to the
	// full-size set rather than vanishing.
	void blit_wireframe_small(uint32_t* data, size_t pitch, const unit_t* u, int dstx, int dsty, int box, const grp_t& src) {
		// Faze 4.7c: the group grid uses the HD wireframe when HD assets are active
		// (matches the single-unit display). The intermittent crash here was an
		// out-of-bounds read in scale_box on an undersized (partial) DDS decode
		// under FULL HD memory pressure -- now guarded in scale_box +
		// get_hd_wireframe_cells, so the worst case is a one-frame SD fallback.
		if (hd_casc || !hd_ui_dir.empty()) { blit_wireframe_unit(data, pitch, u, dstx, dsty, box); return; }
		int fi = u->unit_type ? (int)u->unit_type->id : -1;
		if (fi < 0 || fi >= (int)src.frames.size() || src.frames[fi].size.x == 0) {
			blit_wireframe_unit(data, pitch, u, dstx, dsty, box);
			return;
		}
		draw_wire_rim(data, pitch, u, dstx, dsty, box);
		blit_wire_grp(data, pitch, src, fi, dstx, dsty, box, wire_palette(u));
	}

	void blit_wireframe_unit(uint32_t* data, size_t pitch, const unit_t* u, int dstx, int dsty, int box) {
		int hpp = unit_hp_percent(u);
		wire_colors_t wc = wire_palette(u);
		auto* wf = get_hd_wireframe_cells((int)u->unit_type->id, box);
		// v0.82.026: layered path. The mask cells are drawn one at a time in
		// their own colours; the artwork cell is NOT drawn, because it is the
		// pre-composed picture and painting it under the layers is exactly what
		// made every previous colour choice look wrong.
		if (wf && wc.hd_layered && !wf->parts.empty()) {
			// v0.82.031: no rim on the layered path. It was invented -- nothing
			// in the strip or in twire.pcx asks for an extra outline, and the
			// art already carries its own contour layers.
			// A dark backing under the whole silhouette. Without it the layers
			// sit straight on the console's own grey and read as washed out --
			// the colours have nothing to be bright against.
			blit_hd_wireframe_cell(data, pitch, wf->outline, dstx, dsty,
				0xff000000u | wc.hd_fill, nullptr, 255);
			// No extra darkening here: v0.82.028 moved damage into the ramp
			// itself, so the layer colours already carry it. Dimming on top of
			// that would just mute the spectrum it is meant to walk through.
			for (size_t i = 0; i != wf->parts.size(); ++i) {
				uint32_t col = i < 6 ? wc.hd_layer[i] : wc.hd_line;
				if (!col) continue;                       // transparent layer
				// Full strength: the masks are near-white, and letting their own
				// luminance scale the colour was costing ~12% of every one.
				blit_hd_wireframe_cell(data, pitch, wf->parts[i], dstx, dsty,
					0xff000000u | col, nullptr, 255);
			}
			// v0.83.032: heroes get the bright outermost outline even here (the
			// layered path otherwise draws no rim). draw_wire_rim traces wf->outline.
			if (ut_flag(u->unit_type, unit_type_t::flag_hero))
				draw_wire_rim(data, pitch, u, dstx, dsty, box, &wf->outline);
			return;
		}
		if (!wf || wf->parts.size() < 2) {
			// SD: the rim builds its own mask from wirefram.grp.
			draw_wire_rim(data, pitch, u, dstx, dsty, box);
			blit_wireframe_type(data, pitch, u->unit_type, dstx, dsty, box, wc);
			return;
		}
		// HD: hand the rim the HD silhouette so it traces the shape that is
		// actually drawn. This is also the only path on which Protoss gets a
		// rim at all -- see draw_wire_rim.
		draw_wire_rim(data, pitch, u, dstx, dsty, box, &wf->outline);
		// v0.82.010: was `wc.blend ? wc.tint : 0x30f030u`, which handed a Zerg at
		// full HP (blend 0) a flat hardcoded green and discarded wc entirely.
		// Pass wc instead so HD keys off luminance the way SD keys off index.
		// The base must be the UNION of every cell, not cell 0. v0.73 drew only
		// cell 0 on the strength of a comment claiming it was the complete
		// outline; the same comment block also said the outline is the cells'
		// union, the pre-v0.73 code did the union, and in game it turned out
		// cell 0 is just another fragment -- the wireframe rendered as a few
		// disconnected pieces. Union first, damage on top.
		// v0.82.012: the base layer is the FULL-HEALTH colouring, not the current
		// HP band. Drawing the base at the current band recoloured the entire
		// wireframe at once, which is exactly what a damaged SCV should not do --
		// BW switches body parts over one at a time and leaves the rest alone.
		// The measurement behind this: wirefram.grp's four band indices
		// (208..211 for Terran/Protoss) are four spatially DISJOINT fragments of
		// the unit, not one silhouette in four colours -- rendered and confirmed
		// with `aidump wireband`. The HD strip's 8 cells are the same idea.
		wire_colors_t base = wire_palette(u, 100);
		blit_hd_wireframe_cell(data, pitch, wf->outline, dstx, dsty, 0, &base);
		int parts = (int)wf->parts.size();
		int damaged = (100 - hpp) * parts / 100;
		if (damaged <= 0) return;
		if (damaged >= parts) damaged = parts - 1;  // never black out the whole unit
		int start = (int)((unsigned)u->wireframe_randomizer % (unsigned)parts);
		// Damaged parts take the current HP band, so they go yellow first and
		// red only when the unit is nearly dead -- the colour and the count both
		// track damage instead of the colour alone.
		for (int k = 0; k != damaged; ++k)
			blit_hd_wireframe_cell(data, pitch, wf->parts[(start + k) % parts], dstx, dsty, 0, &wc);
	}

	// Scale + tint a wireframe (by unit type) into a square box at (dstx,dsty).
	// Draws the wireframe as edge->fill lerped by the art's own luminance.
	// Damage is carried by the colours themselves (wire_palette), not by
	// blanking parts of the shape.
	void blit_wireframe_type(uint32_t* data, size_t pitch, const unit_type_t* ut, int dstx, int dsty, int box, wire_colors_t wc) {
		int fi = (int)ut->id;
		if (ddsgrp::frame_t* hw = get_hd_wireframe(fi, box)) {
			int tr = (wc.tint >> 16) & 0xff, tg = (wc.tint >> 8) & 0xff, tb = wc.tint & 0xff;
			for (int y = 0; y != hw->h; ++y) {
				int py = dsty + y;
				if (py < 0 || py >= (int)screen_height) continue;
				uint32_t* row = data + (size_t)py * pitch;
				const uint32_t* srow = &hw->rgba[(size_t)y * hw->w];
				for (int x = 0; x != hw->w; ++x) {
					int px = dstx + x;
					if (px < 0 || px >= (int)screen_width) continue;
					uint32_t c = srow[x];
					uint32_t a = c >> 24;
					if (a < 16) continue;
					uint32_t lum = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
					lum = lum * a / 255;
					uint32_t old = row[px];
					uint32_t ia = 255 - a;
					uint32_t r = ((uint32_t)tr * lum / 255 * a + ((old >> 16) & 0xff) * ia) / 255;
					uint32_t g = ((uint32_t)tg * lum / 255 * a + ((old >> 8) & 0xff) * ia) / 255;
					uint32_t b = ((uint32_t)tb * lum / 255 * a + (old & 0xff) * ia) / 255;
					row[px] = 0xff000000u | (r << 16) | (g << 8) | b;
				}
			}
			return;
		}
		blit_wire_grp(data, pitch, wireframe_grp, fi, dstx, dsty, box, wc);
	}

	// Blit one frame of any of the three wireframe sets, scaled into `box`.
	// Dark pixels of the source take `edge`, bright ones `fill`; for Terran and
	// Protoss the two are equal, so it collapses to the familiar single tint.
	void blit_wire_grp(uint32_t* data, size_t pitch, const grp_t& grp, int fi, int dstx, int dsty, int box, wire_colors_t wc) {
		if (fi < 0 || fi >= (int)grp.frames.size()) return;
		auto& f = grp.frames[fi];
		size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return;
		wireframe_scratch.assign(fw * fh, 0);
		draw_frame(f, false, wireframe_scratch.data(), fw, 0, 0, fw, fh);
		int dw = box, dh = box;
		if (fw > fh) dh = (int)((int64_t)box * fh / fw);
		else if (fh > fw) dw = (int)((int64_t)box * fw / fh);
		int px0 = dstx + (box - dw) / 2, py0 = dsty + (box - dh) / 2;
		int tr = (wc.tint >> 16) & 0xff, tg = (wc.tint >> 8) & 0xff, tb = wc.tint & 0xff;
		int bl = wc.blend < 0 ? 0 : (wc.blend > 100 ? 100 : wc.blend);
		for (int y = 0; y < dh; ++y) {
			int py = py0 + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = wireframe_scratch.data() + (size_t)((int64_t)y * fh / dh) * fw;
			for (int x = 0; x < dw; ++x) {
				uint8_t idx = srow[(int64_t)x * fw / dw];
				if (idx == 0) continue;
				int px = px0 + x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t rgb = wireframe_palette_entry(idx);
				int cr, cg, cb;
				if ((idx == 192 || idx == 193) && wc.shield_hi) {
					uint32_t s = idx == 192 ? wc.shield_lo : wc.shield_hi;
					row[px] = 0xff000000u | s;
					continue;
				}
				if (wc.literal) {
					cr = (rgb >> 16) & 0xff; cg = (rgb >> 8) & 0xff; cb = rgb & 0xff;
				} else {
					int lum = (((rgb >> 16) & 0xff) * 77 + ((rgb >> 8) & 0xff) * 150 + (rgb & 0xff) * 29) >> 8;
					lum = lum * 3 / 2 + 64;
					if (lum > 255) lum = 255;
					cr = tr * lum / 255; cg = tg * lum / 255; cb = tb * lum / 255;
				}
				if (wc.literal && bl) {
					cr += (tr - cr) * bl / 100;
					cg += (tg - cg) * bl / 100;
					cb += (tb - cb) * bl / 100;
				}
				row[px] = 0xff000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
			}
		}
	}

	// The outline drawn around the silhouette. Two races use it for different
	// reasons, which is why it is one function and not a "shield rim":
	//   Protoss -- cyan, scaled by the SHIELD pool, gone entirely at zero.
	//   Zerg    -- red, always there; it is part of how the creature looks.
	//   Terran  -- none.
	void draw_wire_rim(uint32_t* data, size_t pitch, const unit_t* u, int dstx, int dsty, int box,
	                   const ddsgrp::frame_t* hd_shape = nullptr) {
		uint32_t c;
		// v0.82.029: Zerg gets a ONE pixel red rim. It was 2px because pad
		// doubles as both the mask margin and the neighbour search radius, so a
		// thicker rim came for free and nobody had asked for it.
		int pad = 2;
		// v0.83.032: a hero's wireframe gets a bright OUTERMOST outline LAYER (not
		// a rectangular frame) so a named unit stands out among identical
		// silhouettes. Protoss/shielded heroes get a neon CYAN outline; everyone
		// else white. Drawn on every path (see the hero call on the HD layered
		// path too), and it overrides the race/shield rim below.
		bool hero = u->unit_type && ut_flag(u->unit_type, unit_type_t::flag_hero);
		if (hero) {
			c = (u->unit_type->has_shield && u->unit_type->shield_points > 0)
			    ? 0xff20f0ffu : 0xffffffffu;
			pad = 2;
		} else if (u->unit_type && unit_race(u) == race_t::zerg) {
			c = 0xff2020u;
			pad = 1;
		} else if (!hd_shape) {
			// SD keeps the approved v0.82.008 behaviour: no Protoss outline,
			// because there the shield lights indices 192/193 inside the drawing.
			// HD has no indices, so it falls through to the shield rim below --
			// which is why the user saw no blue on a Protoss unit in HD at all.
			return;
		} else {
			if (!u->unit_type->has_shield || u->unit_type->shield_points <= 0) return;
			int sp = (int)u->shield_points.integer_part() * 100 / u->unit_type->shield_points;
			if (sp <= 0) return;
			if (sp > 100) sp = 100;
			// The authentic shield ramp, read out of game\twire.pcx: its 24
			// entries group into runs, and one of them is a seven-step BLUE
			// fading to near black. The earlier #40e0ff cyan was invented --
			// this is what the file actually holds. Its dark end matches the
			// art's own indices 192/193 (#0c1424, #243c64) byte for byte,
			// which is why those two live in the drawing at all.
			static const uint32_t shield_ramp[] = {
				0x001034u, 0x042064u, 0x042064u, 0x083498u, 0x083498u, 0x0c48ccu, 0x0c48ccu
			};
			int n = (int)(sizeof(shield_ramp) / sizeof(shield_ramp[0]));
			int step = sp * (n - 1) / 100;
			c = shield_ramp[step];
		}
		// A real outline, not offset copies of the shape. The first attempt
		// blitted the whole silhouette twelve times underneath, and since the
		// wireframe is a filled drawing rather than thin line art, those copies
		// showed through as a massive cyan glow INSIDE the unit. Build a
		// coverage mask instead and light only empty pixels that sit next to a
		// covered one, which cannot leak inwards by construction.
		// v0.82.012: the mask must come from the SAME art the drawing does. On
		// the HD path it was still built from the SD wirefram.grp silhouette
		// scaled to the box while the unit itself was drawn from
		// wirefram.dds.grp -- two different shapes, so the Zerg rim sat wrong
		// against its own interior and read as a deformed second wireframe.
		int dw, dh, mw, mh;   // pad chosen above: 1px for Zerg, 2px otherwise
		a_vector<uint8_t> mask;
		if (hd_shape) {
			dw = hd_shape->w; dh = hd_shape->h;
			if (dw <= 0 || dh <= 0) return;
			mw = dw + pad * 2; mh = dh + pad * 2;
			mask.assign((size_t)mw * mh, 0);
			for (int y = 0; y != dh; ++y)
				for (int x = 0; x != dw; ++x)
					if ((hd_shape->rgba[(size_t)y * dw + x] >> 24) >= 16)
						mask[(size_t)(y + pad) * mw + (x + pad)] = 1;
		} else {
			int fi = (int)u->unit_type->id;
			if (fi < 0 || fi >= (int)wireframe_grp.frames.size()) return;
			auto& f = wireframe_grp.frames[fi];
			size_t fw = f.size.x, fh = f.size.y;
			if (!fw || !fh) return;
			wireframe_scratch.assign(fw * fh, 0);
			draw_frame(f, false, wireframe_scratch.data(), fw, 0, 0, fw, fh);
			dw = box; dh = box;
			if (fw > fh) dh = (int)((int64_t)box * fh / fw);
			else if (fh > fw) dw = (int)((int64_t)box * fw / fh);
			if (dw <= 0 || dh <= 0) return;
			mw = dw + pad * 2; mh = dh + pad * 2;
			mask.assign((size_t)mw * mh, 0);
			for (int y = 0; y != dh; ++y) {
				const uint8_t* srow = wireframe_scratch.data() + (size_t)((int64_t)y * fh / dh) * fw;
				for (int x = 0; x != dw; ++x)
					if (srow[(int64_t)x * fw / dw]) mask[(size_t)(y + pad) * mw + (x + pad)] = 1;
			}
		}
		// Only pixels reachable from the border are OUTSIDE. Without this the
		// rim floods the interior: the earlier version lit any empty pixel next
		// to a covered one and claimed that "cannot leak inwards by
		// construction", which holds for a solid silhouette but not for line
		// art. HD wireframes are line art full of interior gaps, and every one
		// of those gaps touches a line -- so a Zerg Hatchery came out as a red
		// mass with green showing through.
		{
			a_vector<int> stack;
			auto push = [&](int x, int y) {
				size_t i = (size_t)y * mw + x;
				if (mask[i]) return;
				mask[i] = 2;                       // 2 = known outside
				stack.push_back(y * mw + x);
			};
			for (int x = 0; x != mw; ++x) { push(x, 0); push(x, mh - 1); }
			for (int y = 0; y != mh; ++y) { push(0, y); push(mw - 1, y); }
			while (!stack.empty()) {
				int v = stack.back(); stack.pop_back();
				int x = v % mw, y = v / mw;
				if (x > 0) push(x - 1, y);
				if (x + 1 < mw) push(x + 1, y);
				if (y > 0) push(x, y - 1);
				if (y + 1 < mh) push(x, y + 1);
			}
		}
		int px0 = dstx + (box - dw) / 2 - pad, py0 = dsty + (box - dh) / 2 - pad;
		for (int y = 0; y != mh; ++y) {
			int py = py0 + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			for (int x = 0; x != mw; ++x) {
				if (mask[(size_t)y * mw + x] != 2) continue;   // covered, or an interior gap
				bool near = false;
				for (int dy = -pad; dy <= pad && !near; ++dy)
					for (int dx = -pad; dx <= pad; ++dx) {
						int ny = y + dy, nx = x + dx;
						if (ny < 0 || ny >= mh || nx < 0 || nx >= mw) continue;
						if (mask[(size_t)ny * mw + nx] == 1) { near = true; break; }
					}
				if (!near) continue;
				int px = px0 + x;
				if (px < 0 || px >= (int)screen_width) continue;
				row[px] = 0xff000000u | c;
			}
		}
	}

	static uint32_t hp_tint(int hpp) {
		return hpp > 66 ? 0x30f030u : (hpp > 33 ? 0xf0f030u : 0xf03030u);
	}

	// Scale an 0xRRGGBB tint by a percentage, used for the shield halo and for
	// darkening damaged wireframe rows.
	static uint32_t scale_rgb(uint32_t c, int pct) {
		if (pct < 0) pct = 0; if (pct > 100) pct = 100;
		uint32_t r = ((c >> 16) & 0xff) * (uint32_t)pct / 100;
		uint32_t g = ((c >> 8) & 0xff) * (uint32_t)pct / 100;
		uint32_t b = (c & 0xff) * (uint32_t)pct / 100;
		return (r << 16) | (g << 8) | b;
	}

	// When several units are selected, the center panel shows a grid of small
	// HP-tinted wireframes (up to 12) with an HP bar under each, like the
	// original group view - instead of a single big wireframe + stat lines.
	// #4: cached grid-cell -> unit map for the multi-selection wireframe grid, so
	// a tap can drill down to a single unit (its 3x3 card + portrait). Rebuilt on
	// every draw_selection_group; cleared when the selection is a single unit.
	struct sel_grid_hit_t { rect r; unit_id uid; };
	a_vector<sel_grid_hit_t> selection_grid_hit;

	// Shared geometry for the multi-selection wireframe grid AND the SCBW2 sub-card
	// selector column, so the two never disagree. The wireframe icons are packed with a
	// minimal gap, shrunk ~15% from the fit size, and RIGHT-aligned in the Unit Display
	// (a small margin from the right edge); the space that frees up on the LEFT is the
	// sub-card selector column [panel_left, grid_left).
	struct sel_grid_geom_t { int ox, oy, cell_w, cell_h, box, panel_left; };
	sel_grid_geom_t sel_grid_geom() {
		sel_grid_geom_t g{};
		const int cols = 6, rows = 2;
		xy o = console_to_screen(134.0f, 374.0f);
		int gx1 = console_to_screen(398.0f, 0.0f).x;
		int gy1 = console_to_screen(0.0f, 466.0f).y;
		int cw0 = (gx1 - o.x) / cols, ch = (gy1 - o.y) / rows;
		int fit = (cw0 - 4 < ch - 8 ? cw0 - 4 : ch - 8);
		if (fit < 4) fit = 4;
		g.box = fit * 85 / 100;                 // shrink icons 15%
		g.cell_w = g.box + 3;                   // minimal gap
		g.cell_h = g.box + 6;                   // + HP bar
		int grid_w = cols * g.cell_w;
		int right_margin = 6;                   // don't touch the panel's right edge
		g.ox = gx1 - right_margin - grid_w;     // right-align
		g.panel_left = o.x;
		if (g.ox < g.panel_left) g.ox = g.panel_left;
		g.oy = o.y;
		return g;
	}

	void draw_selection_group(uint32_t* data, size_t pitch) {
		selection_grid_hit.clear();
		a_vector<unit_t*> us;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) { us.push_back(u); if (us.size() >= 12) break; }
		}
		if (us.empty()) return;
		load_wireframe();
		if (!wireframe_ok) return;
		const int cols = 6;
		sel_grid_geom_t g = sel_grid_geom();
		if (g.box < 4) return;
		for (size_t i = 0; i < us.size(); ++i) {
			unit_t* u = us[i];
			int col = (int)(i % cols), rowi = (int)(i / cols);
			int cx = g.ox + col * g.cell_w, cy = g.oy + rowi * g.cell_h;
			selection_grid_hit.push_back({rect{{cx, cy}, {cx + g.cell_w, cy + g.cell_h}}, get_unit_id(u)});
			int hpp = unit_hp_percent(u);
			uint32_t tint = hp_tint(hpp);
			blit_wireframe_type(data, pitch, u->unit_type, cx + (g.cell_w - g.box) / 2, cy, g.box, tint);
			int bx = cx + (g.cell_w - g.box) / 2, by = cy + g.box + 2, bw = g.box;
			for (int t = 0; t < 3; ++t) {
				int yy = by + t;
				if (yy < 0 || yy >= (int)screen_height) continue;
				uint32_t* r = data + (size_t)yy * pitch;
				for (int x = 0; x < bw; ++x) {
					int xx = bx + x;
					if (xx < 0 || xx >= (int)screen_width) continue;
					r[xx] = (x * 100 / (bw < 1 ? 1 : bw)) < hpp ? (tint | 0xff000000u) : 0xff202020u;
				}
			}
		}
	}

	// Draw the primary selected unit's wireframe at the bottom-center of the
	// screen, HP-tinted, with an "HP cur/max" line beneath it.
	void draw_wireframe_panel(uint32_t* data, size_t pitch) {
		// A multi-unit selection shows a wireframe grid instead of one unit.
		int own = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) ++own;
		}
		if (own > 1) { draw_selection_group(data, pitch); return; }
		selection_grid_hit.clear(); // single/empty selection has no drill-down grid
		unit_t* u = first_own_selected_unit();
		if (!u) return;
		load_wireframe();
		if (!wireframe_ok) return;
		int fi = (int)u->unit_type->id;
		if (fi < 0 || fi >= (int)wireframe_grp.frames.size()) return;
		auto& f = wireframe_grp.frames[fi];
		size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return;
		wireframe_scratch.assign(fw * fh, 0);
		draw_frame(f, false, wireframe_scratch.data(), fw, 0, 0, fw, fh);

		int hpp = unit_hp_percent(u);
		uint32_t tint = hpp > 66 ? 0x30f030u : (hpp > 33 ? 0xf0f030u : 0xf03030u);
		int tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;

		int line_h = ui::unispace_h + 2;

		// Authentic BW unit panel, all inside the center console panel (console
		// space, 640x480): unit name centered near the panel top, the wireframe
		// in its slot on the left (scaled to fill, HP-tinted), and HP/shield/
		// energy stacked under the wireframe.
		// Unit name centered across the Unit Display panel, near its top.
		load_unit_names();
		a_string name = unit_name(u->unit_type);
		if (!name.empty()) {
			int nw = (int)name.size() * (ui::unispace_w + 1);
			int cx = console_to_screen(262.0f, 0.0f).x;
			int ny = console_to_screen(0.0f, 372.0f).y;
			draw_text_rgba(data, pitch, cx - nw / 2, ny, name.c_str(), 0xffe0e0e0u);
		}

		// Wireframe scaled into its slot (aspect-preserved), HP-tinted.
		xy ws = console_to_screen(162.0f, 396.0f);
		int slot = console_len(58.0f);
		int dw = slot, dh = slot;
		if (fw > fh) dh = (int)((int64_t)slot * fh / fw);
		else if (fh > fw) dw = (int)((int64_t)slot * fw / fh);
		int px0 = ws.x + (slot - dw) / 2, py0 = ws.y + (slot - dh) / 2;
		for (int y = 0; y < dh; ++y) {
			int py = py0 + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = wireframe_scratch.data() + (size_t)((int64_t)y * fh / dh) * fw;
			for (int x = 0; x < dw; ++x) {
				uint8_t idx = srow[(int64_t)x * fw / dw];
				if (idx == 0) continue;
				int px = px0 + x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t rgb = game_palette[idx];
				int lum = (((rgb >> 16) & 0xff) * 77 + ((rgb >> 8) & 0xff) * 150 + (rgb & 0xff) * 29) >> 8;
				// Brighten so the wireframe reads as vividly as the stat text
				// (the source line-art is mid-luminance).
				lum = lum * 3 / 2 + 64;
				if (lum > 255) lum = 255;
				row[px] = 0xff000000u | ((uint32_t)(tr * lum / 255) << 16)
					| ((uint32_t)(tg * lum / 255) << 8) | (uint32_t)(tb * lum / 255);
			}
		}

		// Stat lines centered under the wireframe: HP, then shields / energy.
		int wf_cx = ws.x + slot / 2;
		int ty = ws.y + slot + 4;
		char line[48];
		auto stat_line = [&](const char* s, uint32_t color) {
			int tw = (int)strlen(s) * (ui::unispace_w + 1);
			draw_text_rgba(data, pitch, wf_cx - tw / 2, ty, s, color);
			ty += line_h;
		};
		snprintf(line, sizeof(line), "HP %d/%d", (int)u->hp.integer_part(), (int)u->unit_type->hitpoints.integer_part());
		stat_line(line, tint | 0xff000000u);
		if (u->unit_type->has_shield) {
			snprintf(line, sizeof(line), "SH %d/%d", (int)u->shield_points.integer_part(), u->unit_type->shield_points);
			stat_line(line, 0xff5cb0ffu);
		}
		if (u->unit_type->flags & unit_type_t::flag_has_energy) {
			snprintf(line, sizeof(line), "EN %d/%d", (int)u->energy.integer_part(), (int)unit_max_energy(u).integer_part());
			stat_line(line, 0xffc884ffu);
		}

		// A producing building shows its training/production queue where a combat
		// unit shows its attack/armor stats.
		if ((u->unit_type->flags & unit_type_t::flag_building) && !u->build_queue.empty()) {
			draw_building_queue(data, pitch, u);
		} else {
			// Combat stats on the right of the panel, like the original stat
			// icons: an amber marker + weapon damage, a cyan marker + armor.
			const weapon_type_t* gw = u->unit_type->ground_weapon;
			const weapon_type_t* aw = u->unit_type->air_weapon;
			int dmg = gw && gw->damage_amount > 0 ? gw->damage_amount
				: (aw && aw->damage_amount > 0 ? aw->damage_amount : 0);
			int hits = gw && gw->damage_amount > 0 ? u->unit_type->max_ground_hits
				: (aw && aw->damage_amount > 0 ? u->unit_type->max_air_hits : 1);
			if (hits < 1) hits = 1;
			int sx0 = console_to_screen(300.0f, 0.0f).x;
			int sy0 = console_to_screen(0.0f, 400.0f).y;
			int ico = console_len(9.0f);
			auto stat_icon = [&](int yy, uint32_t mark, const char* text) {
				fill_rect_rgba(data, pitch, {{sx0, yy}, {sx0 + ico, yy + ico}}, mark);
				line_rectangle_rgba(data, pitch, {{sx0, yy}, {sx0 + ico, yy + ico}}, 0xff101418u);
				draw_text_rgba(data, pitch, sx0 + ico + 5, yy + (ico - ui::unispace_h) / 2, text, 0xffe0e0e0u);
			};
			if (dmg > 0) {
				if (hits > 1) snprintf(line, sizeof(line), "%dx%d", dmg, hits);
				else snprintf(line, sizeof(line), "%d", dmg);
				stat_icon(sy0, 0xffe0b020u, line);
			}
			snprintf(line, sizeof(line), "%d", u->unit_type->armor);
			stat_icon(sy0 + line_h + 4, 0xff5cb0ffu, line);
		}
	}

	// A producing building's queue: up to 5 small wireframes of the units it is
	// training, drawn in a row to the right of the building wireframe. The first
	// (currently training) gets a green build-progress bar underneath.
	void draw_building_queue(uint32_t* data, size_t pitch, unit_t* u) {
		load_wireframe();
		if (!wireframe_ok) return;
		int n = (int)u->build_queue.size();
		if (n > 5) n = 5;
		xy o = console_to_screen(232.0f, 392.0f);
		int cell = console_len(30.0f);
		int box = cell - console_len(4.0f);
		for (int i = 0; i < n; ++i) {
			const unit_type_t* ut = u->build_queue[i];
			if (!ut) continue;
			int fi = (int)ut->id;
			if (fi < 0 || fi >= (int)wireframe_grp.frames.size()) continue;
			int cx = o.x + i * cell, cy = o.y;
			line_rectangle_rgba(data, pitch, {{cx, cy}, {cx + box, cy + box}}, 0xff405060u);
			// A temporary wireframe-blit for an arbitrary unit type (not the
			// selected unit), tinted green.
			blit_wireframe_type(data, pitch, ut, cx + 2, cy + 2, box - 4, 0x30f030u);
			// Build-progress bar under the first (currently training) unit.
			if (i == 0 && ut->build_time > 0) {
				int done = ut->build_time - u->remaining_build_time;
				if (done < 0) done = 0;
				if (done > ut->build_time) done = ut->build_time;
				int bw = box, bx = cx, by = cy + box + 2;
				for (int t = 0; t < 3; ++t) {
					int yy = by + t;
					if (yy < 0 || yy >= (int)screen_height) continue;
					uint32_t* r = data + (size_t)yy * pitch;
					for (int x = 0; x < bw; ++x) {
						int xx = bx + x;
						if (xx < 0 || xx >= (int)screen_width) continue;
						r[xx] = (x * ut->build_time / (bw < 1 ? 1 : bw)) < done ? 0xff40ff70u : 0xff202020u;
					}
				}
			}
		}
	}

	// Center-right console: a thin MENU button on top, the unit Portrait slot
	// below it (per the user's HUD layout — MENU no longer fills the portrait
	// area). The portrait art itself lands in v0.12 (unit *fid* GRP); for now
	// the slot is a recessed placeholder.
	rect menu_button_area() {
		if (!ui_legacy) return hud2_menu_panel();
		xy a = console_to_screen(400.0f, 384.0f);
		return {a, {a.x + console_len(92.0f), a.y + console_len(22.0f)}};
	}

	rect portrait_slot_area() {
		xy a = console_to_screen(400.0f, 410.0f);
		return {a, {a.x + console_len(92.0f), a.y + console_len(60.0f)}};
	}

	bool menu_button_hit(int mx, int my) {
		rect r = menu_button_area();
		return mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y;
	}

	void draw_menu_button(uint32_t* data, size_t pitch) {
		rect r = menu_button_area();
		fill_rect_rgba(data, pitch, r, hud_theme().util_fill);
		line_rectangle_rgba(data, pitch, r, hud_theme().util_line);
		const char* label = "MENU";
		int tw = (int)strlen(label) * (ui::unispace_w + 1);
		int cx = (r.from.x + r.to.x) / 2 - tw / 2;
		int cy = (r.from.y + r.to.y) / 2 - ui::unispace_h / 2;
		draw_text_rgba(data, pitch, cx, cy, label, 0xffd0d0d0u);
	}

	// Unit portrait slot: plays the authentic SMK talking-head video when a
	// unit with a portrait is selected, otherwise stays a recessed placeholder.
	void draw_portrait_slot(uint32_t* data, size_t pitch) {
		rect r = portrait_slot_area();
		if (r.to.x <= r.from.x) return;
		fill_rect_rgba(data, pitch, r, 0xff0a0e10u);
		line_rectangle_rgba(data, pitch, r, 0xff2c343c);
		draw_portrait_video(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.to.x - 1, r.to.y - 1}});
	}

	// --- Control groups (0-9) -------------------------------------------
	// A row of numbered buttons between the minimap and the Unit Display (the
	// "Groups" strip). Tap a button to recall that group; long-press to assign
	// the current selection to it. State is stored as unit ids so it survives
	// units dying (recall filters out the dead).
	a_vector<unit_id> control_groups[10];

	rect group_button_area(int n) {
		if (hud_v3) return n >= 0 && n < 10 ? hud3_layout().group[n] : rect{};
		if (!ui_legacy) {
			// HUD v2: 10 buttons in a HORIZONTAL row above the Unit Display.
			rect g = hud2_groups_strip();
			int gw = (g.to.x - g.from.x) / 10;
			return {{g.from.x + n * gw + 1, g.from.y}, {g.from.x + n * gw + gw - 1, g.to.y}};   // (v0.125: the row itself now sits under the accent line)
		}
		// legacy: 10 buttons across console x[136,398], near the top.
		int x0 = console_to_screen(136.0f, 0.0f).x;
		int x1 = console_to_screen(398.0f, 0.0f).x;
		int y = console_to_screen(0.0f, 356.0f).y;
		int h = console_len(20.0f);
		if (x1 <= x0 || h <= 0) return {};
		int cell = (x1 - x0) / 10;
		int bx = x0 + n * cell;
		return {{bx, y}, {bx + cell - 2, y + h}};
	}

	bool group_button_hit(int mx, int my, int& out_n) {
		for (int n = 0; n < 10; ++n) {
			rect r = group_button_area(n);
			if (r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) {
				out_n = n;
				return true;
			}
		}
		return false;
	}

	// SCBW2 unlimited_selection sub-card selectors (the "1/2/3" pages). A VERTICAL column
	// of numbered buttons down the LEFT edge of the Unit Display, in the space freed by
	// right-aligning the wireframe grid. ONLY present when unlimited_selection is on and
	// MORE than 12 units are selected (page count > 1), so vanilla / <=12 selections are
	// visually unchanged.
	rect sel_page_tab_area(int i, int count) {
		if (count <= 1 || i < 0 || i >= count) return {};
		hud2_grid_geom_t g = hud2_group_grid_geom();
		int freed = g.ox - g.panel_left;
		if (freed < 8) return {};
		int gap = hud_h_px(2);
		int avail_h = 2 * g.cell_h;                          // the 2-row grid height
		// Enhanced selection tops out at 6 pages (72 units): size every button to a
		// fixed ~half-icon height so all 6 stack uniformly in the panel height, and
		// keep the width ~one 3x3 icon. Fewer pages -> short buttons at the top.
		const int max_slots = 6;
		int bh = (avail_h - (max_slots - 1) * gap) / max_slots;
		int bw = g.cell_w;
		if (bw > freed - hud_h_px(2)) bw = freed - hud_h_px(2);
		if (bh < 6 || bw < 8) return {};
		int y = g.oy + i * (bh + gap);
		return {{g.panel_left, y}, {g.panel_left + bw, y + bh}};
	}
	void draw_sel_page_tabs(uint32_t* data, size_t pitch) {
		if (!unlimited_sel_on()) return;
		int count = wide_page_count();
		if (count <= 1) return;
		for (int i = 0; i < count; ++i) {
			rect r = sel_page_tab_area(i, count);
			if (r.to.x <= r.from.x) continue;
			bool active = i == sel_page;
			// Active page is GREENED like the QUEUE-ON button; the number is the HUD
			// accent yellow (same as the group-number labels / QUEUE / 3x3 icons) for a
			// consistent HUD look.
			fill_rect_rgba(data, pitch, r, active ? hud_theme().on_fill : hud_theme().off_fill);
			line_rectangle_rgba(data, pitch, r, active ? hud_theme().strip_on_line : hud_theme().off_line);
			char lbl[3]; int n = i + 1;
			if (n < 10) { lbl[0] = (char)('0' + n); lbl[1] = 0; } else { lbl[0] = '0' + n / 10; lbl[1] = '0' + n % 10; lbl[2] = 0; }
			int tw = (int)strlen(lbl) * (ui::unispace_w + 1);
			int cx = (r.from.x + r.to.x) / 2 - tw / 2;
			int cy = (r.from.y + r.to.y) / 2 - ui::unispace_h / 2;
			draw_text_rgba(data, pitch, cx, cy, lbl, hud_theme().amber);
		}
	}
	int sel_page_tab_index_at(int mx, int my) {   // -1 if none; non-mutating
		if (!unlimited_sel_on() || hud_v3) return -1;
		int count = wide_page_count();
		if (count <= 1) return -1;
		for (int i = 0; i < count; ++i) {
			rect r = sel_page_tab_area(i, count);
			if (r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) return i;
		}
		return -1;
	}
	bool sel_page_tab_hit_test(int mx, int my) { return sel_page_tab_index_at(mx, my) >= 0; }
	bool sel_page_tab_hit(int mx, int my) {   // tap action: switch the active sub-card
		int i = sel_page_tab_index_at(mx, my);
		if (i < 0) return false;
		sel_page = i; rebuild_active_page(); return true;
	}

	// #4: which selected unit (if any) is under a tap on the multi-selection grid.
	unit_t* selection_grid_unit_at(int mx, int my) {
		for (auto& h : selection_grid_hit)
			if (mx >= h.r.from.x && mx < h.r.to.x && my >= h.r.from.y && my < h.r.to.y)
				return get_unit(h.uid);
		return nullptr;
	}

	void draw_group_buttons(uint32_t* data, size_t pitch) {
		for (int n = 0; n < 10; ++n) {
			rect r = group_button_area(n);
			if (r.to.x <= r.from.x) continue;
			bool filled = !control_groups[n].empty();
			fill_rect_rgba(data, pitch, r, filled ? 0xf0203848u : 0xc0141a20u);
			line_rectangle_rgba(data, pitch, r, filled ? 0xff6088b0u : 0xff3a4048u);
			// #3: a filled group shows the icon of its first living own unit, with
			// the group number as a small corner badge; empty groups keep the plain
			// centered number.
			unit_t* first = nullptr;
			if (filled) {
				for (auto uid : control_groups[n]) {
					unit_t* u = get_unit(uid);
					if (u && u->owner == game_local_player) { first = u; break; }
				}
			}
			char lbl[2] = { (char)('0' + n), 0 };
			int lw = ui::unispace_w + 1;
			if (first && cmdicons_ok) {
				draw_cmdicon_fit(data, pitch, (int)first->unit_type->id, r, false, 8);
				// Number badge in the top-left corner over a dark chip.
				rect badge = {{r.from.x + 1, r.from.y + 1}, {r.from.x + 1 + lw + 3, r.from.y + 2 + ui::unispace_h}};
				fill_rect_rgba(data, pitch, badge, 0xc0000000u);
				draw_text_rgba(data, pitch, r.from.x + 3, r.from.y + 2, lbl, 0xffe0e8f0u);
			} else {
				draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - lw / 2,
					(r.from.y + r.to.y) / 2 - ui::unispace_h / 2, lbl,
					filled ? 0xffe0e8f0u : 0xff808890u);
			}
		}
	}

	// Recall (tap): select the group's still-living units.
	void recall_control_group(int n) {
		if (n < 0 || n >= 10) return;
		current_selection_clear();
		for (auto uid : control_groups[n]) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) current_selection_add(u);
		}
		play_unit_what(first_own_selected_unit());
	}

	// Assign (long-press): store the current selection into the group. With
	// unlimited_selection, store the whole WIDE set (not just the active sub-card) so
	// recall restores the full group; recall_control_group re-adds through
	// current_selection_add, which rebuilds the wide set + paging.
	void assign_control_group(int n) {
		if (n < 0 || n >= 10) return;
		const a_vector<unit_id>& src = unlimited_sel_on() && !wide_selection.empty() ? wide_selection : current_selection;
		control_groups[n].assign(src.begin(), src.end());
	}

	// task 8: number of still-living, owned members of a group (drives the count
	// badge and the auto-clear).
	int control_group_live_count(int n) const {
		if (n < 0 || n >= 10) return 0;
		int c = 0;
		for (auto uid : control_groups[n]) {
			const unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) ++c;
		}
		return c;
	}
	// task 8: the first still-living, owned member (drives the wireframe badge).
	unit_t* control_group_first(int n) {
		if (n < 0 || n >= 10) return nullptr;
		for (auto uid : control_groups[n]) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) return u;
		}
		return nullptr;
	}
	// task 8(a): drop dead / no-longer-owned ids every frame so a group whose
	// members have all died auto-clears (its button reverts to the empty slot).
	void prune_control_groups() {
		for (int n = 0; n < 10; ++n) {
			auto& g = control_groups[n];
			for (size_t i = 0; i < g.size();) {
				const unit_t* u = get_unit(g[i]);
				if (!u || u->owner != game_local_player) g.erase(g.begin() + i);
				else ++i;
			}
		}
	}
	// task 8(c): ADD the selection to an existing group instead of overwriting it
	// -- up to 12, or up to the wide cap (72) when the mass-group rule
	// (unlimited_selection) is on. Deduplicates. Says so when not everything fit.
	void append_control_group(int n) {
		if (n < 0 || n >= 10) return;
		prune_control_groups();
		auto& g = control_groups[n];
		if (g.empty()) { assign_control_group(n); return; } // empty slot: just assign
		int cap = unlimited_sel_on() ? (int)wide_selection_cap : 12;
		const a_vector<unit_id>& src = unlimited_sel_on() && !wide_selection.empty() ? wide_selection : current_selection;
		int wanted = 0, added = 0;
		for (auto uid : src) {
			bool dup = false;
			for (auto e : g) if (e == uid) { dup = true; break; }
			if (dup) continue;
			++wanted;
			if ((int)g.size() >= cap) continue;
			g.push_back(uid); ++added;
		}
		if (added < wanted) {
			char m[64];
			snprintf(m, sizeof m, lang_lt ? "Grupe pilna: prideta %d is %d" : "Group full: added %d of %d", added, wanted);
			show_hud_message(m);
		}
	}

	// --- control-group hold menu (HUD v2; 2026-10-02) ---------------
	// Holding a group no longer overwrites it, and needs no modifier. With own units
	// selected a small menu opens above the group: lifting the finger on the group
	// ADDS the selection to it (the common case -- reinforcements), sliding to
	// REPLACE overwrites the group, sliding to CLEAR empties it. With nothing
	// selected only CLEAR is offered, and it stays up for a moment so it can be
	// tapped. An empty group is assigned at once, without a menu. Groups whose
	// units are all dead clear themselves (prune_control_groups), as before.
	enum { gm_none, gm_add, gm_replace, gm_clear };
	int group_menu_n = -1;                   // -1 = closed
	bool group_menu_has_sel = false;
	bool group_menu_held = false;            // the finger that opened it is still down
	xy group_menu_pos{-1, -1};               // that finger
	std::chrono::steady_clock::time_point group_menu_until{};
	rect group_menu_cell(int which) {
		if (group_menu_n < 0) return {};
		rect b = group_button_area(group_menu_n);
		int h = (b.to.y - b.from.y) * 2; if (h < hud_h_px(22)) h = hud_h_px(22);
		const int w = usp_adv() * 9 + hud_h_px(8), gap = hud_h_px(3);
		const int cells = group_menu_has_sel ? 3 : 1;   // REPLACE | + ADD | CLEAR, or CLEAR alone
		const int total = cells * w + (cells - 1) * gap;
		int x0 = (b.from.x + b.to.x) / 2 - total / 2;
		if (x0 + total > (int)screen_width - 2) x0 = (int)screen_width - 2 - total;
		if (x0 < 2) x0 = 2;
		const int y1 = b.from.y - gap, y0 = y1 - h;
		int idx = -1;
		// "+ ADD" sits in the middle, right above the group: it is what lifting the
		// finger where it is does, and the finger hides anything drawn under it.
		if (group_menu_has_sel) idx = which == gm_replace ? 0 : which == gm_add ? 1 : which == gm_clear ? 2 : -1;
		else idx = which == gm_clear ? 0 : -1;
		if (idx < 0) return {};
		return {{x0 + idx * (w + gap), y0}, {x0 + idx * (w + gap) + w, y1}};
	}
	// What a point is over: gm_replace / gm_clear (a menu cell), gm_add (the group
	// itself, when there is something to add), gm_none.
	int group_menu_hit(int mx, int my) {
		if (group_menu_n < 0) return gm_none;
		for (int which : {gm_replace, gm_add, gm_clear}) {
			rect r = group_menu_cell(which);
			if (r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) return which;
		}
		rect b = group_button_area(group_menu_n);
		// the group itself, and the strip between it and the menu, also mean ADD
		if (group_menu_has_sel && mx >= b.from.x && mx < b.to.x && my >= b.from.y - hud_h_px(3) && my < b.to.y) return gm_add;
		return gm_none;
	}
	// v0.125: a group is "currently selected" when the selection is exactly the
	// group's living units -- no more, no fewer. Shown green in the
	// group row for every race, like the green selection circle in the field.
	bool group_is_current(int n) {
		if (n < 0 || n >= 10) return false;
		const a_vector<unit_id>& sel = unlimited_sel_on() && !wide_selection.empty() ? wide_selection : current_selection;
		size_t live = 0;
		for (auto uid : control_groups[n]) {
			const unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player) continue;
			++live;
			if (std::find(sel.begin(), sel.end(), uid) == sel.end()) return false;
		}
		if (live == 0) return false;
		size_t own = 0;
		for (auto uid : sel) { const unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) ++own; }
		return own == live;
	}
	void group_menu_close() { group_menu_n = -1; group_menu_held = false; }
	void group_menu_open(int n) {
		if (n < 0 || n >= 10) return;
		prune_control_groups();
		const bool has_sel = first_own_selected_unit() != nullptr;
		if (control_groups[n].empty()) {              // empty group: assign, no menu
			if (has_sel) assign_control_group(n);
			return;
		}
		group_menu_n = n;
		group_menu_has_sel = has_sel;
		group_menu_held = true;
		rect b = group_button_area(n);
		group_menu_pos = xy((b.from.x + b.to.x) / 2, (b.from.y + b.to.y) / 2);
		group_menu_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	}
	void group_menu_apply(int action) {
		const int n = group_menu_n;
		if (n < 0) return;
		if (action == gm_add) append_control_group(n);
		else if (action == gm_replace) assign_control_group(n);
		else if (action == gm_clear) control_groups[n].clear();
		group_menu_close();
	}
	// The finger that opened the menu lifted at (mx, my).
	void group_menu_release(int mx, int my) {
		if (group_menu_n < 0 || !group_menu_held) return;
		group_menu_held = false;
		const int a = group_menu_hit(mx, my);
		if (a != gm_none) { group_menu_apply(a); return; }
		if (group_menu_has_sel) { group_menu_close(); return; }   // slid away: nothing happens
		// nothing selected: CLEAR stays up for a moment, to be tapped
		group_menu_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	}
	void group_menu_tick() {
		if (group_menu_n >= 0 && !group_menu_held && std::chrono::steady_clock::now() > group_menu_until) group_menu_close();
		if (group_menu_n >= 0 && control_groups[group_menu_n].empty()) group_menu_close();
	}
	void draw_group_menu(uint32_t* data, size_t pitch) {
		if (group_menu_n < 0) return;
		hud_theme_t th = hud_theme();
		const int hover = group_menu_held ? group_menu_hit(group_menu_pos.x, group_menu_pos.y) : gm_none;
		for (int which : {gm_replace, gm_add, gm_clear}) {
			rect r = group_menu_cell(which);
			if (r.to.x <= r.from.x) continue;
			const bool hot = hover == which;
			fill_rect_rgba(data, pitch, r, hot ? th.btn_pressed : th.btn_fill);
			line_rectangle_rgba(data, pitch, r, which == gm_clear ? th.ping_edge : hot ? th.amber : th.border);
			const char* lbl = which == gm_replace ? L("REPLACE") : which == gm_add ? L("+ ADD") : L("CLEAR");
			int tw = (int)strlen(lbl) * usp_adv();
			draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - tw / 2, (r.from.y + r.to.y) / 2 - usp_h() / 2, lbl,
				which == gm_clear ? th.ping_edge : th.amber);
		}
		// the group itself: releasing here adds
		rect b = group_button_area(group_menu_n);
		line_rectangle_rgba(data, pitch, b, group_menu_has_sel && (hover == gm_add || !group_menu_held) ? th.amber : th.grp_border);
		if (group_menu_has_sel)
			line_rectangle_rgba(data, pitch, {{b.from.x + 1, b.from.y + 1}, {b.to.x - 1, b.to.y - 1}}, hover == gm_add ? th.amber : th.grp_border);
	}

	// --- Pause menu (v0.10) ---------------------------------------------
	// The MENU button opens an in-game pause menu. In single player the game
	// pauses (is_paused); in LAN it keeps running. request_exit is picked up by
	// the host loop to leave the game (net-aware). Screens: 0 closed, 1 main,
	// 2 help, 3 controls, 4 tips, 5 objectives, 6 confirm-leave.
	int pause_screen = 0;
	bool request_exit = false;
	bool is_net_game = false;
	// Smoothed lockstep-barrier wait, in ms; the LAN latency readout under the
	// portrait. Fed by the net loop, meaningless (and unshown) in single player.
	int net_latency_ms = 0;
	a_string mission_objectives = "Destroy all of the opponent's buildings\nto eliminate him.";

	// Live mission objectives for the F10 menu. A trigger action (engine
	// mission_objectives_string) wins; then the campaign briefing's objectives;
	// then the default melee text. Fixes the campaign F10 showing the melee
	// objective (campaign audit, v0.30).
	a_string current_objectives() {
		int p = game_local_player >= 0 ? game_local_player : 0;
		if ((size_t)p < st.mission_objectives_string.size()) {
			// CHK string refs are 1-based; map_strings is 0-based. The engine's
			// own reader (trigger_string) decrements, and so must this one --
			// without it the F10 screen renders the string *after* the correct
			// one, which reads as "the objective never updates".
			int si = st.mission_objectives_string[p];
			if (si > 0 && (size_t)(si - 1) < game_st.map_strings.size()) {
				a_string s;
				for (char c : game_st.map_strings[si - 1]) {
					if ((unsigned char)c >= 0x20) s += c;
					else if (c == '\n' || c == '\r') s += '\n';
				}
				while (!s.empty() && s.back() == '\n') s.pop_back();
				if (!s.empty()) return s;
			}
		}
		if (!briefing_objectives.empty()) return briefing_objectives;
		return mission_objectives;
	}

	// --- v0.30 in-game chat + cheats ------------------------------------
	// Tap-Hold on the MENU button opens a chat bar; on Android start_text_input
	// raises the soft keyboard. Enter submits: recognized cheats are applied
	// (single player / campaign only), everything else becomes a chat line.
	// LAN networked delivery is deferred to the LAN-lobby milestone; for now a
	// submitted line echoes locally. Cheats are FORBIDDEN in net games.
	bool chat_active = false;
	a_string chat_input;
	// D5 free save-name: a text prompt that shares the chat IME buffer (chat_input)
	// but confirms into disk_save(slot, name) instead of a chat line.
	bool save_name_active = false;
	int pending_save_slot = 0;
	void open_save_name_prompt(int slot) {
		if (!game_input || slot < 1) return;
		close_chat();
		save_name_active = true;
		pending_save_slot = slot;
		chat_input = save_slot_name(slot);   // prefill so an existing name can be edited
		if (wnd) wnd.start_text_input();
	}
	void cancel_save_name_prompt() {
		if (!save_name_active) return;
		save_name_active = false;
		pending_save_slot = 0;
		chat_input.clear();
		if (wnd) wnd.stop_text_input();
	}
	void confirm_save_name_prompt() {
		if (!save_name_active) return;
		int slot = pending_save_slot;
		a_string name = chat_trim(chat_input);
		save_name_active = false;
		pending_save_slot = 0;
		chat_input.clear();
		if (wnd) wnd.stop_text_input();
		if (slot >= 1) disk_save(slot, name);
	}
	// How much of the game surface the soft keyboard covers at the bottom, in
	// 1/10000 of its height (0 when hidden). Set by the Android glue from the
	// window insets. v0.125: it used to be DEVICE pixels compared against buffer
	// rows (and world rows in draw_chat), which is only right when the buffer is
	// as tall as the screen -- at the half-resolution tier the bar was lifted
	// twice as far as needed.
	int ime_frac = 0;
	int ime_top_y() const {
		if (ime_frac <= 0) return (int)screen_height;
		int y = (int)screen_height - (int)((long long)ime_frac * (int)screen_height / 10000);
		return y < 0 ? 0 : y;
	}
	a_string local_player_name;              // set by the glue; "You" if empty
	// v0.83: names for every slot, so an incoming LAN chat line can be
	// attributed. The glue fills these from the lobby's client list; an empty
	// entry falls back to "Player N".
	std::array<a_string, 12> slot_player_names;

	a_string chat_display_name(int owner) const {
		if (owner == game_local_player)
			return local_player_name.empty() ? a_string("You") : local_player_name;
		if (owner >= 0 && owner < 12 && !slot_player_names[(size_t)owner].empty())
			return slot_player_names[(size_t)owner];
		char b[16];
		std::snprintf(b, sizeof(b), "Player %d", owner + 1);
		return a_string(b);
	}
	struct chat_line_t { a_string text; uint32_t color; int frame; };
	a_vector<chat_line_t> chat_lines;

	void add_chat_line(const a_string& text, uint32_t color) {
		chat_lines.push_back({text, color, (int)st.current_frame});
		while (chat_lines.size() > 5) chat_lines.erase(chat_lines.begin());
	}
	void open_chat() {
		if (!game_input || chat_active || save_name_active) return;   // D5: don't hijack the save-name IME
		if (pause_screen != 0) return;   // v0.125: a hold on MENU under the open pause menu used to open the chat behind it
		chat_active = true;
		chat_input.clear();
		if (wnd) wnd.start_text_input();
	}
	void close_chat() {
		if (!chat_active) return;
		chat_active = false;
		chat_input.clear();
		if (wnd) wnd.stop_text_input();
	}
	static a_string chat_trim(const a_string& in) {
		size_t a = 0, b = in.size();
		while (a < b && (unsigned char)in[a] <= ' ') ++a;
		while (b > a && (unsigned char)in[b - 1] <= ' ') --b;
		return in.substr(a, b - a);
	}
	// Cheat requests the android_main loop consumes (win/lose need main_t).
	bool cheat_force_win = false;
	bool cheat_force_lose = false;

	// Sends an engine cheat action (opcode 18): bit 0x10 = +10000 min/gas,
	// bit 2 = operation cwal (fast build). SP executes it immediately.
	void submit_cheat_flags(int flags) {
		st.cheats_enabled = true;
		game_action_writer w;
		w.u8(18);
		w.u32((uint32_t)flags);
		submit_game_action(w);
	}

	// Applies a StarCraft: Brood War cheat if the text matches one (single
	// player / campaign only — LAN forbidden). The classic 16 codes; effects
	// that map to engine state are applied directly (SP is a single instance,
	// so direct st edits can't desync). Returns true if a cheat was recognized.
	bool try_cheat(const a_string& text) {
		if (is_net_game) return false;             // LAN cheats forbidden
		int p = game_local_player;
		if (p < 0 || p >= 12) return false;
		a_string s;
		for (char c : text) s += (char)std::tolower((unsigned char)c);

		if (s == "show me the money") {
			submit_cheat_flags(0x10 | (st.cheat_operation_cwal ? 2 : 0)); // keep cwal
			show_hud_message("Cheat: Minerals +10000, Vespene +10000.");
		} else if (s == "whats mine is mine") {
			st.current_minerals[p] += 10000;
			show_hud_message("Cheat: Minerals +10000.");
		} else if (s == "breathe deep") {
			st.current_gas[p] += 10000;
			show_hud_message("Cheat: Vespene gas +10000.");
		} else if (s == "operation cwal") {
			submit_cheat_flags(st.cheat_operation_cwal ? 0 : 2);          // toggle
			show_hud_message(st.cheat_operation_cwal ? "Cheat: Operation CWAL ON (fast build)."
			                                         : "Cheat: Operation CWAL OFF.");
		} else if (s == "something for nothing" || s == "medieval man") {
			for (int i = 0; i < 61; ++i)
				st.upgrade_levels[p][(UpgradeTypes)i] = player_max_upgrade_level(p, (UpgradeTypes)i);
			for (int i = 0; i < 44; ++i)
				st.tech_researched[p][(TechTypes)i] = true;
			show_hud_message("Cheat: All upgrades and technologies granted.");
		} else if (s == "power overwhelming") {
			for (unit_t* u : ptr(st.player_units[p]))
				u_set_status_flag(u, unit_t::status_flag_invincible, true);
			show_hud_message("Cheat: Power overwhelming (invincible).");
		} else if (s == "the gathering") {
			for (unit_t* u : ptr(st.player_units[p]))
				set_unit_energy(u, unit_max_energy(u));
			show_hud_message("Cheat: The Gathering (energy refilled).");
		} else if (s == "there is no cow level") {
			cheat_force_win = true;
			show_hud_message("Cheat: Mission complete.");
		} else if (s == "game over man") {
			cheat_force_lose = true;
			show_hud_message("Cheat: Game over, man.");
		} else {
			return false;
		}
		return true;
	}
	// Chat now goes through the action stream (opcode 92) instead of being
	// appended locally, so the other peers in a LAN game actually receive it.
	// The line appears on the sender's own screen through the same path: in
	// single player submit_game_action() executes the action immediately, and
	// in a network game it comes back with everyone else's copy.
	void submit_chat() {
		a_string text = chat_trim(chat_input);
		// Cheats stay strictly local -- they are already gated to single
		// player, and broadcasting one would desync every peer.
		if (!text.empty() && !try_cheat(text)) {
			int p = game_local_player >= 0 ? game_local_player : 0;
			game_action_writer w;
			w.u8(92);
			w.u8((uint8_t)p);                       // retail's player byte
			size_t n = text.size(); if (n > 79) n = 79;
			for (size_t i = 0; i != n; ++i) w.u8((uint8_t)text[i]);
			for (size_t i = n; i != 80; ++i) w.u8(0);
			submit_game_action(w);
		}
		close_chat();
	}

	virtual void on_chat_message(int owner, const a_string& text) override {
		add_chat_line(chat_display_name(owner) + ": " + text,
			player_colors_rgba(owner >= 0 && owner < 12 ? owner : 0));
	}

	// In-session quick save/load: a deep copy of the game state (state_copier).
	// Single player only (LAN would desync). Disk save is a later step.
	std::unique_ptr<state> qsave_st;
	std::unique_ptr<action_state> qsave_ast;
	bool has_qsave = false;

	void quick_save() {
		if (is_net_game) return;
		qsave_st = std::make_unique<state>(copy_state(st));
		qsave_ast = std::make_unique<action_state>(copy_state(action_st, st, *qsave_st));
		has_qsave = true;
	}
	void quick_load() {
		if (is_net_game || !has_qsave) return;
		st = copy_state(*qsave_st);
		action_st = copy_state(*qsave_ast, *qsave_st, st);
	}

	// --- Disk save/load (v0.86) -----------------------------------------
	//
	// A .sav file is a small self-describing container header in front of the
	// savegame::save() blob. The header lets the launcher (Java) and an
	// in-session load read a slot's map / frame / relaunch args WITHOUT parsing
	// the blob. All integers little-endian; strings are u32 length + bytes.
	//
	//   u32 magic ("SAVS")   u32 container_version
	//   u32 frame            u32 map_id            u32 engine_version
	//   u32 name_len  name    u32 args_len  args
	//   <savegame blob>      (its own magic 0x57424f21)
	//
	// map_id + engine_version gate compatibility: an in-session load refuses a
	// save from a different map or a different engine build, and savegame::load
	// re-checks them against the blob header. Set by the port after map load.
	static const uint32_t sav_magic = 0x53564153;   // "SAVS"
	static const uint32_t sav_container_version = 1;
	static const int sav_slot_count = 3;

	a_string save_dir;              // <data>/saves (port sets it; empty = disabled)
	// open1161HD (save/load submilestone 1): campaign and single-player saves are kept
	// in SEPARATE slot namespaces so they never mix -- a campaign game only sees and
	// overwrites campaign slots ("cslotN"), a single-player game only its own ("slotN").
	// Set by the port from the launch mode.
	bool is_campaign_game = false;
	const char* save_slot_prefix() const { return is_campaign_game ? "cslot" : "slot"; }
	uint32_t save_map_id = 0;       // crc32 of the loaded map's source bytes
	uint32_t save_engine_version = 0; // app versionCode -- rejects cross-build saves
	a_string save_map_name;         // display name for the load list
	a_string save_args;             // '\n'-joined launch argv (minus --load) to relaunch
	a_string save_feedback;         // transient "Saved to slot N" line for the menu

	a_string save_slot_path(int slot) const {
		char b[32];
		snprintf(b, sizeof(b), "/%s%d.sav", save_slot_prefix(), slot);
		return save_dir + b;
	}

	// D5 free save-name: a plain-text SIDECAR next to the .sav, so a user-given
	// name needs NO change to the .sav container (no format bump, no gate risk).
	a_string save_slot_name_path(int slot) const {
		char b[32];
		snprintf(b, sizeof(b), "/%s%d.name", save_slot_prefix(), slot);
		return save_dir + b;
	}
	a_string save_slot_name(int slot) const {
		if (save_dir.empty()) return "";
		FILE* f = fopen(save_slot_name_path(slot).c_str(), "rb");
		if (!f) return "";
		char buf[128];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;
		return a_string(buf, buf + n);
	}

	// Read just the container header of a slot. Returns true and fills frame/
	// map_id/engine on success; leaves them untouched on a missing/short/bad file.
	bool save_slot_header(int slot, uint32_t& frame, uint32_t& map_id, uint32_t& engine) const {
		if (save_dir.empty()) return false;
		FILE* f = fopen(save_slot_path(slot).c_str(), "rb");
		if (!f) return false;
		uint32_t h[6];
		bool ok = fread(h, 4, 6, f) == 6;
		fclose(f);
		if (!ok || h[0] != sav_magic || h[1] != sav_container_version) return false;
		frame = h[2]; map_id = h[3]; engine = h[4];
		return true;
	}

	// Submilestone 3: the container's map/mission display name (the string right
	// after the 24-byte header). Used as the in-game slot label when the player
	// didn't name the save, so the pause-menu list reads like the launcher's
	// ("Norad II" / a melee map) instead of a bare frame number.
	a_string save_slot_map_name(int slot) const {
		if (save_dir.empty()) return "";
		FILE* f = fopen(save_slot_path(slot).c_str(), "rb");
		if (!f) return "";
		uint32_t h[6];
		a_string out;
		if (fread(h, 4, 6, f) == 6 && h[0] == sav_magic && h[1] == sav_container_version) {
			uint32_t nlen = h[5];
			if (nlen > 0 && nlen < 4096) { out.resize(nlen); if (fread(&out[0], 1, nlen, f) != nlen) out.clear(); }
		}
		fclose(f);
		return out;
	}

	// One-line slot description for the menu: "Frame 1234" for a save that
	// belongs to THIS map+engine, "(other map)" / "(other version)" for a
	// mismatch, "" for an empty/unreadable slot.
	a_string save_slot_info(int slot) const {
		uint32_t frame = 0, map_id = 0, engine = 0;
		if (!save_slot_header(slot, frame, map_id, engine)) return "";
		if (engine != save_engine_version) return "(other version)";
		if (map_id != save_map_id) return "(other map)";
		a_string name = save_slot_name(slot);   // D5: user name wins over the synthetic label
		if (!name.empty()) return name;
		char b[48];
		a_string mn = save_slot_map_name(slot); // submilestone 3: map/mission name
		if (!mn.empty()) { snprintf(b, sizeof(b), "  \xC2\xB7  Frame %u", (unsigned)frame); return mn + b; }
		snprintf(b, sizeof(b), "Frame %u", (unsigned)frame);
		return a_string(b);
	}
	bool save_slot_loadable(int slot) const {
		uint32_t frame = 0, map_id = 0, engine = 0;
		if (!save_slot_header(slot, frame, map_id, engine)) return false;
		return engine == save_engine_version && map_id == save_map_id;
	}
	bool any_save_slot_loadable() const {
		for (int s = 1; s <= sav_slot_count; ++s) if (save_slot_loadable(s)) return true;
		return false;
	}

	bool disk_save(int slot, const a_string& name = a_string()) {
		if (is_net_game || save_dir.empty()) { save_feedback = "Save unavailable"; return false; }
		a_vector<uint8_t> out;
		auto pu32 = [&](uint32_t v) {
			uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
			out.insert(out.end(), b, b + 4);
		};
		auto pstr = [&](const a_string& s) {
			pu32((uint32_t)s.size());
			out.insert(out.end(), (const uint8_t*)s.data(), (const uint8_t*)s.data() + s.size());
		};
		pu32(sav_magic);
		pu32(sav_container_version);
		pu32((uint32_t)st.current_frame);
		pu32(save_map_id);
		pu32(save_engine_version);
		pstr(save_map_name);
		pstr(save_args);
		a_vector<uint8_t> blob = savegame::save(st, action_st, save_engine_version, save_map_id);
		out.insert(out.end(), blob.begin(), blob.end());
		FILE* f = fopen(save_slot_path(slot).c_str(), "wb");
		if (!f) { save_feedback = "Save FAILED (no file)"; return false; }
		bool ok = out.empty() || fwrite(out.data(), 1, out.size(), f) == out.size();
		fclose(f);
		// D5 sidecar: write the user name (or clear a stale one). Sidecar-only -> the
		// .sav bytes above are untouched, so the save-container gate is unaffected.
		{
			a_string np = save_slot_name_path(slot);
			if (!name.empty()) {
				FILE* nf = fopen(np.c_str(), "wb");
				if (nf) { fwrite(name.data(), 1, name.size(), nf); fclose(nf); }
			} else {
				remove(np.c_str());
			}
		}
		char b[32];
		snprintf(b, sizeof(b), ok ? "Saved to slot %d" : "Save FAILED (slot %d)", slot);
		save_feedback = b;
		return ok;
	}

	// In-session load: the map is already loaded (same game_st), so this only
	// re-parses the container, verifies map+engine, and applies the blob onto
	// the live st/action_st. A cross-session (fresh launch) load reloads the
	// map first, then calls savegame::load directly -- see the port's --load.
	bool disk_load_current(int slot) {
		if (is_net_game || save_dir.empty()) return false;
		FILE* f = fopen(save_slot_path(slot).c_str(), "rb");
		if (!f) return false;
		a_vector<uint8_t> buf;
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0) { buf.resize((size_t)sz); if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) buf.clear(); }
		fclose(f);
		size_t off = 0;
		auto rd_u32 = [&](uint32_t& v) -> bool {
			if (off + 4 > buf.size()) return false;
			v = (uint32_t)buf[off] | ((uint32_t)buf[off+1] << 8) | ((uint32_t)buf[off+2] << 16) | ((uint32_t)buf[off+3] << 24);
			off += 4; return true;
		};
		auto skip_str = [&]() -> bool { uint32_t n; if (!rd_u32(n)) return false; if (off + n > buf.size()) return false; off += n; return true; };
		uint32_t magic = 0, ver = 0, frame = 0, map_id = 0, engine = 0;
		if (!rd_u32(magic) || magic != sav_magic) return false;
		if (!rd_u32(ver) || ver != sav_container_version) return false;
		if (!rd_u32(frame) || !rd_u32(map_id) || !rd_u32(engine)) return false;
		if (!skip_str() || !skip_str()) return false;          // map_name, args
		if (engine != save_engine_version || map_id != save_map_id) return false;
		auto lr = savegame::load(st, action_st, buf.data() + off, buf.size() - off, save_engine_version, save_map_id);
		return lr == savegame::load_result::ok;
	}

	// ---- Pause menu and dialogs (v0.125) --------
	// One centred panel with the race tab, over the game dimmed by 55% -- the field
	// and the HUD stay visible. (It used to be an opaque black screen: the old
	// backdrop was fill_rect_rgba(0xd8000000), and fill_rect_rgba does not blend.)
	// Rows stay 50 dp high as before. Every rectangle comes from the same function
	// for drawing (hx < 0) and for hit-testing (hx >= 0), so the two cannot disagree.
	// Sizes are design px at the 879-high tier and go through hud_h_px(), like the
	// rest of HUD v2; text metrics follow hud_scale_text (set in pause_menu_render).
	struct menu_lay_t {
		rect panel{};
		int x0 = 0, x1 = 0;   // content column
		int y = 0;            // top of the first content row
		int bh = 0, gap = 0;  // row height, gap between rows
		int slot_h = 0;       // save slot row (two text lines)
	};
	enum { menu_btn_normal = 0, menu_btn_primary = 1, menu_btn_danger = 2 };
	int menu_text_adv(int scale) {
		float f = hud_text_pct(scale * 50) / 100.0f;
		int a = (int)((ui::unispace_w + 1) * f + 0.5f);
		return a < 1 ? 1 : a;
	}
	int menu_text_w(const char* s, int scale) { return (int)strlen(s) * menu_text_adv(scale); }
	int menu_text_h(int scale) {
		float f = hud_text_pct(scale * 50) / 100.0f;
		int gs = (int)(f + 0.5f); if (gs < 1) gs = 1;
		return ui::unispace_h * gs;
	}
	static bool menu_in(const rect& r, int x, int y) { return x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y; }
	int menu_content_w() {
		int bw = hud_h_px(600);
		int lim = (int)screen_width - hud_h_px(2 * 18 + 16);
		return bw > lim ? lim : bw;
	}
	int menu_half_w() { return (menu_content_w() - hud_h_px(20)) / 2; }
	int menu_head_h() { return hud_h_px(22) + menu_text_h(4) + hud_h_px(18); }
	// A dialog may use what is above the HUD bar (or the replay bar) and above the
	// soft keyboard.
	int menu_area_bottom() {
		int b = playfield_bottom_y();
		int k = ime_top_y();
		if (k < b) b = k;
		if (b < hud_h_px(120)) b = hud_h_px(120);
		return b;
	}
	// content_h(lay) -> height of everything under the title for the row sizes in
	// lay. The rows are 50 dp with 12 dp gaps; only when the dialog would not fit
	// (eleven alliance rows, the save list above the keyboard) they step down.
	template<typename F>
	menu_lay_t menu_layout(F&& content_h) {
		static const int steps[4][3] = {{50, 12, 64}, {44, 8, 56}, {38, 6, 50}, {34, 4, 46}};
		menu_lay_t l;
		const int pad = hud_h_px(18), head = menu_head_h();
		const int bw = menu_content_w();
		const int area = menu_area_bottom();
		int h = 0;
		for (int i = 0; i != 4; ++i) {
			l.bh = hud_h_px(steps[i][0]); l.gap = hud_h_px(steps[i][1]); l.slot_h = hud_h_px(steps[i][2]);
			h = head + content_h(l) + pad;
			if (h <= area - hud_h_px(16)) break;
		}
		int top = area / 2 - h / 2;
		if (top < hud_h_px(4)) top = hud_h_px(4);
		const int left = (int)screen_width / 2 - bw / 2 - pad;
		l.panel = {{left, top}, {left + bw + 2 * pad, top + h}};
		l.x0 = left + pad; l.x1 = l.x0 + bw;
		l.y = top + head;
		return l;
	}
	// Darkens a rectangle: keep = 0..256 of the original brightness. Shift-only, so
	// a full-screen dim every frame stays cheap at the FULL HD tier (a LAN game
	// keeps running under the menu).
	void dim_rect_rgba(uint32_t* data, size_t pitch, rect r, int keep) {
		if (r.from.x < 0) r.from.x = 0;
		if (r.from.y < 0) r.from.y = 0;
		if (r.to.x > (int)screen_width) r.to.x = (int)screen_width;
		if (r.to.y > (int)screen_height) r.to.y = (int)screen_height;
		const uint32_t k = (uint32_t)(keep < 0 ? 0 : keep > 256 ? 256 : keep);
		for (int y = r.from.y; y < r.to.y; ++y) {
			uint32_t* row = data + (size_t)y * pitch;
			for (int x = r.from.x; x < r.to.x; ++x) {
				const uint32_t d = row[x];
				row[x] = 0xff000000u | ((((d & 0xff00ffu) * k) >> 8) & 0xff00ffu) | ((((d & 0x00ff00u) * k) >> 8) & 0x00ff00u);
			}
		}
	}
	void menu_draw_panel(uint32_t* data, size_t pitch, const menu_lay_t& l, const char* title) {
		dim_rect_rgba(data, pitch, {{0, 0}, {(int)screen_width, (int)screen_height}}, 115);   // 45% left = dimmed by 55%
		hud2_frame(data, pitch, l.panel);
		draw_hud_tab(data, pitch, l.panel, 0);
		int scale = 4;
		while (scale > 2 && menu_text_w(title, scale) > l.x1 - l.x0) --scale;
		draw_text_rgba(data, pitch, (l.x0 + l.x1) / 2 - menu_text_w(title, scale) / 2, l.panel.from.y + hud_h_px(22), title, hud_theme().menu_title, scale);
	}
	// Button colours: normal; primary = the race's "on" colours (what the player
	// most likely wants); danger = red line and text (leaves the game); disabled.
	void menu_button_box(uint32_t* data, size_t pitch, rect r, int kind, bool enabled, uint32_t* text_color = nullptr) {
		const hud_theme_t th = hud_theme();
		uint32_t fill = th.menu_btn_fill | 0xff000000u, line = th.menu_btn_line, text = 0xffe0e8f0u;
		if (!enabled) { fill = th.panel_fill | 0xff000000u; line = th.off_line; text = 0xff6a7078u; }
		else if (kind == menu_btn_primary) { fill = th.on_fill | 0xff000000u; line = th.on_line; text = th.on_text; }
		else if (kind == menu_btn_danger) { fill = 0xff2a1113u; line = 0xffe0564eu; text = 0xffff7068u; }
		else if (hud_pressed(r)) fill = th.btn_pressed;
		fill_rect_rgba(data, pitch, r, fill);
		line_rectangle_rgba(data, pitch, r, line);
		if (text_color) *text_color = text;
	}
	void menu_button(uint32_t* data, size_t pitch, rect r, const char* label, int kind, bool enabled, int scale = 3) {
		uint32_t text = 0;
		menu_button_box(data, pitch, r, kind, enabled, &text);
		if (!label || !*label) return;
		while (scale > 2 && menu_text_w(label, scale) > (r.to.x - r.from.x) - hud_h_px(6)) --scale;
		if (menu_text_h(scale) > r.to.y - r.from.y) scale = 2;
		draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - menu_text_w(label, scale) / 2, (r.from.y + r.to.y) / 2 - menu_text_h(scale) / 2, label, text, scale);
	}

	// Modal list: title and n full-width rows. With a real (hx,hy), returns the
	// enabled row hit (no draw); else draws, returns -1. kinds: per-row button kind
	// (null = all normal). back_last: the last row is a half-width button under the
	// list (the "Back" of a list whose rows are things, not commands).
	int menu_modal(uint32_t* data, size_t pitch, const char* title,
	               const char* const* labels, const bool* enabled, int n, int hx, int hy,
	               const int* kinds = nullptr, bool back_last = false) {
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) { return n * m.bh + (n - 1) * m.gap; });
		if (hx < 0) menu_draw_panel(data, pitch, l, title);
		for (int i = 0; i < n; ++i) {
			int by = l.y + i * (l.bh + l.gap);
			rect r = {{l.x0, by}, {l.x1, by + l.bh}};
			if (back_last && i == n - 1) r.to.x = l.x0 + menu_half_w();
			if (hx >= 0) {
				if (enabled[i] && menu_in(r, hx, hy)) return i;
				continue;
			}
			menu_button(data, pitch, r, labels[i], kinds ? kinds[i] : (int)menu_btn_normal, enabled[i]);
		}
		return -1;
	}

	// Splits body on newlines and wraps over-long lines on spaces (hard cut for one
	// unbroken word). v0.83.002: campaign objectives are whole sentences and used
	// to run off the edge.
	a_vector<a_string> menu_wrap(const char* body, size_t max_chars) {
		a_vector<a_string> lines;
		a_string src;
		for (const char* p = body;; ++p) {
			if (*p == '\n' || *p == 0) {
				if (src.size() <= max_chars) lines.push_back(src);
				else {
					size_t at = 0;
					while (src.size() - at > max_chars) {
						size_t cut = at + max_chars;
						size_t space = cut;
						while (space > at && src[space] != ' ') --space;
						if (space == at) space = cut;   // one unbroken word: hard cut
						lines.push_back(src.substr(at, space - at));
						at = space < src.size() && src[space] == ' ' ? space + 1 : space;
					}
					lines.push_back(src.substr(at));
				}
				src.clear();
				if (*p == 0) break;
			} else src += *p;
		}
		return lines;
	}

	// Modal text page (title + newline-separated body + a Back button). Returns
	// 0 if Back is hit (hit-test mode), else draws and returns -1.
	// Objectives are authored as a hand-formatted block (maps conventionally start
	// each line with '-'), so with left_align the lines share one left margin and
	// the block is centred by its widest line.
	int menu_text(uint32_t* data, size_t pitch, const char* title, const char* body, int hx, int hy,
	              bool left_align = false) {
		const int char_w = menu_text_adv(2);
		const int lh = menu_text_h(2) + hud_h_px(6);
		const int bw = menu_content_w();
		size_t max_chars = bw > char_w * 8 ? (size_t)(bw / char_w) : (size_t)8;
		a_vector<a_string> lines = menu_wrap(body, max_chars);
		// never taller than the area: drop what does not fit (smallest row sizes)
		{
			int room = menu_area_bottom() - hud_h_px(16) - menu_head_h() - hud_h_px(18) - hud_h_px(34 + 4);
			size_t max_lines = room > lh ? (size_t)(room / lh) : (size_t)1;
			if (lines.size() > max_lines) lines.resize(max_lines);
		}
		const int n = (int)lines.size();
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) { return n * lh + m.gap + m.bh; });
		const int cx = (l.x0 + l.x1) / 2;
		const int by = l.y + n * lh + l.gap;
		rect back = {{cx - menu_half_w() / 2, by}, {cx - menu_half_w() / 2 + menu_half_w(), by + l.bh}};
		if (hx >= 0) return menu_in(back, hx, hy) ? 0 : -1;
		menu_draw_panel(data, pitch, l, title);
		int left_x = l.x0;
		if (left_align) {
			size_t maxw = 0;
			for (auto& s : lines) if (s.size() > maxw) maxw = s.size();
			left_x = cx - (int)maxw * char_w / 2;
			if (left_x < l.x0) left_x = l.x0;
		}
		int ty = l.y;
		for (auto& s : lines) {
			draw_text_rgba(data, pitch, left_align ? left_x : cx - (int)s.size() * char_w / 2, ty, s.c_str(), 0xffd0d8e0u, 2);
			ty += lh;
		}
		menu_button(data, pitch, back, L("Back"), menu_btn_normal, true);
		return -1;
	}

	const char* controls_text() {
		// The chain-orders toggle is SHIFT in HUD v2 and QUEUE in the legacy HUD.
		return ui_legacy
			? "Tap: select unit.   Drag: box-select.\n"
			  "Long-press ground: move / attack / rally.\n"
			  "Two fingers: pan the camera.   Pinch: zoom.\n"
			  "Tap minimap: jump.   Drag minimap: slide view.\n"
			  "QUEUE button or S-Pen side button: chain orders.\n"
			  "-/+ buttons: zoom in and out.\n"
			  "Back button or MENU: open this pause menu."
			: "Tap: select unit.   Drag: box-select.\n"
			  "Long-press ground: move / attack / rally.\n"
			  "Two fingers: pan the camera.   Pinch: zoom.\n"
			  "Tap minimap: jump.   Drag minimap: slide view.\n"
			  "SHIFT button or S-Pen side button: chain orders.\n"
			  "Hold a group number: add to it, replace it or clear it.\n"
			  "Back button or MENU: open this pause menu.";
	}
	int tip_index = 0;
	bool show_tips_on_startup = true;
	const char* tip_at(int i) {
		static const char* tips[] = {
			"Hold and drag on the minimap to sweep your\nview across the map quickly.",
			"Turn on SHIFT (or hold the S-Pen side button) to\nline up several move or attack orders.",
			"Pinch with two fingers to zoom the battle in\nand out.",
			"Long-press the ground with a building selected\nto set its rally point.",
			"Double-tap a unit to select every unit of that\ntype currently on screen.",
		};
		static const char* tips_legacy[] = {
			"Hold and drag on the minimap to sweep your\nview across the map quickly.",
			"Turn on QUEUE (or hold the S-Pen side button) to\nline up several move or attack orders.",
			"Pinch with two fingers to zoom the battle in\nand out, or use the - / + buttons.",
			"Long-press the ground with a building selected\nto set its rally point.",
			"Double-tap a unit to select every unit of that\ntype currently on screen.",
		};
		int n = (int)(sizeof(tips) / sizeof(tips[0]));
		return (ui_legacy ? tips_legacy : tips)[((i % n) + n) % n];
	}
	int tip_count() { return 5; }

	// Tips page: current tip, a "show on startup" checkbox, and OK / Previous /
	// Next buttons. Returns 0=OK, 1=Prev, 2=Next, 3=checkbox (hit-test), else -1.
	// The whole checkbox row is the touch target (it used to be 23 dp high).
	int menu_tips(uint32_t* data, size_t pitch, int hx, int hy) {
		const int char_w = menu_text_adv(2);
		const int lh = menu_text_h(2) + hud_h_px(6);
		a_vector<a_string> lines = menu_wrap(tip_at(tip_index), (size_t)(menu_content_w() / char_w));
		// every tip takes the room of the tallest one, so the buttons do not move
		// under the finger when paging through the tips
		const int body_lines = 3;
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) { return body_lines * lh + m.gap + m.bh + m.gap + m.bh; });
		// OK is short, the two paging buttons share the rest: three equal 190 dp
		// buttons left "Previous Tip" touching its frame (seen on the S8 Ultra).
		const int bg = hud_h_px(12);
		const int bok = hud_h_px(150);
		const int b3 = (l.x1 - l.x0 - 2 * bg - bok) / 2;
		const int cy = l.y + body_lines * lh + l.gap;
		const int by = cy + l.bh + l.gap;
		rect cbrow = {{l.x0, cy}, {l.x1, cy + l.bh}};
		rect ok = {{l.x0, by}, {l.x0 + bok, by + l.bh}};
		rect prev = {{l.x0 + bok + bg, by}, {l.x0 + bok + bg + b3, by + l.bh}};
		rect next = {{l.x1 - b3, by}, {l.x1, by + l.bh}};
		if (hx >= 0) {
			if (menu_in(ok, hx, hy)) return 0;
			if (menu_in(prev, hx, hy)) return 1;
			if (menu_in(next, hx, hy)) return 2;
			if (menu_in(cbrow, hx, hy)) return 3;
			return -1;
		}
		char title[32];
		snprintf(title, sizeof(title), "TIP %d / %d", tip_index + 1, tip_count());
		menu_draw_panel(data, pitch, l, title);
		const int cx = (l.x0 + l.x1) / 2;
		int ty = l.y;
		for (auto& s : lines) {
			draw_text_rgba(data, pitch, cx - (int)s.size() * char_w / 2, ty, s.c_str(), 0xffd0d8e0u, 2);
			ty += lh;
		}
		// checkbox
		const int cbsz = hud_h_px(24);
		rect cb = {{l.x0, cy + (l.bh - cbsz) / 2}, {l.x0 + cbsz, cy + (l.bh - cbsz) / 2 + cbsz}};
		fill_rect_rgba(data, pitch, cb, hud_theme().portrait_fill);
		line_rectangle_rgba(data, pitch, cb, show_tips_on_startup ? hud_theme().on_line : hud_theme().menu_btn_line);
		if (show_tips_on_startup) {
			const int in = hud_h_px(5);
			fill_rect_rgba(data, pitch, {{cb.from.x + in, cb.from.y + in}, {cb.to.x - in, cb.to.y - in}}, hud_theme().on_line);
		}
		draw_text_rgba(data, pitch, cb.to.x + hud_h_px(12), cy + l.bh / 2 - menu_text_h(2) / 2, "Show tips on startup", 0xffc0c8d0u, 2);
		menu_button(data, pitch, ok, "OK", menu_btn_primary, true);
		menu_button(data, pitch, prev, "Previous Tip", menu_btn_normal, true);
		menu_button(data, pitch, next, "Next Tip", menu_btn_normal, true);
		return -1;
	}

	// Two buttons side by side under a question: returns 0 (left) / 1 (right).
	int menu_confirm(uint32_t* data, size_t pitch, const char* title, const char* left, int left_kind,
	                 const char* right, int right_kind, int hx, int hy) {
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) { return m.bh; });
		const int hw = menu_half_w();
		rect a = {{l.x0, l.y}, {l.x0 + hw, l.y + l.bh}};
		rect b = {{l.x1 - hw, l.y}, {l.x1, l.y + l.bh}};
		if (hx >= 0) return menu_in(a, hx, hy) ? 0 : menu_in(b, hx, hy) ? 1 : -1;
		menu_draw_panel(data, pitch, l, title);
		menu_button(data, pitch, a, left, left_kind, true);
		menu_button(data, pitch, b, right, right_kind, true);
		return -1;
	}

	// v0.83 Alliances. One row per other active player, cycling Enemy -> Allied ->
	// Allied + Vision (the three states are the only combinations worth having:
	// shared vision without an alliance is a curiosity, not a setting). The name is
	// on the left, the state on the right in its own colour. Returns the row hit,
	// alliance_rows.size() for Back.
	int menu_alliances(uint32_t* data, size_t pitch, int hx, int hy) {
		alliance_rows.clear();
		for (int p = 0; p != 12; ++p) {
			if (p == game_local_player) continue;
			if (!player_slot_active(p)) continue;
			bool ally = game_local_player >= 0 && st.alliances[game_local_player][p] != 0;
			bool vis = game_local_player >= 0
				&& (st.shared_vision[game_local_player] & (1u << p)) != 0;
			alliance_rows.push_back({p, chat_display_name(p), ally ? (vis ? 2 : 1) : 0});
		}
		const int n = (int)alliance_rows.size();
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) { return (n + 1) * m.bh + n * m.gap; });
		rect back = {{l.x0, l.y + n * (l.bh + l.gap)}, {l.x0 + menu_half_w(), l.y + n * (l.bh + l.gap) + l.bh}};
		if (hx < 0) menu_draw_panel(data, pitch, l, "ALLIANCES");
		for (int i = 0; i < n; ++i) {
			int by = l.y + i * (l.bh + l.gap);
			rect r = {{l.x0, by}, {l.x1, by + l.bh}};
			if (hx >= 0) {
				if (menu_in(r, hx, hy)) return i;
				continue;
			}
			static const char* const state_text[3] = {"Enemy", "Allied", "Allied + Vision"};
			static const uint32_t state_color[3] = {0xffe0564eu, 0xff4fd46au, 0xff4fd46au};
			const auto& row = alliance_rows[(size_t)i];
			uint32_t text = 0;
			menu_button_box(data, pitch, r, menu_btn_normal, true, &text);
			const char* stx = state_text[row.state];
			const int in = hud_h_px(12);
			int scale = 3;
			if (menu_text_h(scale) > l.bh
				|| menu_text_w(row.name.c_str(), scale) + menu_text_w(stx, scale) > (r.to.x - r.from.x) - 3 * in) scale = 2;
			const int ty = (r.from.y + r.to.y) / 2 - menu_text_h(scale) / 2;
			draw_text_rgba(data, pitch, r.from.x + in, ty, row.name.c_str(), text, scale);
			draw_text_rgba(data, pitch, r.to.x - in - menu_text_w(stx, scale), ty, stx, state_color[row.state], scale);
		}
		if (hx >= 0) return menu_in(back, hx, hy) ? n : -1;
		menu_button(data, pitch, back, L("Back"), menu_btn_normal, true);
		return -1;
	}

	// v0.86 disk save / load slot list. Rebuilt each render so the texts stay
	// current. A slot is two lines: "Slot N - <name>" and what is in it. On the
	// load screen a slot is disabled unless it holds a save for THIS map + engine
	// build. While a save is being named (D5), the chosen slot is highlighted and
	// the name field with Back / Save sits under the list -- in the panel, above
	// the keyboard, instead of a bar across the bottom of the screen.
	// Returns slot index 0..sav_slot_count-1, sav_slot_count = Back,
	// sav_slot_count + 1 = Save (only while naming).
	int menu_slots(uint32_t* data, size_t pitch, bool loading, int hx, int hy) {
		const bool naming = !loading && save_name_active;
		menu_lay_t l = menu_layout([&](const menu_lay_t& m) {
			return sav_slot_count * (m.slot_h + m.gap) + (naming ? m.bh + m.gap : 0) + m.bh;
		});
		const int hw = menu_half_w();
		int y = l.y + sav_slot_count * (l.slot_h + l.gap);
		rect field = {{l.x0, y}, {l.x1, y + l.bh}};
		if (naming) y += l.bh + l.gap;
		rect back = {{l.x0, y}, {l.x0 + hw, y + l.bh}};
		rect save = {{l.x1 - hw, y}, {l.x1, y + l.bh}};
		if (hx < 0) {
			const char* title = loading ? L("LOAD GAME")
				: (save_feedback.empty() ? L("SAVE GAME") : save_feedback.c_str());
			menu_draw_panel(data, pitch, l, title);
		}
		for (int s = 1; s <= sav_slot_count; ++s) {
			int by = l.y + (s - 1) * (l.slot_h + l.gap);
			rect r = {{l.x0, by}, {l.x1, by + l.slot_h}};
			const bool en = loading ? save_slot_loadable(s) : true;
			if (hx >= 0) {
				if (en && menu_in(r, hx, hy)) return s - 1;
				continue;
			}
			char l1[96], l2[96];
			uint32_t frame = 0, map_id = 0, engine = 0;
			if (!save_slot_header(s, frame, map_id, engine)) {
				snprintf(l1, sizeof(l1), "Slot %d", s);
				snprintf(l2, sizeof(l2), "%s", "Empty");
			} else {
				a_string name = save_slot_name(s), map = save_slot_map_name(s);
				const a_string& shown = name.empty() ? map : name;
				if (shown.empty()) snprintf(l1, sizeof(l1), "Slot %d", s);
				else snprintf(l1, sizeof(l1), "Slot %d - %s", s, shown.c_str());
				const unsigned secs = (unsigned)((unsigned long long)frame * 42 / 1000);   // "fastest" game speed
				if (engine != save_engine_version) snprintf(l2, sizeof(l2), "%s", "other version");
				else if (map_id != save_map_id) snprintf(l2, sizeof(l2), "%s", "other map");
				else if (!name.empty() && !map.empty()) snprintf(l2, sizeof(l2), "%s  %02u:%02u", map.c_str(), secs / 60, secs % 60);
				else snprintf(l2, sizeof(l2), "Frame %u  %02u:%02u", (unsigned)frame, secs / 60, secs % 60);
			}
			uint32_t text = 0;
			menu_button_box(data, pitch, r, naming && pending_save_slot == s ? (int)menu_btn_primary : (int)menu_btn_normal, en, &text);
			const int in = hud_h_px(12);
			int s1 = l.slot_h >= hud_h_px(60) ? 3 : 2;
			while (s1 > 2 && menu_text_w(l1, s1) > (r.to.x - r.from.x) - 2 * in) --s1;
			const int block = menu_text_h(s1) + hud_h_px(3) + menu_text_h(2);
			const int ty = (r.from.y + r.to.y) / 2 - block / 2;
			draw_text_rgba(data, pitch, r.from.x + in, ty, l1, text, s1);
			draw_text_rgba(data, pitch, r.from.x + in, ty + menu_text_h(s1) + hud_h_px(3), l2, en ? 0xff8a96a0u : 0xff565c62u, 2);
		}
		if (hx >= 0) {
			if (menu_in(back, hx, hy)) return sav_slot_count;
			if (naming && menu_in(save, hx, hy)) return sav_slot_count + 1;
			return -1;
		}
		if (naming) {
			fill_rect_rgba(data, pitch, field, hud_theme().portrait_fill);
			line_rectangle_rgba(data, pitch, field, hud_theme().on_line);
			const int in = hud_h_px(12);
			const int ty = (field.from.y + field.to.y) / 2 - menu_text_h(2) / 2;
			const char* lbl = "Save name:";
			draw_text_rgba(data, pitch, field.from.x + in, ty, lbl, hud_theme().amber, 2);
			a_string shown = chat_input + "_";
			draw_text_rgba(data, pitch, field.from.x + in + menu_text_w(lbl, 2) + menu_text_adv(2), ty, shown.c_str(), 0xffffffffu, 2);
			menu_button(data, pitch, save, "Save", menu_btn_primary, true);
		}
		menu_button(data, pitch, back, L("Back"), menu_btn_normal, true);
		return -1;
	}

	// Renders (hx<0) or hit-tests the current pause screen; returns hit index.
	int pause_menu_render(uint32_t* data, size_t pitch, int hx, int hy) {
		// Faze 4.7: scale the pause-menu glyphs to the FULL HD buffer for the length
		// of this call (the menu_* helpers size their layout with hud_h_px() to
		// match). No-op at SD/HD2. In hit-test mode no text is drawn, but the text
		// metrics the layout uses must be the same ones.
		bool _prev_hst = hud_scale_text; // restore, not force-false: may be called from draw_hud2
		hud_scale_text = true;
		struct g_t { bool* f; bool p; ~g_t() { *f = p; } } _g{&hud_scale_text, _prev_hst};
		switch (pause_screen) {
		case 1: {
			const char* labels[] = {L("Return to game"), L("Save game"), L("Load game"),
				L("Help"), "Mission Objectives", "Alliances",
				is_net_game ? "Leave game" : L("End Mission")};
			// Alliances only matter with someone else in the game.
			bool any_other = false;
			for (int p = 0; p != 12 && !any_other; ++p)
				if (p != game_local_player && player_slot_active(p)) any_other = true;
			bool en[] = {true, !is_net_game, !is_net_game && any_save_slot_loadable(), true, true, any_other, true};
			const int kinds[] = {menu_btn_primary, menu_btn_normal, menu_btn_normal, menu_btn_normal,
				menu_btn_normal, menu_btn_normal, menu_btn_danger};
			const int hit = menu_modal(data, pitch, L("PAUSED"), labels, en, 7, hx, hy, kinds);
			if (hx < 0 && hud_v3) {
				// HUD v3 keeps the field clear, so the version is here (design 4.16)
				menu_lay_t vl = menu_layout([&](const menu_lay_t& m) { return 7 * m.bh + 6 * m.gap; });
				if (!hud_version_img.empty()) draw_hud_version_img(data, pitch, (vl.panel.from.x + vl.panel.to.x) / 2, vl.panel.to.y + hud_h_px(6));
				else if (!hud_version.empty()) draw_text_rgba(data, pitch, (vl.panel.from.x + vl.panel.to.x) / 2 - menu_text_w(hud_version.c_str(), 2) / 2, vl.panel.to.y + hud_h_px(6), hud_version.c_str(), 0xff7f9486u);
			}
			return hit;
		}
		case 2: {
			const char* labels[] = {"Controls", "Tips", L("Back")};
			bool en[] = {true, true, true};
			return menu_modal(data, pitch, "HELP", labels, en, 3, hx, hy);
		}
		case 3: return menu_text(data, pitch, L("CONTROLS"), controls_text(), hx, hy);
		case 4: return menu_tips(data, pitch, hx, hy);
		case 5: return menu_text(data, pitch, L("MISSION OBJECTIVES"), current_objectives().c_str(), hx, hy, true);
		case 6:
			return menu_confirm(data, pitch, is_net_game ? L("Leave the game?") : L("End the mission?"),
				L("Yes, leave"), menu_btn_danger, L("No, keep playing"), menu_btn_primary, hx, hy);
		case 7: return menu_alliances(data, pitch, hx, hy);
		case 8: case 9: return menu_slots(data, pitch, pause_screen == 9, hx, hy);
		}
		return -1;
	}

	// Rows shown by pause screen 7; rebuilt on every render/hit-test so the
	// state always matches the game. state: 0 Enemy, 1 Allied, 2 Allied + Vision.
	struct alliance_row_t { int player; a_string name; int state; };
	a_vector<alliance_row_t> alliance_rows;

	// Cycles one player through Enemy -> Allied -> Allied + Vision and sends the
	// result. Both engine actions take the local player's FULL table, not a
	// delta, so the new table is computed here and handed over -- st is never
	// written directly, or a LAN game would diverge the moment one peer changed
	// an alliance.
	void submit_alliance_cycle(int p) {
		int me = game_local_player;
		if (me < 0 || p < 0 || p >= 12 || p == me) return;
		bool ally = st.alliances[me][p] != 0;
		bool vis = (st.shared_vision[me] & (1u << p)) != 0;
		bool new_ally, new_vis;
		if (!ally) { new_ally = true; new_vis = false; }
		else if (!vis) { new_ally = true; new_vis = true; }
		else { new_ally = false; new_vis = false; }

		std::array<int, 12> arr{};
		for (int i = 0; i != 12; ++i) arr[(size_t)i] = st.alliances[me][i] & 3;
		arr[(size_t)p] = new_ally ? 1 : 0;
		uint32_t vision = (uint32_t)st.shared_vision[me];
		if (new_vis) vision |= (1u << p); else vision &= ~(1u << p);

		{
			game_action_writer w;
			w.u8(14);
			uint32_t vals = 0;
			for (int i = 11; i >= 0; --i) { vals <<= 2; vals |= (uint32_t)arr[(size_t)i]; }
			w.u32(vals);
			submit_game_action(w);
		}
		{
			game_action_writer w;
			w.u8(13);
			w.u16((uint16_t)vision);
			submit_game_action(w);
		}
	}

	void draw_pause_menu(uint32_t* data, size_t pitch) {
		if (pause_screen != 0) pause_menu_render(data, pitch, -1, -1);   // the save-name field (D5) is part of the save panel
	}

	// Handles a tap while the pause menu is open. Returns true (always consumes
	// the tap while open).
	bool pause_menu_tap(int mx, int my) {
		if (pause_screen == 0) return false;
		// D5: the save-name prompt is only ever open on the save screen; anywhere
		// else a tap cancels it.
		if (save_name_active && pause_screen != 8) { cancel_save_name_prompt(); return true; }
		int hit = pause_menu_render(nullptr, 0, mx, my);
		int screen = pause_screen;
		if (screen == 1) { // main
			if (hit == 0) close_pause_menu();
			else if (hit == 1) { save_feedback.clear(); pause_screen = 8; } // Save -> slot list
			else if (hit == 2) pause_screen = 9;      // Load -> slot list
			else if (hit == 3) pause_screen = 2;      // Help
			else if (hit == 4) pause_screen = 5;      // Objectives
			else if (hit == 5) pause_screen = 7;      // Alliances
			else if (hit == 6) pause_screen = 6;      // End/Leave -> confirm
		} else if (screen == 8) { // save slots
			// Rows 0..sav_slot_count-1 are slots; the last row is Back. Saving
			// stays on this screen (title shows the result); Back returns to main.
			// While a save is being named: Save (or Enter) writes it, Back drops the
			// name and stays on the list, another slot moves the prompt there. A tap
			// anywhere else does nothing (it used to cancel the prompt).
			if (hit >= 0 && hit < sav_slot_count) {
				if (save_name_active) cancel_save_name_prompt();
				open_save_name_prompt(hit + 1); // D5: name it first
			} else if (hit == sav_slot_count) {
				if (save_name_active) cancel_save_name_prompt();
				else { save_feedback.clear(); pause_screen = 1; }
			} else if (hit == sav_slot_count + 1) confirm_save_name_prompt();
		} else if (screen == 9) { // load slots
			if (hit >= 0 && hit < sav_slot_count) { if (disk_load_current(hit + 1)) close_pause_menu(); }
			else if (hit == sav_slot_count) pause_screen = 1;
		} else if (screen == 2) { // help
			if (hit == 0) pause_screen = 3;
			else if (hit == 1) pause_screen = 4;
			else if (hit == 2) pause_screen = 1;
		} else if (screen == 3) { // controls
			if (hit == 0) pause_screen = 2;
		} else if (screen == 4) { // tips
			if (hit == 0) pause_screen = 2;                                    // OK -> Help
			else if (hit == 1) tip_index = (tip_index - 1 + tip_count()) % tip_count();
			else if (hit == 2) tip_index = (tip_index + 1) % tip_count();
			else if (hit == 3) show_tips_on_startup = !show_tips_on_startup;
		} else if (screen == 5) { // objectives
			if (hit == 0) pause_screen = 1;
		} else if (screen == 6) { // confirm leave
			if (hit == 0) {
				request_exit = true;
				// Single player leaves the process (back to launcher) right away;
				// a net game clears exit_on_close and is torn down by the host loop.
				// v1.0.3: the app's host loop handles the leave (leave_through_host):
				// it SAVES THE REPLAY first. Exiting here lost the recording of every
				// game the player left instead of finishing.
				if (exit_on_close && !leave_through_host) std::exit(0);
			} else if (hit == 1) pause_screen = 1;
		} else if (screen == 7) { // alliances
			// The last row is Back; the rest map to alliance_rows, which
			// pause_menu_render() just rebuilt for this hit test.
			if (hit >= 0 && hit < (int)alliance_rows.size())
				submit_alliance_cycle(alliance_rows[(size_t)hit].player);
			else if (hit == (int)alliance_rows.size()) pause_screen = 1;
		}
		return true;
	}

	void open_pause_menu() {
		pause_screen = 1;
		if (!is_net_game) is_paused = true;
	}
	void close_pause_menu() {
		pause_screen = 0;
		if (!is_net_game) is_paused = false;
		cancel_save_name_prompt();   // D5: don't leave a naming prompt open after the menu
	}

	// --- Action queue (v0.9) --------------------------------------------
	// With queue mode on (HUD toggle or the S-Pen side button held), orders
	// append to the unit's order queue instead of replacing it, so the player
	// can chain move/attack/build commands. The HUD toggle's visibility is a
	// Settings > Controls option (queue_button_visible), set from the launcher
	// via --show-queue (android_main). Players who queue only with the S-Pen
	// side button can hide the on-screen toggle.
	bool queue_mode = false;
	bool queue_button_visible = true;
	// HUD style: true = legacy tconsole.pcx (640x480 raster), false = native
	// responsive HUD v2 (anchored panels). Set from --ui-legacy (Settings).
	// Default legacy until HUD v2 is complete, then flips to false.
	bool ui_legacy = true;
	// Tap-to-command (modern touch-RTS control): with a selection, a plain tap on
	// empty ground / an enemy issues the contextual order instead of requiring a
	// 400 ms long-press. Set from --tap-command (default on). Long-press still works.
	bool tap_command = true;
	// task 2: SCBW2 auto-worker is now a manual in-game toggle instead of an
	// always-on assist. qol.auto_worker (the setting) only makes the button
	// APPEAR; this runtime flag, flipped by the button, actually drives
	// production. Off by default every game so it never runs unasked.
	bool auto_worker_active = false;
	a_string hud_version; // shown bottom-left in HUD v2 (set from android_main)
	// v0.125: the same label as a small ARGB image rendered by the Java side in the
	// launcher's own font and size (the engine only has the bitmap HUD font). HUD v2
	// draws it inside the bar's bottom-left corner, level with the panels' bottom
	// edge; without it (legacy HUD, host tools) the bitmap text is used as before.
	a_vector<uint32_t> hud_version_img;          // ARGB, device pixels
	int hud_version_img_w = 0, hud_version_img_h = 0;
	int hud_version_img_screen_h = 0;            // device height it was rendered for
	a_vector<uint32_t> hud_version_scaled;       // resampled to the render buffer
	int hud_version_scaled_w = 0, hud_version_scaled_h = 0, hud_version_scaled_for = -1;
	void draw_hud_version_img(uint32_t* data, size_t pitch, int at_x = -1, int at_y = -1) {
		const int sw = hud_version_img_w, sh = hud_version_img_h;
		if (sw <= 0 || sh <= 0 || (int)hud_version_img.size() != sw * sh) return;
		if (hud_version_scaled_for != (int)screen_height) {
			// area resample from device pixels to buffer pixels (premultiplied)
			double f = hud_version_img_screen_h > 0 ? (double)screen_height / hud_version_img_screen_h : 1.0;
			if (f > 1.0) f = 1.0;
			int dw = (int)(sw * f + 0.5), dh = (int)(sh * f + 0.5);
			if (dw < 1) dw = 1; if (dh < 1) dh = 1;
			hud_version_scaled.assign((size_t)dw * dh, 0);
			for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
				int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
				if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
				uint64_t a = 0, r = 0, g = 0, b = 0; int n = 0;
				for (int yy = y0; yy < y1 && yy < sh; ++yy) for (int xx = x0; xx < x1 && xx < sw; ++xx) {
					uint32_t c = hud_version_img[(size_t)yy * sw + xx];
					uint32_t ca = c >> 24;
					a += ca; r += ((c >> 16) & 0xff) * ca; g += ((c >> 8) & 0xff) * ca; b += (c & 0xff) * ca; ++n;
				}
				if (!n || !a) continue;
				hud_version_scaled[(size_t)y * dw + x] = ((uint32_t)(a / n) << 24) | ((uint32_t)(r / a) << 16) | ((uint32_t)(g / a) << 8) | (uint32_t)(b / a);
			}
			hud_version_scaled_w = dw; hud_version_scaled_h = dh; hud_version_scaled_for = (int)screen_height;
		}
		const int dw = hud_version_scaled_w, dh = hud_version_scaled_h;
		// (HUD v3 has no bar: the label is drawn under the PAUSED panel, at_x / at_y)
		const int x0 = at_x >= 0 ? at_x - dw / 2 : hud2_margin(), y0 = at_y >= 0 ? at_y : (int)screen_height - hud2_margin() - dh;
		for (int y = 0; y < dh; ++y) {
			int py = y0 + y; if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			for (int x = 0; x < dw; ++x) {
				int px = x0 + x; if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c = hud_version_scaled[(size_t)y * dw + x], a = c >> 24;
				if (!a) continue;
				uint32_t d = row[px], ia = 255 - a;
				row[px] = 0xff000000u | (((((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) / 255) << 16)
					| (((((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia) / 255) << 8) | (((c & 0xff) * a + (d & 0xff) * ia) / 255);
			}
		}
	}
	// Orders queue when the HUD QUEUE toggle is on, or while Shift is held.
	// The S-Pen barrel button is delivered as Left-Shift (see OpenBWActivity),
	// so the pen's side button also queues - matching desktop BW's Shift.
	bool order_queued() {
		if (queue_mode || hud3_force_queue || hud3_shift_held) return true;
		if (hud_v3 && hud3_pen_wheel) return false;   // v3: the S-Pen side button opens the wheel
		return wnd.get_key_state(225) || wnd.get_key_state(229);
	}

	rect queue_button_area() {
		if (!queue_button_visible) return {};
		if (!ui_legacy) return hud2_queue_strip(); // right-edge vertical strip
		int w = console_len(52.0f), h = console_len(24.0f);
		if (w <= 0) return {};
		return {{12, 46}, {12 + w, 46 + h}};
	}

	bool queue_button_hit(int mx, int my) {
		rect r = queue_button_area();
		return r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y;
	}

	// Zoom -/+ buttons under the QUEUE button (also usable when pinch is awkward).
	// v0.64 item 6: off by default. Pinch-zoom works and these are rudimentary,
	// so they are opt-in from Settings rather than permanent screen furniture.
	// An empty rect both hides the button and disables its hit test.
	bool show_zoom_buttons = false;
	// Dev: when >= 0, overlay a labelled grid of cmdicons.grp frames starting
	// here. The only way to establish which frame is which without guessing --
	// the historic comment block naming these was demonstrably wrong (it called
	// 232 "attack"; 232 is Repair).
	int cmdicon_debug_base = -1;
	rect zoom_button_area(int which) {
		if (!show_zoom_buttons) return {};
		if (hud_v3) return R3(12 + which * 52, 8 + 48 + 8, 44, 40, false);   // under MENU
		int sz = console_len(26.0f);
		if (sz <= 0) return {};
		// Sit under the QUEUE toggle, or take its place when it is hidden.
		int y = queue_button_visible ? 46 + console_len(24.0f) + 8 : 46;
		int x = 12 + which * (sz + 6);
		return {{x, y}, {x + sz, y + sz}};
	}

	void draw_zoom_buttons(uint32_t* data, size_t pitch) {
		for (int i = 0; i < 2; ++i) {
			rect r = zoom_button_area(i);
			if (r.to.x <= r.from.x) continue;
			fill_rect_rgba(data, pitch, r, hud_theme().util_fill);
			line_rectangle_rgba(data, pitch, r, hud_theme().util_line);
			const char* label = i == 0 ? "-" : "+";
			int tw = usp_adv();
			draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - tw / 2,
				(r.from.y + r.to.y) / 2 - usp_h() / 2, label, 0xffc0c8d0u);
		}
	}

	bool zoom_button_hit(int mx, int my, int& delta) {
		for (int i = 0; i < 2; ++i) {
			rect r = zoom_button_area(i);
			if (r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) {
				delta = i == 0 ? -1 : 1;
				return true;
			}
		}
		return false;
	}

	void draw_queue_button(uint32_t* data, size_t pitch) {
		rect r = queue_button_area();
		if (r.to.x <= r.from.x) return;
		bool on = order_queued();
		hud_theme_t th = hud_theme();
		fill_rect_rgba(data, pitch, r, on ? th.on_fill : th.util_fill);
		line_rectangle_rgba(data, pitch, r, on ? th.on_line : th.util_line);
		const char* label = "QUEUE";
		int tw = (int)strlen(label) * (ui::unispace_w + 1);
		draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - tw / 2,
			(r.from.y + r.to.y) / 2 - ui::unispace_h / 2, label, on ? th.on_text : 0xffb0b8c0u);
	}

	// task 2: the auto-worker toggle button. Same footprint as the QUEUE strip,
	// sitting one strip-height above it. Only present in HUD v2 and only when the
	// SCBW2 auto-worker rule is enabled in settings (qol.auto_worker).
	rect auto_worker_button_area() {
		if (ui_legacy || !st.game || !st.game->qol.auto_worker) return {};
		if (hud_v3) return hud3_layout().autow;
		rect q = hud2_queue_strip();
		if (q.to.x <= q.from.x) return {};
		int h = q.to.y - q.from.y, gap = hud2_margin();
		int top = q.from.y - gap - h;
		if (top < 0) return {};
		return {{q.from.x, top}, {q.to.x, q.from.y - gap}};
	}
	bool auto_worker_button_hit(int mx, int my) {
		rect r = auto_worker_button_area();
		return r.to.x > r.from.x && mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y;
	}
	void draw_auto_worker_button(uint32_t* data, size_t pitch) {
		rect r = auto_worker_button_area();
		if (r.to.x <= r.from.x) return;
		hud_theme_t th = hud_theme();
		bool on = auto_worker_active;
		fill_rect_rgba(data, pitch, r, on ? th.on_fill : th.util_fill);
		line_rectangle_rgba(data, pitch, r, on ? th.on_line : th.util_line);
		race_t race = (game_local_player >= 0 && game_local_player < 12) ? st.players[game_local_player].race : race_t::terran;
		const char* wn = race == race_t::protoss ? "PROBE" : race == race_t::zerg ? "DRONE" : "SCV";
		char full[16]; snprintf(full, sizeof full, "AUTO %s", wn);
		int avail = r.to.x - r.from.x;
		const char* lbl = ((int)strlen(full) * usp_adv() <= avail - hud_h_px(4)) ? full : wn;
		int tw = (int)strlen(lbl) * usp_adv();
		draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - tw / 2,
			(r.from.y + r.to.y) / 2 - usp_h() / 2, lbl, on ? th.on_text : 0xffb0b8c0u);
	}

	// In-game elapsed timer (MM:SS) at the very top of the screen, centered.
	void draw_game_timer(uint32_t* data, size_t pitch) {
		char buf[16];
		if (st.countdown_timer_ms > 0) {
			// Timed modes (Slaughter / Sudden Death / timed missions): show the
			// remaining time (reverse timer) in place of the elapsed clock - no
			// point in two opposite clocks at once (user request v0.29).
			int secs = st.countdown_timer_ms / 1000;
			snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
			int tw = (int)strlen(buf) * usp_adv();
			uint32_t col = secs < 30 ? 0xffff5050u : 0xffffb040u; // urgent under 30s
			draw_text_rgba(data, pitch, (int)screen_width / 2 - tw / 2, 6 * hud_us(), buf, col);
			return;
		}
		int secs = (int)((int64_t)st.current_frame * 42 / 1000);
		snprintf(buf, sizeof(buf), "%02d:%02d", secs / 60, secs % 60);
		int tw = (int)strlen(buf) * usp_adv();
		draw_text_rgba(data, pitch, (int)screen_width / 2 - tw / 2, 6 * hud_us(), buf, 0xffd8e4c0u);
	}

	// Small FPS readout in the very top-left corner (like the timer).
	void draw_fps(uint32_t* data, size_t pitch) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d fps", fps_display);
		draw_text_rgba(data, pitch, 6 * hud_us(), 6 * hud_us(), buf, 0xff90a0a0u);
	}

	// Local player's APM (actions per minute), under the FPS readout. The apm_t
	// history was recorded but never surfaced (update() was uncalled) - it is
	// UI-only state, so updating/reading it here does not touch determinism.
	void draw_apm(uint32_t* data, size_t pitch) {
		if (!game_input || game_local_player < 0) return;
		apm[game_local_player].update((int)st.current_frame);
		char buf[16];
		snprintf(buf, sizeof(buf), "APM %d", apm[game_local_player].current_apm);
		// Top-center, just below the game timer (top-left is busy with buttons).
		int tw = (int)strlen(buf) * usp_adv();
		draw_text_rgba(data, pitch, (int)screen_width / 2 - tw / 2, (6 + 4) * hud_us() + usp_h(), buf, 0xff8894a0u);
	}

	void load_cmdicons() {
		if (cmdicons_tried) return;
		cmdicons_tried = true;
		if (!load_data_file) return;
		try {
			a_vector<uint8_t> buf;
			load_data_file(buf, "unit\\cmdbtns\\cmdicons.grp");
			if (buf.empty()) return;
			data_loading::data_reader_le r(buf.data(), buf.data() + buf.size());
			cmdicons_grp = read_grp(r);
			// The icon palette lives in the PCX trailer: a 0x0c marker byte
			// followed by 768 RGB bytes (256 entries) at the end of the file.
			a_vector<uint8_t> pbuf;
			load_data_file(pbuf, "unit\\cmdbtns\\ticon.pcx");
			bool footer = pbuf.size() >= 769 && pbuf[pbuf.size() - 769] == 0x0c;
			if (!footer) {
				// v0.83.012: the palette is optional, the icons are not.
				// cmdicons_ok used to be set only inside the footer branch, so
				// a mod shipping its own ticon.pcx without the 0x0c trailer
				// silently lost EVERY SD command icon while cmdicons.grp
				// itself had loaded fine. Retail has the trailer, so this path
				// only ever bites modders -- which is exactly who we now
				// support. Fall back to a straight grayscale ramp: draw_cmdicon
				// tints by luminance into the fixed HUD gold, so a monotonic
				// ramp still produces readable icons.
				for (int i = 0; i != 256; ++i) {
					uint32_t g = (uint32_t)i;
					cmdicon_rgb[i] = (g << 16) | (g << 8) | g;
				}
				ui::log("cmdicons: ticon.pcx has no 0x0c palette trailer (%d bytes)"
					" -- using a grayscale ramp\n", (int)pbuf.size());
			}
			if (footer) {
				const uint8_t* pal = pbuf.data() + pbuf.size() - 768;
				for (int i = 0; i != 256; ++i) {
					cmdicon_rgb[i] = ((uint32_t)pal[i * 3 + 0] << 16)
						| ((uint32_t)pal[i * 3 + 1] << 8) | (uint32_t)pal[i * 3 + 2];
				}
			}
			// Indices 8..15 are the team-color placeholder range (0xff00ff
			// magenta in ticon.pcx). draw_cmdicon() renders the whole icon as
			// a luminance-tinted gold (the authentic HUD icons are always the
			// fixed gold/yellow color, never player-colored), so here we just
			// give 8..15 a neutral gray ramp for well-defined luminance.
			for (int k = 0; k != 8; ++k) {
				uint32_t g = (uint32_t)(40 + 200 * k / 7);
				cmdicon_rgb[8 + k] = (g << 16) | (g << 8) | g;
			}
			{
				std::vector<uint32_t> tp; int tw = 0, th = 0;
				if (loading_screen::decode_pcx8(pbuf.data(), pbuf.size(), tp, tw, th) && th >= 1 && tw >= 96) {
					for (int t = 0; t != 6; ++t) for (int i = 0; i != 16; ++i) cmdicon_tbl[t][i] = tp[(size_t)(t * 16 + i)] & 0xffffffu;
					cmdicon_tbl_ok = true;
				}
			}
			// The GRP decides this, not the palette.
			cmdicons_ok = !cmdicons_grp.frames.empty();
		} catch (...) {
		}
	}

	// cmdicons.grp frame for a card button. Unit/building buttons use the icon
	// at the UnitTypes id; generic commands map to fixed command-icon frames
	// (identified from the cmdicons grid: 228=move, 229=stop, 231=build,
	// 232=attack, 236=cancel, ...).
	// Authentic cmdicons.grp frame for a no-target ability opcode. Frame index
	// = PyMS Icons.txt line - 1 (0-based grp order; verified: Stop = 229).
	static int ability_opcode_icon(int opcode) {
		switch (opcode) {
		case 30: return 233; // return cargo
		case 37: return 246; // unsiege -> Tank Mode
		case 38: return 245; // siege -> Siege Mode
		case 40: return 312; // unload all
		case 43: return 255; // hold position
		case 44: return 259; // burrow
		case 45: return 260; // unburrow
		case 33: return 252; // cloak
		case 34: return 253; // decloak
		case 54: return 237; // stim pack
		case 42: return 68;  // morph archon (Archon unit icon)
		case 90: return 63;  // morph dark archon (Dark Archon unit icon)
		default: return -1;
		}
	}
	// cmdicons frames referenced outside the card as well (the IDLE button
	// borrows the Repair icon). Frame index = PyMS Icons.txt line - 1.
	static constexpr int cmdicon_repair = 232;
	// Unit buttons use frame == unit id; Zerg Larva is unit 35 (stat_txt.tbl
	// entry 36, 1-based). v0.66 guessed 261, which was simply wrong.
	static constexpr int cmdicon_larva = 35;
	static constexpr int cmdicon_move = 228;    // PyMS order: Move, Stop(229), Attack(230)
	static constexpr int cmdicon_attack = 230;
	static constexpr int cmdicon_liftoff = 282;
	static constexpr int cmdicon_land = 283;

	// Authentic cmdicons frame for an armed-target order.
	int order_icon(Orders o) {
		switch (o) {
		case Orders::Repair: return cmdicon_repair;
		case Orders::Move: return cmdicon_move;   // v0.66 added the button, not the icon
		case Orders::Patrol: return 254;
		case Orders::BuildingLand: return cmdicon_land;
		case Orders::CastLockdown: return 240;
		case Orders::CastNuclearStrike: return 311;
		case Orders::CastPsionicStorm: return 275;
		case Orders::CastDefensiveMatrix: return 247;
		case Orders::CastEMPShockwave: return 241;
		case Orders::CastIrradiate: return 242;
		case Orders::CastHallucination: return 277;
		case Orders::CastStasisField: return 278;
		case Orders::CastRecall: return 280;
		case Orders::CastDisruptionWeb: return 369;   // techdata.dat (measured: loadscreen_uicheck card)
		case Orders::CastMindControl: return 371;
		case Orders::CastFeedback: return 372;
		case Orders::CastMaelstrom: return 381;
		case Orders::CastDarkSwarm: return 270;
		case Orders::CastPlague: return 265;
		case Orders::CastConsume: return 302;
		case Orders::CastEnsnare: return 267;
		case Orders::CastParasite: return 271;
		case Orders::CastInfestation: return 313;
		case Orders::CastSpawnBroodlings: return 40; // Broodling unit icon
		case Orders::CastRestoration: return 366;
		case Orders::CastOpticalFlare: return 373;
		default: return -1;
		}
	}
	static int tech_icon(TechTypes t) {
		switch (t) {
		case TechTypes::Stim_Packs: return 237;
		case TechTypes::Lockdown: return 240;
		case TechTypes::EMP_Shockwave: return 241;
		case TechTypes::Spider_Mines: return 243;
		case TechTypes::Tank_Siege_Mode: return 245;
		case TechTypes::Defensive_Matrix: return 247;
		case TechTypes::Irradiate: return 242;
		case TechTypes::Yamato_Gun: return 251;
		case TechTypes::Cloaking_Field: return 252;
		case TechTypes::Personnel_Cloaking: return 252;
		case TechTypes::Burrowing: return 259;
		case TechTypes::Infestation: return 313;
		case TechTypes::Spawn_Broodlings: return 40;
		case TechTypes::Dark_Swarm: return 270;
		case TechTypes::Plague: return 265;
		case TechTypes::Consume: return 302;
		case TechTypes::Ensnare: return 267;
		case TechTypes::Parasite: return 271;
		case TechTypes::Psionic_Storm: return 275;
		case TechTypes::Hallucination: return 277;
		case TechTypes::Recall: return 280;
		case TechTypes::Stasis_Field: return 278;
		case TechTypes::Restoration: return 366;
		case TechTypes::Disruption_Web: return 369;
		case TechTypes::Mind_Control: return 371;
		case TechTypes::Optical_Flare: return 373;
		case TechTypes::Maelstrom: return 381;
		case TechTypes::Lurker_Aspect: return 376;
		case TechTypes::Healing: return 365;
		default: return -1;
		}
	}
	// v0.82.009: upgrades.dat carries its own icon index (verified against the
	// user's MPQ with `aidump equip`: every entry matches the table below), so
	// read it from the data and keep the table only as a fallback for when the
	// types are not loaded yet. This also makes mods that re-point an upgrade
	// icon work instead of being overridden by a baked-in guess.
	int upgrade_icon(UpgradeTypes u) const {
		if ((size_t)u < game_st.upgrade_types.vec.size()) {
			int icon = game_st.upgrade_types.vec[(size_t)u].icon;
			if (icon > 0) return icon;
		}
		return upgrade_icon_fallback(u);
	}
	static int upgrade_icon_fallback(UpgradeTypes u) {
		switch (u) {
		case UpgradeTypes::Terran_Infantry_Armor: return 292;
		case UpgradeTypes::Terran_Vehicle_Plating: return 293;
		case UpgradeTypes::Terran_Ship_Plating: return 291;
		case UpgradeTypes::Zerg_Carapace: return 297;
		case UpgradeTypes::Zerg_Flyer_Carapace: return 298;
		case UpgradeTypes::Protoss_Ground_Armor: return 303;
		case UpgradeTypes::Protoss_Air_Armor: return 304;
		case UpgradeTypes::Terran_Infantry_Weapons: return 288;
		case UpgradeTypes::Terran_Vehicle_Weapons: return 289;
		case UpgradeTypes::Terran_Ship_Weapons: return 290;
		case UpgradeTypes::Zerg_Melee_Attacks: return 299;
		case UpgradeTypes::Zerg_Missile_Attacks: return 300;
		case UpgradeTypes::Zerg_Flyer_Attacks: return 301;
		case UpgradeTypes::Protoss_Ground_Weapons: return 305;
		case UpgradeTypes::Protoss_Air_Weapons: return 306;
		case UpgradeTypes::Protoss_Plasma_Shields: return 310;
		case UpgradeTypes::U_238_Shells: return 238;
		case UpgradeTypes::Ion_Thrusters: return 287;
		case UpgradeTypes::Titan_Reactor: return 248;
		case UpgradeTypes::Ocular_Implants: return 249;
		case UpgradeTypes::Moebius_Reactor: return 256;
		case UpgradeTypes::Apollo_Reactor: return 284;
		case UpgradeTypes::Colossus_Reactor: return 285;
		case UpgradeTypes::Ventral_Sacs: return 261;
		case UpgradeTypes::Antennae: return 262;
		case UpgradeTypes::Pneumatized_Carapace: return 296;
		case UpgradeTypes::Metabolic_Boost: return 263;
		case UpgradeTypes::Adrenal_Glands: return 264;
		case UpgradeTypes::Muscular_Augments: return 266;
		case UpgradeTypes::Grooved_Spines: return 268;
		case UpgradeTypes::Gamete_Meiosis: return 294;
		case UpgradeTypes::Metasynaptic_Node: return 295;
		case UpgradeTypes::Singularity_Charge: return 281;
		case UpgradeTypes::Leg_Enhancements: return 307;
		case UpgradeTypes::Scarab_Damage: return 314;
		case UpgradeTypes::Reaver_Capacity: return 315;
		case UpgradeTypes::Gravitic_Drive: return 316;
		case UpgradeTypes::Sensor_Array: return 317;
		case UpgradeTypes::Gravitic_Boosters: return 276;
		case UpgradeTypes::Khaydarin_Amulet: return 318;
		case UpgradeTypes::Apial_Sensors: return 319;
		case UpgradeTypes::Gravitic_Thrusters: return 320;
		case UpgradeTypes::Carrier_Capacity: return 321;
		case UpgradeTypes::Khaydarin_Core: return 322;
		case UpgradeTypes::Argus_Jewel: return 383;
		case UpgradeTypes::Argus_Talisman: return 385;
		case UpgradeTypes::Caduceus_Reactor: return 384;
		case UpgradeTypes::Chitinous_Plating: return 379;
		case UpgradeTypes::Anabolic_Synthesis: return 378;
		case UpgradeTypes::Charon_Boosters: return 380;
		default: return -1;
		}
	}

	// Prerequisite building/tech for a trainable/morphable unit, for the
	// command-card tooltip. Returns null when the requirement is just the
	// producer itself (obvious from context). Hardcoded (SC keeps this in the
	// exe, not the .dat data OpenBW exposes).
	static const char* unit_requirement(UnitTypes t) {
		switch (t) {
		// Terran
		case UnitTypes::Terran_Firebat: return "Academy";
		case UnitTypes::Terran_Medic: return "Academy";
		case UnitTypes::Terran_Ghost: return "Academy + Covert Ops";
		case UnitTypes::Terran_Siege_Tank_Tank_Mode: return "Machine Shop";
		case UnitTypes::Terran_Goliath: return "Armory";
		case UnitTypes::Terran_Science_Vessel: return "Science Facility";
		case UnitTypes::Terran_Battlecruiser: return "Physics Lab";
		case UnitTypes::Terran_Valkyrie: return "Armory + Control Tower";
		case UnitTypes::Terran_Nuclear_Silo: return "Covert Ops";
		// Protoss
		case UnitTypes::Protoss_Dragoon: return "Cybernetics Core";
		case UnitTypes::Protoss_High_Templar: return "Templar Archives";
		case UnitTypes::Protoss_Dark_Templar: return "Templar Archives";
		case UnitTypes::Protoss_Reaver: return "Robotics Support Bay";
		case UnitTypes::Protoss_Observer: return "Observatory";
		case UnitTypes::Protoss_Carrier: return "Fleet Beacon";
		case UnitTypes::Protoss_Arbiter: return "Arbiter Tribunal";
		// Zerg
		case UnitTypes::Zerg_Hydralisk: return "Hydralisk Den";
		case UnitTypes::Zerg_Mutalisk: return "Spire";
		case UnitTypes::Zerg_Scourge: return "Spire";
		case UnitTypes::Zerg_Queen: return "Queen's Nest";
		case UnitTypes::Zerg_Ultralisk: return "Ultralisk Cavern";
		case UnitTypes::Zerg_Defiler: return "Defiler Mound";
		case UnitTypes::Zerg_Lurker: return "Lurker Aspect";
		case UnitTypes::Zerg_Guardian: return "Greater Spire";
		case UnitTypes::Zerg_Devourer: return "Greater Spire";
		// v0.83.034: Zerg building tier gates -- so the BUILD menu and the
		// Lair/Hive/Sunken/Spore/Greater-Spire morph buttons show what a new
		// player needs. The producing Hatchery/Lair/Hive is implicit.
		case UnitTypes::Zerg_Lair: return "Spawning Pool";
		case UnitTypes::Zerg_Hive: return "Queen's Nest";
		case UnitTypes::Zerg_Hydralisk_Den: return "Spawning Pool";
		case UnitTypes::Zerg_Spire: return "Lair";
		case UnitTypes::Zerg_Greater_Spire: return "Hive";
		case UnitTypes::Zerg_Queens_Nest: return "Lair";
		case UnitTypes::Zerg_Defiler_Mound: return "Hive";
		case UnitTypes::Zerg_Ultralisk_Cavern: return "Hive";
		case UnitTypes::Zerg_Nydus_Canal: return "Hive";
		case UnitTypes::Zerg_Creep_Colony: return "Spawning Pool";
		case UnitTypes::Zerg_Sunken_Colony: return "Spawning Pool";
		case UnitTypes::Zerg_Spore_Colony: return "Evolution Chamber";
		default: return nullptr;
		}
	}

	// Building a tech/upgrade needs the lab where it is researched (and, for a
	// few, an extra tier). Shown as "Need:" in the tooltip so players learn the
	// tech tree. nullptr = no listed requirement.
	static const char* tech_requirement(TechTypes t) {
		switch (t) {
		case TechTypes::Stim_Packs: case TechTypes::Restoration: case TechTypes::Optical_Flare: return "Academy";
		case TechTypes::Tank_Siege_Mode: case TechTypes::Spider_Mines: return "Machine Shop";
		case TechTypes::Cloaking_Field: return "Control Tower";
		case TechTypes::Personnel_Cloaking: case TechTypes::Lockdown: return "Covert Ops";
		case TechTypes::EMP_Shockwave: case TechTypes::Irradiate: return "Science Facility";
		case TechTypes::Yamato_Gun: return "Physics Lab";
		case TechTypes::Psionic_Storm: case TechTypes::Hallucination: case TechTypes::Mind_Control: case TechTypes::Maelstrom: return "Templar Archives";
		case TechTypes::Recall: case TechTypes::Stasis_Field: return "Arbiter Tribunal";
		case TechTypes::Disruption_Web: return "Fleet Beacon";
		case TechTypes::Lurker_Aspect: return "Hydralisk Den + Lair";
		case TechTypes::Ensnare: case TechTypes::Spawn_Broodlings: return "Queen's Nest";
		case TechTypes::Plague: case TechTypes::Consume: return "Defiler Mound";
		default: return nullptr;
		}
	}

	static const char* upgrade_requirement(UpgradeTypes u) {
		switch (u) {
		case UpgradeTypes::U_238_Shells: case UpgradeTypes::Caduceus_Reactor: return "Academy";
		case UpgradeTypes::Ion_Thrusters: case UpgradeTypes::Charon_Boosters: return "Machine Shop";
		case UpgradeTypes::Apollo_Reactor: return "Control Tower";
		case UpgradeTypes::Titan_Reactor: return "Science Facility";
		case UpgradeTypes::Ocular_Implants: case UpgradeTypes::Moebius_Reactor: return "Covert Ops";
		case UpgradeTypes::Colossus_Reactor: return "Physics Lab";
		case UpgradeTypes::Singularity_Charge: return "Cybernetics Core";
		case UpgradeTypes::Leg_Enhancements: return "Citadel of Adun";
		case UpgradeTypes::Khaydarin_Amulet: return "Templar Archives";
		case UpgradeTypes::Khaydarin_Core: return "Arbiter Tribunal";
		case UpgradeTypes::Scarab_Damage: case UpgradeTypes::Reaver_Capacity: case UpgradeTypes::Gravitic_Drive: return "Robotics Support Bay";
		case UpgradeTypes::Sensor_Array: case UpgradeTypes::Gravitic_Boosters: return "Observatory";
		case UpgradeTypes::Carrier_Capacity: case UpgradeTypes::Apial_Sensors: case UpgradeTypes::Gravitic_Thrusters: return "Fleet Beacon";
		case UpgradeTypes::Metabolic_Boost: case UpgradeTypes::Adrenal_Glands: return "Spawning Pool";
		case UpgradeTypes::Muscular_Augments: case UpgradeTypes::Grooved_Spines: return "Hydralisk Den";
		case UpgradeTypes::Gamete_Meiosis: return "Queen's Nest";
		case UpgradeTypes::Metasynaptic_Node: return "Defiler Mound";
		case UpgradeTypes::Anabolic_Synthesis: case UpgradeTypes::Chitinous_Plating: return "Ultralisk Cavern";
		default: return nullptr;
		}
	}

	// Official (in-game) name for a tech, so the tooltip shows "Stim Packs" rather
	// than the terse command-card code ("STIM"). Names match the original game.
	static const char* tech_official_name(TechTypes t) {
		switch (t) {
		case TechTypes::Stim_Packs: return "Stim Packs";
		case TechTypes::Lockdown: return "Lockdown";
		case TechTypes::EMP_Shockwave: return "EMP Shockwave";
		case TechTypes::Spider_Mines: return "Spider Mines";
		case TechTypes::Scanner_Sweep: return "Scanner Sweep";
		case TechTypes::Tank_Siege_Mode: return "Siege Mode";
		case TechTypes::Defensive_Matrix: return "Defensive Matrix";
		case TechTypes::Irradiate: return "Irradiate";
		case TechTypes::Yamato_Gun: return "Yamato Gun";
		case TechTypes::Cloaking_Field: return "Cloaking Field";
		case TechTypes::Personnel_Cloaking: return "Personnel Cloaking";
		case TechTypes::Burrowing: return "Burrow";
		case TechTypes::Infestation: return "Infestation";
		case TechTypes::Spawn_Broodlings: return "Spawn Broodlings";
		case TechTypes::Dark_Swarm: return "Dark Swarm";
		case TechTypes::Plague: return "Plague";
		case TechTypes::Consume: return "Consume";
		case TechTypes::Ensnare: return "Ensnare";
		case TechTypes::Parasite: return "Parasite";
		case TechTypes::Psionic_Storm: return "Psionic Storm";
		case TechTypes::Hallucination: return "Hallucination";
		case TechTypes::Recall: return "Recall";
		case TechTypes::Stasis_Field: return "Stasis Field";
		case TechTypes::Archon_Warp: return "Archon Warp";
		case TechTypes::Restoration: return "Restoration";
		case TechTypes::Disruption_Web: return "Disruption Web";
		case TechTypes::Mind_Control: return "Mind Control";
		case TechTypes::Dark_Archon_Meld: return "Dark Archon Meld";
		case TechTypes::Feedback: return "Feedback";
		case TechTypes::Optical_Flare: return "Optical Flare";
		case TechTypes::Maelstrom: return "Maelstrom";
		case TechTypes::Lurker_Aspect: return "Lurker Aspect";
		default: return nullptr;
		}
	}

	// What the tech does, phrased as in the original game / SC wiki. One line.
	static const char* tech_effect(TechTypes t) {
		switch (t) {
		case TechTypes::Stim_Packs: return "+50% attack/move speed, costs 10 HP";
		case TechTypes::Lockdown: return "Disables a mechanical unit; 100 energy";
		case TechTypes::EMP_Shockwave: return "Drains shields and energy in area; 100 en";
		case TechTypes::Spider_Mines: return "Vulture lays 3 cloaked burrowing mines";
		case TechTypes::Scanner_Sweep: return "Reveals and detects an area; 50 energy";
		case TechTypes::Tank_Siege_Mode: return "Range 7 to 12, splash, cannot move";
		case TechTypes::Defensive_Matrix: return "250 HP damage shield on a unit";
		case TechTypes::Irradiate: return "250 dmg over time to biologicals; 75 en";
		case TechTypes::Yamato_Gun: return "Single blast for 260 damage; 150 energy";
		case TechTypes::Cloaking_Field: return "Cloaks the Wraith, drains energy";
		case TechTypes::Personnel_Cloaking: return "Cloaks the Ghost, drains energy";
		case TechTypes::Burrowing: return "Zerg units burrow to hide from sight";
		case TechTypes::Infestation: return "Infests a damaged Command Center";
		case TechTypes::Spawn_Broodlings: return "Kills target, spawns 2 Broodlings; 150 en";
		case TechTypes::Dark_Swarm: return "Blocks ranged attacks in the area";
		case TechTypes::Plague: return "Damage over time, cannot kill; 150 energy";
		case TechTypes::Consume: return "Sacrifice own unit for +50 energy";
		case TechTypes::Ensnare: return "Slows units, reveals cloaked; 75 energy";
		case TechTypes::Parasite: return "Vision through the target unit; 75 energy";
		case TechTypes::Psionic_Storm: return "112 damage over an area; 75 energy";
		case TechTypes::Hallucination: return "Creates 2 illusory copies; 100 energy";
		case TechTypes::Recall: return "Teleports units to the Arbiter; 150 en";
		case TechTypes::Stasis_Field: return "Freezes units invulnerable; 100 energy";
		case TechTypes::Archon_Warp: return "Merges 2 High Templar into an Archon";
		case TechTypes::Restoration: return "Removes debuffs from a unit; 50 energy";
		case TechTypes::Disruption_Web: return "Ground units in area cannot attack";
		case TechTypes::Mind_Control: return "Permanently take control of a unit";
		case TechTypes::Dark_Archon_Meld: return "Merges 2 Dark Templar into a Dark Archon";
		case TechTypes::Feedback: return "Damages target by its energy; 50 energy";
		case TechTypes::Optical_Flare: return "Blinds unit, disables detection; 75 en";
		case TechTypes::Maelstrom: return "Stuns biological units in an area";
		case TechTypes::Lurker_Aspect: return "Lets Hydralisks morph into Lurkers";
		default: return nullptr;
		}
	}

	static const char* upgrade_official_name(UpgradeTypes u) {
		switch (u) {
		case UpgradeTypes::Terran_Infantry_Weapons: return "Infantry Weapons";
		case UpgradeTypes::Terran_Infantry_Armor: return "Infantry Armor";
		case UpgradeTypes::Terran_Vehicle_Weapons: return "Vehicle Weapons";
		case UpgradeTypes::Terran_Vehicle_Plating: return "Vehicle Plating";
		case UpgradeTypes::Terran_Ship_Weapons: return "Ship Weapons";
		case UpgradeTypes::Terran_Ship_Plating: return "Ship Plating";
		case UpgradeTypes::U_238_Shells: return "U-238 Shells";
		case UpgradeTypes::Ion_Thrusters: return "Ion Thrusters";
		case UpgradeTypes::Titan_Reactor: return "Titan Reactor";
		case UpgradeTypes::Ocular_Implants: return "Ocular Implants";
		case UpgradeTypes::Moebius_Reactor: return "Moebius Reactor";
		case UpgradeTypes::Apollo_Reactor: return "Apollo Reactor";
		case UpgradeTypes::Colossus_Reactor: return "Colossus Reactor";
		case UpgradeTypes::Caduceus_Reactor: return "Caduceus Reactor";
		case UpgradeTypes::Charon_Boosters: return "Charon Boosters";
		case UpgradeTypes::Protoss_Ground_Weapons: return "Ground Weapons";
		case UpgradeTypes::Protoss_Ground_Armor: return "Ground Armor";
		case UpgradeTypes::Protoss_Plasma_Shields: return "Plasma Shields";
		case UpgradeTypes::Protoss_Air_Weapons: return "Air Weapons";
		case UpgradeTypes::Protoss_Air_Armor: return "Air Armor";
		case UpgradeTypes::Singularity_Charge: return "Singularity Charge";
		case UpgradeTypes::Leg_Enhancements: return "Leg Enhancements";
		case UpgradeTypes::Scarab_Damage: return "Scarab Damage";
		case UpgradeTypes::Reaver_Capacity: return "Reaver Capacity";
		case UpgradeTypes::Gravitic_Drive: return "Gravitic Drive";
		case UpgradeTypes::Gravitic_Thrusters: return "Gravitic Thrusters";
		case UpgradeTypes::Gravitic_Boosters: return "Gravitic Boosters";
		case UpgradeTypes::Sensor_Array: return "Sensor Array";
		case UpgradeTypes::Apial_Sensors: return "Apial Sensors";
		case UpgradeTypes::Khaydarin_Amulet: return "Khaydarin Amulet";
		case UpgradeTypes::Khaydarin_Core: return "Khaydarin Core";
		case UpgradeTypes::Argus_Jewel: return "Argus Jewel";
		case UpgradeTypes::Argus_Talisman: return "Argus Talisman";
		case UpgradeTypes::Carrier_Capacity: return "Carrier Capacity";
		case UpgradeTypes::Zerg_Melee_Attacks: return "Melee Attacks";
		case UpgradeTypes::Zerg_Missile_Attacks: return "Missile Attacks";
		case UpgradeTypes::Zerg_Carapace: return "Carapace";
		case UpgradeTypes::Zerg_Flyer_Attacks: return "Flyer Attacks";
		case UpgradeTypes::Zerg_Flyer_Carapace: return "Flyer Carapace";
		case UpgradeTypes::Metabolic_Boost: return "Metabolic Boost";
		case UpgradeTypes::Adrenal_Glands: return "Adrenal Glands";
		case UpgradeTypes::Muscular_Augments: return "Muscular Augments";
		case UpgradeTypes::Grooved_Spines: return "Grooved Spines";
		case UpgradeTypes::Pneumatized_Carapace: return "Pneumatized Carapace";
		case UpgradeTypes::Antennae: return "Antennae";
		case UpgradeTypes::Ventral_Sacs: return "Ventral Sacs";
		case UpgradeTypes::Chitinous_Plating: return "Chitinous Plating";
		case UpgradeTypes::Anabolic_Synthesis: return "Anabolic Synthesis";
		case UpgradeTypes::Gamete_Meiosis: return "Gamete Meiosis";
		case UpgradeTypes::Metasynaptic_Node: return "Metasynaptic Node";
		default: return nullptr;
		}
	}

	static const char* upgrade_effect(UpgradeTypes u) {
		switch (u) {
		case UpgradeTypes::Terran_Infantry_Weapons: return "+1 dmg: Marine, Firebat, Ghost per lvl";
		case UpgradeTypes::Terran_Infantry_Armor: return "+1 armor: Marine, Firebat, Ghost, Medic";
		case UpgradeTypes::Terran_Vehicle_Weapons: return "+dmg: Vulture, Tank, Goliath per lvl";
		case UpgradeTypes::Terran_Vehicle_Plating: return "+1 armor: Vulture, Tank, Goliath";
		case UpgradeTypes::Terran_Ship_Weapons: return "+dmg: Wraith, BC, Valkyrie per lvl";
		case UpgradeTypes::Terran_Ship_Plating: return "+1 armor to Terran air units";
		case UpgradeTypes::U_238_Shells: return "+1 Marine attack range (4 to 5)";
		case UpgradeTypes::Ion_Thrusters: return "+Vulture movement speed";
		case UpgradeTypes::Titan_Reactor: return "+50 max energy for Science Vessels";
		case UpgradeTypes::Ocular_Implants: return "+2 Ghost sight range (9 to 11)";
		case UpgradeTypes::Moebius_Reactor: return "+50 max energy for Ghosts";
		case UpgradeTypes::Apollo_Reactor: return "+50 max energy for Wraiths";
		case UpgradeTypes::Colossus_Reactor: return "+50 max energy for Battlecruisers";
		case UpgradeTypes::Caduceus_Reactor: return "+50 max energy for Medics";
		case UpgradeTypes::Charon_Boosters: return "+3 Goliath air attack range (5 to 8)";
		case UpgradeTypes::Protoss_Ground_Weapons: return "+dmg: Zealot, Dragoon, DT, Archon";
		case UpgradeTypes::Protoss_Ground_Armor: return "+1 armor to Protoss ground units";
		case UpgradeTypes::Protoss_Plasma_Shields: return "+1 shields to all Protoss units";
		case UpgradeTypes::Protoss_Air_Weapons: return "+dmg: Scout, Corsair, Carrier, Arbiter";
		case UpgradeTypes::Protoss_Air_Armor: return "+1 armor to Protoss air units";
		case UpgradeTypes::Singularity_Charge: return "+2 Dragoon attack range (4 to 6)";
		case UpgradeTypes::Leg_Enhancements: return "+Zealot movement speed";
		case UpgradeTypes::Scarab_Damage: return "+25 Scarab damage (100 to 125)";
		case UpgradeTypes::Reaver_Capacity: return "+5 Scarab capacity (5 to 10)";
		case UpgradeTypes::Gravitic_Drive: return "+Shuttle movement speed";
		case UpgradeTypes::Gravitic_Thrusters: return "+Scout movement speed";
		case UpgradeTypes::Gravitic_Boosters: return "+Observer movement speed";
		case UpgradeTypes::Sensor_Array: return "+2 Observer sight range (9 to 11)";
		case UpgradeTypes::Apial_Sensors: return "+2 Scout sight range (8 to 10)";
		case UpgradeTypes::Khaydarin_Amulet: return "+50 max energy for High Templar";
		case UpgradeTypes::Khaydarin_Core: return "+50 max energy for Arbiters";
		case UpgradeTypes::Argus_Jewel: return "+50 max energy for Corsairs";
		case UpgradeTypes::Argus_Talisman: return "+50 max energy for Dark Archons";
		case UpgradeTypes::Carrier_Capacity: return "+4 Interceptor capacity (4 to 8)";
		case UpgradeTypes::Zerg_Melee_Attacks: return "+dmg: Zergling, Ultralisk, Broodling";
		case UpgradeTypes::Zerg_Missile_Attacks: return "+dmg: Hydralisk, Lurker per lvl";
		case UpgradeTypes::Zerg_Carapace: return "+1 armor to Zerg ground units";
		case UpgradeTypes::Zerg_Flyer_Attacks: return "+dmg: Mutalisk, Guardian, Devourer";
		case UpgradeTypes::Zerg_Flyer_Carapace: return "+1 armor to Zerg air units";
		case UpgradeTypes::Metabolic_Boost: return "+Zergling movement speed";
		case UpgradeTypes::Adrenal_Glands: return "+Zergling attack speed";
		case UpgradeTypes::Muscular_Augments: return "+Hydralisk movement speed";
		case UpgradeTypes::Grooved_Spines: return "+1 Hydralisk attack range (4 to 5)";
		case UpgradeTypes::Pneumatized_Carapace: return "+Overlord movement speed";
		case UpgradeTypes::Antennae: return "+2 Overlord sight range (9 to 11)";
		case UpgradeTypes::Ventral_Sacs: return "Overlords can transport ground units";
		case UpgradeTypes::Chitinous_Plating: return "+2 Ultralisk armor";
		case UpgradeTypes::Anabolic_Synthesis: return "+Ultralisk movement speed";
		case UpgradeTypes::Gamete_Meiosis: return "+50 max energy for Queens";
		case UpgradeTypes::Metasynaptic_Node: return "+50 max energy for Defilers";
		default: return nullptr;
		}
	}

	// #8/#7: one-line flavor for a card that trains/builds/morphs a unit or an
	// addon -- what the building is FOR (and what it trains/upgrades). Combat
	// units get a terse role line. nullptr = no flavor (tooltip stays cost-only).
	static const char* unit_flavor(UnitTypes t) {
		switch (t) {
		// --- Terran buildings ---
		case UnitTypes::Terran_Command_Center: return "Trains SCVs; drop-off for resources. Lifts off.";
		case UnitTypes::Terran_Supply_Depot: return "Provides 8 supply for more units.";
		case UnitTypes::Terran_Refinery: return "Built on gas geyser; SCVs harvest vespene.";
		case UnitTypes::Terran_Barracks: return "Trains Marines, Firebats, Medics, Ghosts.";
		case UnitTypes::Terran_Academy: return "Ghost/Medic tech: Stim, U-238, Restore, Optical.";
		case UnitTypes::Terran_Factory: return "Trains Vultures, Siege Tanks, Goliaths.";
		case UnitTypes::Terran_Starport: return "Trains Wraith, Dropship, Vessel, Valkyrie, BC.";
		case UnitTypes::Terran_Engineering_Bay: return "Upgrades infantry weapons/armor; detects.";
		case UnitTypes::Terran_Armory: return "Upgrades vehicle and ship weapons/armor.";
		case UnitTypes::Terran_Science_Facility: return "Vessel tech; unlocks the Physics Lab addon.";
		case UnitTypes::Terran_Missile_Turret: return "Anti-air defense; detects cloaked units.";
		case UnitTypes::Terran_Bunker: return "Garrison up to 4 infantry to fire from cover.";
		case UnitTypes::Terran_Nuclear_Silo: return "Builds a Nuclear Missile for the Ghost.";
		// --- Terran addons ---
		case UnitTypes::Terran_Comsat_Station: return "CC addon: Scanner Sweep reveals an area.";
		case UnitTypes::Terran_Control_Tower: return "Starport addon: enables Dropship/Vessel/Valk/BC.";
		case UnitTypes::Terran_Machine_Shop: return "Factory addon: Siege, Spider Mines, Ion Thrusters.";
		case UnitTypes::Terran_Covert_Ops: return "Sci Facility addon: Lockdown, Ghost energy/sight.";
		case UnitTypes::Terran_Physics_Lab: return "Sci Facility addon: unlocks the Battlecruiser Yamato.";
		// --- Zerg buildings ---
		case UnitTypes::Zerg_Hatchery: return "Spawns Larva; base structure. Morphs to Lair.";
		case UnitTypes::Zerg_Lair: return "Morphed Hatchery; unlocks Lair-tier tech. To Hive.";
		case UnitTypes::Zerg_Hive: return "Top base tier; unlocks Ultralisk/Defiler/Hive tech.";
		case UnitTypes::Zerg_Extractor: return "Built on gas geyser; Drones harvest vespene.";
		case UnitTypes::Zerg_Spawning_Pool: return "Enables Zerglings; Metabolic/Adrenal upgrades.";
		case UnitTypes::Zerg_Evolution_Chamber: return "Upgrades ground carapace and attacks.";
		case UnitTypes::Zerg_Hydralisk_Den: return "Enables Hydralisks; range/speed upgrades.";
		case UnitTypes::Zerg_Spire: return "Enables Mutalisks/Scourge; flyer upgrades.";
		case UnitTypes::Zerg_Greater_Spire: return "Enables Guardian/Devourer morph.";
		case UnitTypes::Zerg_Queens_Nest: return "Enables Queens; needed for Hive.";
		case UnitTypes::Zerg_Defiler_Mound: return "Enables Defilers; Plague and Consume.";
		case UnitTypes::Zerg_Ultralisk_Cavern: return "Enables Ultralisks; armor/speed upgrades.";
		case UnitTypes::Zerg_Nydus_Canal: return "Build a linked exit for instant travel.";
		case UnitTypes::Zerg_Creep_Colony: return "Morphs into a Sunken or Spore Colony.";
		case UnitTypes::Zerg_Sunken_Colony: return "Anti-ground defense on creep.";
		case UnitTypes::Zerg_Spore_Colony: return "Anti-air defense; detects cloaked units.";
		// --- Protoss buildings ---
		case UnitTypes::Protoss_Nexus: return "Trains Probes; drop-off for resources.";
		case UnitTypes::Protoss_Pylon: return "Provides 8 supply and powers nearby buildings.";
		case UnitTypes::Protoss_Assimilator: return "Built on gas geyser; Probes harvest vespene.";
		case UnitTypes::Protoss_Gateway: return "Trains Zealots, Dragoons, Templar, Dark Templar.";
		case UnitTypes::Protoss_Forge: return "Upgrades ground weapons/armor/shields; cannon.";
		case UnitTypes::Protoss_Cybernetics_Core: return "Enables Dragoons; air upgrades, Singularity.";
		case UnitTypes::Protoss_Photon_Cannon: return "Anti-ground/air defense; detects cloaked.";
		case UnitTypes::Protoss_Citadel_of_Adun: return "Leg Enhancements; enables Templar Archives.";
		case UnitTypes::Protoss_Templar_Archives: return "Templar tech: Storm, Hallucination, DT.";
		case UnitTypes::Protoss_Robotics_Facility: return "Trains Shuttles, Reavers, Observers.";
		case UnitTypes::Protoss_Robotics_Support_Bay: return "Reaver/Shuttle upgrades; Scarab capacity.";
		case UnitTypes::Protoss_Observatory: return "Enables Observers; sight/speed upgrades.";
		case UnitTypes::Protoss_Stargate: return "Trains Scouts, Corsairs, Carriers, Arbiters.";
		case UnitTypes::Protoss_Fleet_Beacon: return "Carrier/Scout/Corsair tech and upgrades.";
		case UnitTypes::Protoss_Arbiter_Tribunal: return "Arbiter tech: Recall, Stasis Field.";
		case UnitTypes::Protoss_Shield_Battery: return "Restores shields of nearby Protoss units.";
		// --- key combat/support units (role line) ---
		case UnitTypes::Terran_SCV: case UnitTypes::Protoss_Probe: case UnitTypes::Zerg_Drone:
			return "Worker: harvests minerals/gas and builds.";
		case UnitTypes::Terran_Medic: return "Heals biological units; Restore/Optical Flare.";
		case UnitTypes::Terran_Science_Vessel: return "Detector; Defensive Matrix, EMP, Irradiate.";
		case UnitTypes::Zerg_Overlord: return "Provides supply; detector; can transport (Ventral).";
		case UnitTypes::Protoss_Observer: return "Cloaked flying detector; scouts safely.";
		case UnitTypes::Protoss_Shuttle: case UnitTypes::Terran_Dropship:
			return "Transport: carries ground units.";
		default: return nullptr;
		}
	}

	// #9: ability tooltip name + one-line description (what it does, area vs
	// single, and whether it targets friend/foe). Keyed by (action, arg): for
	// cardact_ability_target arg is an Orders value; for cardact_ability arg is
	// the action opcode. nullptr name -> fall back to the terse card label.
	static const char* ability_name(int action, int arg) {
		if (action == cardact_ability_target) switch ((Orders)arg) {
		case Orders::CastScannerSweep: return "Scanner Sweep";
		case Orders::CastPsionicStorm: return "Psionic Storm";
		case Orders::CastStasisField: return "Stasis Field";
		case Orders::CastRecall: return "Recall";
		case Orders::CastLockdown: return "Lockdown";
		case Orders::FireYamatoGun: return "Yamato Gun";
		case Orders::CastEMPShockwave: return "EMP Shockwave";
		case Orders::CastIrradiate: return "Irradiate";
		case Orders::CastDefensiveMatrix: return "Defensive Matrix";
		case Orders::CastRestoration: return "Restoration";
		case Orders::CastOpticalFlare: return "Optical Flare";
		case Orders::CastHallucination: return "Hallucination";
		case Orders::CastMindControl: return "Mind Control";
		case Orders::CastFeedback: return "Feedback";
		case Orders::CastMaelstrom: return "Maelstrom";
		case Orders::CastDisruptionWeb: return "Disruption Web";
		case Orders::CastDarkSwarm: return "Dark Swarm";
		case Orders::CastPlague: return "Plague";
		case Orders::CastConsume: return "Consume";
		case Orders::CastEnsnare: return "Ensnare";
		case Orders::CastSpawnBroodlings: return "Spawn Broodlings";
		case Orders::CastParasite: return "Parasite";
		case Orders::CastInfestation: return "Infest Command Center";
		case Orders::PlaceMine: return "Spider Mines";
		case Orders::CastNuclearStrike: return "Nuclear Strike";
		default: return nullptr;
		}
		if (action == cardact_ability) switch (arg) {
		case 38: return "Siege Mode";
		case 37: return "Tank Mode";
		case 54: return "Stim Pack";
		case 43: return "Hold Position";
		case 30: return "Return Cargo";
		case 40: return "Unload All";
		case 42: return "Archon Warp";
		case 90: return "Dark Archon Meld";
		default: return nullptr;
		}
		return nullptr;
	}
	static const char* ability_desc(int action, int arg) {
		if (action == cardact_ability_target) switch ((Orders)arg) {
		case Orders::CastScannerSweep: return "Reveals an area; detects cloaked. No cost but energy.";
		case Orders::CastPsionicStorm: return "AOE damage over an area. Hits friend and foe.";
		case Orders::CastStasisField: return "AOE: freezes units, invulnerable, can't act.";
		case Orders::CastRecall: return "Teleports your units at target to the Arbiter.";
		case Orders::CastLockdown: return "Single target: disables one enemy mechanical unit.";
		case Orders::FireYamatoGun: return "Single target: 260 damage to one unit/building.";
		case Orders::CastEMPShockwave: return "AOE: drains shields and energy of all in area.";
		case Orders::CastIrradiate: return "Single target: radiation damages it and nearby bio.";
		case Orders::CastDefensiveMatrix: return "Single ally: shield absorbs 250 damage.";
		case Orders::CastRestoration: return "Single unit: removes negative status effects.";
		case Orders::CastOpticalFlare: return "Single enemy: blinds it, cutting sight range.";
		case Orders::CastHallucination: return "Single ally: makes 2 illusory copies of it.";
		case Orders::CastMindControl: return "Single enemy: permanently take control of it.";
		case Orders::CastFeedback: return "Single caster: burns its energy as damage.";
		case Orders::CastMaelstrom: return "AOE: stuns biological units for a short time.";
		case Orders::CastDisruptionWeb: return "AOE zone: units inside cannot attack.";
		case Orders::CastDarkSwarm: return "AOE zone: blocks ranged attacks on units under it.";
		case Orders::CastPlague: return "AOE: damages HP over time; can't kill. Friend/foe.";
		case Orders::CastConsume: return "Eat a friendly Zerg unit to restore energy.";
		case Orders::CastEnsnare: return "AOE: slows enemy units; reveals cloaked.";
		case Orders::CastSpawnBroodlings: return "Single enemy: kills it, spawns 2 Broodlings.";
		case Orders::CastParasite: return "Single enemy: see through it (shared vision).";
		case Orders::CastInfestation: return "Infest a damaged enemy Command Center.";
		case Orders::PlaceMine: return "Lay a hidden mine that attacks nearby enemies.";
		case Orders::CastNuclearStrike: return "Paint a target for the silo's Nuclear Missile.";
		default: return nullptr;
		}
		if (action == cardact_ability) switch (arg) {
		case 38: return "Root the Tank: long range, splash, cannot move.";
		case 37: return "Unroot the Tank so it can move again.";
		case 54: return "Costs HP for a burst of attack and move speed.";
		case 43: return "Stay put and attack anything in range.";
		case 30: return "Send workers to drop off carried resources.";
		case 40: return "Unload all passengers from this transport.";
		case 42: return "Merge two High Templar into an Archon.";
		case 90: return "Merge two Dark Templar into a Dark Archon.";
		default: return nullptr;
		}
		return nullptr;
	}

	int card_icon_frame(int action, int arg) {
		switch (action) {
		case cardact_build:
		case cardact_train:
		case cardact_morph:
		case cardact_morph_building:
		case cardact_addon:
			return arg; // unit/building icon frame == unit id
		case cardact_stop: return 229;
		case cardact_attack: return cmdicon_attack;
		case cardact_menu_build:
		case cardact_menu_adv: {
			// Race-specific Basic/Advanced Buildings icon.
			race_t race = race_t::terran;
			if (game_local_player >= 0) race = st.players[game_local_player].race;
			bool adv = action == cardact_menu_adv;
			if (race == race_t::zerg) return adv ? 258 : 257;
			if (race == race_t::protoss) return adv ? 273 : 272;
			return adv ? 235 : 234;
		}
		case cardact_menu_back: return 236;
		case cardact_place_cancel: return 236;
		case cardact_liftoff: return cmdicon_liftoff;
		case cardact_land: return cmdicon_land;
		case cardact_select_larva: return cmdicon_larva;
		case cardact_cancel: return 236;
		case cardact_ability: return ability_opcode_icon(arg);
		case cardact_ability_target: return order_icon((Orders)arg);
		case cardact_research: return tech_icon((TechTypes)arg);
		case cardact_upgrade: return upgrade_icon((UpgradeTypes)arg);
		default:
			return -1;
		}
	}

	// Blit one cmdicons frame into the RGBA HUD buffer, centered at (cx,cy)
	// via the frame's grp offset. Palette index 0 is transparent.
	void draw_cmdicon(uint32_t* data, size_t pitch, int frame_index, int cx, int cy) {
		// HD / user icon override (v0.23): pre-scaled ARGB frame, alpha blended.
		if (ddsgrp::frame_t* ov = get_icon_override(frame_index)) {
			int x0 = cx - ov->w / 2, y0 = cy - ov->h / 2;
			for (int y = 0; y != ov->h; ++y) {
				int py = y0 + y;
				if (py < 0 || py >= (int)screen_height) continue;
				uint32_t* row = data + (size_t)py * pitch;
				const uint32_t* srow = &ov->rgba[(size_t)y * ov->w];
				for (int x = 0; x != ov->w; ++x) {
					int px = x0 + x;
					if (px < 0 || px >= (int)screen_width) continue;
					uint32_t c = srow[x];
					uint32_t a = c >> 24;
					if (a < 8) continue;
					if (a >= 248) { row[px] = 0xff000000u | (c & 0xffffff); continue; }
					uint32_t old = row[px];
					uint32_t ia = 255 - a;
					uint32_t r = (((c >> 16) & 0xff) * a + ((old >> 16) & 0xff) * ia) / 255;
					uint32_t g = (((c >> 8) & 0xff) * a + ((old >> 8) & 0xff) * ia) / 255;
					uint32_t b = ((c & 0xff) * a + (old & 0xff) * ia) / 255;
					row[px] = 0xff000000u | (r << 16) | (g << 8) | b;
				}
			}
			return;
		}
		if (!cmdicons_ok || frame_index < 0 || frame_index >= (int)cmdicons_grp.frames.size()) return;
		auto& f = cmdicons_grp.frames[frame_index];
		size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return;
		cmdicon_scratch.assign(fw * fh, 0);
		draw_frame(f, false, cmdicon_scratch.data(), fw, 0, 0, fw, fh);
		int ox = cx - (int)cmdicons_grp.width / 2 + (int)f.offset.x;
		int oy = cy - (int)cmdicons_grp.height / 2 + (int)f.offset.y;
		for (size_t y = 0; y != fh; ++y) {
			int py = oy + (int)y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = cmdicon_scratch.data() + y * fw;
			for (size_t x = 0; x != fw; ++x) {
				uint8_t idx = srow[x];
				if (idx == 0) continue;
				int px = ox + (int)x;
				if (px < 0 || px >= (int)screen_width) continue;
				// The icon's own yellow table (ticon.pcx row 0), as the original draws it.
				if (cmdicon_tbl_ok && idx < 16) { row[px] = 0xff000000u | cmdicon_tbl[0][idx]; continue; }
				// (no table -- a mod's ticon.pcx: the old luminance-shaded gold)
				uint32_t rgb = cmdicon_rgb[idx];
				int lum = (((rgb >> 16) & 0xff) * 77 + ((rgb >> 8) & 0xff) * 150 + (rgb & 0xff) * 29) >> 8;
				int rr = 0xff * lum / 255, gg = 0xcc * lum / 255, bb = 0x30 * lum / 255;
				row[px] = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
			}
		}
	}

	// Draw a command icon SCALED to fill `cell` (aspect-preserving, ~12% margin)
	// and centered on the icon's own content. The legacy console card cells are
	// much larger than the native ~36x34 cmdicons frame, so draw_cmdicon()'s
	// native-size, canvas-anchored blit looks tiny and pushed to one side. This
	// variant fixes both: it fills the button and centers by the frame content
	// (dropping the canvas offset). HD/user override icons are scaled the same.
	// grey=true drops the fixed gold and draws the icon in neutral grey. The
	// command card is gold in BW, but the Equipment display's icons are not --
	// they read as etched metal there, so the same frames need both looks.
	// margin_pct: the command card wants breathing room around its buttons; the
	// Equipment slots want the icon out to its own border, so they pass 0.
	void draw_cmdicon_fit(uint32_t* data, size_t pitch, int frame_index, rect cell,
	                      bool grey = false, int margin_pct = 12, uint32_t tint = 0) {
		// tint != 0: recolor the icon as luminance-shaded line-art in that RGB
		// (the 3x3 command card uses bright yellow). Overrides the default gold.
		int tr = (tint >> 16) & 0xff, tg = (tint >> 8) & 0xff, tb = tint & 0xff;
		int cw = cell.to.x - cell.from.x, ch = cell.to.y - cell.from.y;
		if (cw < 6 || ch < 6) return;
		int margin = (cw < ch ? cw : ch) * margin_pct / 100;
		int bx0 = cell.from.x + margin, by0 = cell.from.y + margin;
		int bw = cw - 2 * margin, bh = ch - 2 * margin;
		if (bw < 4 || bh < 4) return;
		// Aspect-fit a sw x sh source into the bw x bh box.
		auto fit = [&](int sw, int sh, int& dw, int& dh, int& dx0, int& dy0) {
			if (sw <= 0 || sh <= 0) { dw = dh = 0; return; }
			dw = bw; dh = (int)((long long)bw * sh / sw);
			if (dh > bh) { dh = bh; dw = (int)((long long)bh * sw / sh); }
			dx0 = bx0 + (bw - dw) / 2; dy0 = by0 + (bh - dh) / 2;
		};
		// HD / user icon override (pre-scaled ARGB) — nearest-scale with alpha.
		if (ddsgrp::frame_t* ov = get_icon_override(frame_index)) {
			int dw, dh, dx0, dy0; fit(ov->w, ov->h, dw, dh, dx0, dy0);
			if (dw <= 0 || dh <= 0) return;
			for (int y = 0; y != dh; ++y) {
				int py = dy0 + y;
				if (py < 0 || py >= (int)screen_height) continue;
				uint32_t* row = data + (size_t)py * pitch;
				const uint32_t* srow = &ov->rgba[(size_t)(y * ov->h / dh) * ov->w];
				for (int x = 0; x != dw; ++x) {
					int px = dx0 + x;
					if (px < 0 || px >= (int)screen_width) continue;
					uint32_t c = srow[x * ov->w / dw];
					uint32_t a = c >> 24;
					if (a < 8) continue;
					if (grey) {
						// HD icon packs are gold too, so the Equipment display
						// has to desaturate them here as well -- doing it only
						// on the paletted path left the slots gold whenever an
						// HD mod was active, which is the usual case.
						uint32_t l = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
						c = (c & 0xff000000u) | (l << 16) | (l << 8) | l;
					} else if (tint) {
						// Bright-yellow line-art (3x3 card): full-colour HD icons
						// otherwise read as muted/grey against the dark cell. Lift the
						// luminance (floor+range) so even dark HD-render pixels read as
						// VIVID yellow, not muddy brown -- the user wants bright yellow.
						uint32_t l = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
						l = 80 + l * 175 / 255;
						c = (c & 0xff000000u) | ((uint32_t)(tr * l / 255) << 16) | ((uint32_t)(tg * l / 255) << 8) | (uint32_t)(tb * l / 255);
					}
					if (a >= 248) { row[px] = 0xff000000u | (c & 0xffffff); continue; }
					uint32_t old = row[px], ia = 255 - a;
					uint32_t r = (((c >> 16) & 0xff) * a + ((old >> 16) & 0xff) * ia) / 255;
					uint32_t g = (((c >> 8) & 0xff) * a + ((old >> 8) & 0xff) * ia) / 255;
					uint32_t b = ((c & 0xff) * a + (old & 0xff) * ia) / 255;
					row[px] = 0xff000000u | (r << 16) | (g << 8) | b;
				}
			}
			return;
		}
		if (!cmdicons_ok || frame_index < 0 || frame_index >= (int)cmdicons_grp.frames.size()) return;
		auto& f = cmdicons_grp.frames[frame_index];
		size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return;
		cmdicon_scratch.assign(fw * fh, 0);
		draw_frame(f, false, cmdicon_scratch.data(), fw, 0, 0, fw, fh);
		int dw, dh, dx0, dy0; fit((int)fw, (int)fh, dw, dh, dx0, dy0);
		if (dw <= 0 || dh <= 0) return;
		for (int y = 0; y != dh; ++y) {
			int py = dy0 + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = cmdicon_scratch.data() + (size_t)(y * (int)fh / dh) * fw;
			for (int x = 0; x != dw; ++x) {
				uint8_t idx = srow[x * (int)fw / dw];
				if (idx == 0) continue;
				int px = dx0 + x;
				if (px < 0 || px >= (int)screen_width) continue;
				if (cmdicon_tbl_ok && idx < 16) {
					// The real colour tables: yellow (row 0) for commands, white-grey
					// (row 5) for the Equipment display. A tint keeps the table's
					// shading -- brightest column = the full tint, the outline stays
					// dark -- instead of lifting every pixel to a bright floor.
					uint32_t c = cmdicon_tbl[grey ? 5 : 0][idx];
					if (tint && !grey) {
						const uint32_t top = cmdicon_tbl[0][1];
						int ltop = (((top >> 16) & 0xff) * 77 + ((top >> 8) & 0xff) * 150 + (top & 0xff) * 29) >> 8; if (ltop < 1) ltop = 1;
						int l = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
						if (l > ltop) l = ltop;
						c = ((uint32_t)(tr * l / ltop) << 16) | ((uint32_t)(tg * l / ltop) << 8) | (uint32_t)(tb * l / ltop);
					}
					row[px] = 0xff000000u | c;
					continue;
				}
				// Same fixed-gold, luminance-shaded look as draw_cmdicon().
				uint32_t rgb = cmdicon_rgb[idx];
				int lum = (((rgb >> 16) & 0xff) * 77 + ((rgb >> 8) & 0xff) * 150 + (rgb & 0xff) * 29) >> 8;
				int rr, gg, bb;
				if (grey) { rr = gg = bb = 0x40 + lum * 0xbf / 255; }
				else if (tint) { int lv = 80 + lum * 175 / 255; rr = tr * lv / 255; gg = tg * lv / 255; bb = tb * lv / 255; }
				else { rr = 0xff * lum / 255; gg = 0xcc * lum / 255; bb = 0x30 * lum / 255; }
				row[px] = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
			}
		}
	}

	// A one-pixel rectangular outline. The HUD v2 command-card cells and the
	// MENU button are both drawn with this look, so anything that must "match
	// the cards" uses it rather than inventing a second border style.
	void draw_border_rgba(uint32_t* data, size_t pitch, rect r, uint32_t color, int thickness = 1) {
		for (int t = 0; t != thickness; ++t) {
			int x0 = r.from.x + t, y0 = r.from.y + t, x1 = r.to.x - 1 - t, y1 = r.to.y - 1 - t;
			if (x1 <= x0 || y1 <= y0) return;
			for (int x = x0; x <= x1; ++x) {
				if (x < 0 || x >= (int)screen_width) continue;
				if (y0 >= 0 && y0 < (int)screen_height) data[(size_t)y0 * pitch + x] = color;
				if (y1 >= 0 && y1 < (int)screen_height) data[(size_t)y1 * pitch + x] = color;
			}
			for (int y = y0; y <= y1; ++y) {
				if (y < 0 || y >= (int)screen_height) continue;
				uint32_t* row = data + (size_t)y * pitch;
				if (x0 >= 0 && x0 < (int)screen_width) row[x0] = color;
				if (x1 >= 0 && x1 < (int)screen_width) row[x1] = color;
			}
		}
	}

	// open1161HD task 4: whole-HUD colour identity keyed on the LOCAL player's
	// race (distinct from race_accent, which follows the SELECTED unit). Terran
	// keeps the classic grey-blue console exactly (default values below == the
	// literals the chrome used before), Protoss gets gold frames over a cool blue
	// field, Zerg gets violet-neon frames over a dark violet field. Pure render;
	// no determinism impact. Every field defaults to the Terran look so any call
	// site that themes only some elements still reads as one console.
	struct hud_theme_t {
		uint32_t border;    // panel outer highlight ("apvadai")
		uint32_t inner;     // panel inner shadow line
		uint32_t backdrop;  // full-width bottom console fill (ARGB, has alpha)
		uint32_t edge;      // the 1px lit top edge of the backdrop
		uint32_t accent;    // labels/toggles highlight (MENU, group #, QUEUE-on)
		uint32_t on_fill;   // active-toggle fill (e.g. QUEUE on)
		uint32_t on_line;   // active-toggle border (draw_queue_button style)
		uint32_t on_text;   // active-toggle text
		uint32_t grp_border;    // filled control-group button border (own literal)
		uint32_t strip_on_line; // QUEUE right-edge strip active border (own literal)
		// v0.123 "HUD v2 - race colours": everything below used to be a literal in
		// the draw functions, the same for every race. Defaults = those literals,
		// so the legacy HUD (which keeps the old ten fields too) is unchanged.
		uint32_t amber = 0xffe8c828u;          // accent text, level digits, focus cell, armed border
		uint32_t panel_fill = 0xff14181au;     // hud2_frame body
		uint32_t portrait_fill = 0xff0a0e10u;
		uint32_t btn_fill = 0xff20282cu;       // MENU, latency, card cell, production / cargo slot
		uint32_t btn_line = 0xff4a5458u;
		uint32_t btn_pressed = 0xff20282cu;    // a finger is on it (new state; legacy: none)
		uint32_t off_fill = 0xa014181au;       // toggle off / empty group
		uint32_t off_line = 0xff2c3438u;
		uint32_t grp_fill = 0xf0203848u;       // filled control group
		uint32_t util_fill = 0xc01c242cu;      // IDLE / ARMY and the other small buttons
		uint32_t util_line = 0xff506070u;
		uint32_t util_line_zero = 0xff384450u;
		uint32_t dim_rgb = 0x101418u;          // unavailable card button: blend colour
		uint32_t dim_line = 0xff503838u;
		uint32_t armed_fill = 0xff3a5a2cu;     // waiting for a target
		uint32_t armed_line = 0xffe8c828u;
		uint32_t ping_fill = 0xff702020u, ping_armed_fill = 0xffb02828u, ping_edge = 0xffe07070u;
		uint32_t icon_tint = 0x00ffe020u;      // command card icons
		uint32_t slot_free_line = 0xff3a4248u; // empty production queue slot
		uint32_t tip_fill = 0xf00e1214u, tip_line = 0xff5a6468u;
		uint32_t menu_btn_fill = 0xf01e2c3cu, menu_btn_line = 0xff5878a0u, menu_title = 0xffffe090u;
	};
	// A finger (or the pen) is down inside r: HUD v2 buttons show it. The press
	// point is recorded by the event loop; never in the legacy HUD.
	xy hud_press_pos{-1, -1};
	bool hud_pressed(const rect& r) const {
		return !ui_legacy && hud_press_pos.x >= r.from.x && hud_press_pos.x < r.to.x && hud_press_pos.y >= r.from.y && hud_press_pos.y < r.to.y;
	}
	// HUD v2 race colours (2026-10-02): the structure of HUD v2 is untouched, only colour values change.
	// One palette per race of the LOCAL player; state colours (HP, shields, energy,
	// resources, latency, production accent) are not part of the theme and stay the
	// same for all three. Accent text and command icons are the launcher amber for
	// every race, so a race colour never collides with a state colour.
	hud_theme_t hud_theme() const {
		if (ui_legacy) return hud_theme_legacy();
		race_t r = (game_local_player >= 0 && game_local_player < 12)
			? st.players[game_local_player].race : race_t::terran;
		hud_theme_t t{};
		t.amber = t.accent = t.armed_line = t.menu_title = 0xfff2c53du;
		t.icon_tint = 0x00f2c53du;
		t.armed_fill = 0xff3d3410u;
		t.dim_line = 0xff5a1717u;
		t.ping_fill = 0xff5a1717u; t.ping_armed_fill = 0xffb3261eu; t.ping_edge = 0xffe0564eu;
		t.portrait_fill = 0xff05070au;
		switch (r) {
		case race_t::protoss:   // gold frames over navy, blue toggles
			t.border = t.grp_border = t.tip_line = t.menu_btn_line = 0xffc9a227u;
			t.inner = 0xff1c3a5eu; t.backdrop = 0xf0080d16u; t.edge = 0xff2f5a80u;
			t.on_fill = 0xf013294au; t.on_line = t.strip_on_line = 0xff5aa0f0u; t.on_text = 0xffd6e8ffu;
			t.panel_fill = 0xff0a0f18u; t.off_fill = 0xa00a0f18u; t.dim_rgb = 0x0a0f18u;
			t.btn_fill = 0xff141c2au; t.util_fill = 0xc0141c2au; t.menu_btn_fill = 0xf0141c2au;
			t.btn_line = 0xff3f5270u; t.btn_pressed = 0xff22304au;
			t.off_line = t.util_line_zero = t.slot_free_line = 0xff26344cu;
			t.grp_fill = 0xf014233au; t.util_line = 0xff4a5e7cu;
			t.tip_fill = 0xf0080d16u;
			break;
		case race_t::zerg:      // violet frames over dark violet, violet toggles
			t.border = t.grp_border = t.tip_line = t.menu_btn_line = 0xffa44fd8u;
			t.inner = 0xff4a1436u; t.backdrop = 0xf00e0814u; t.edge = 0xff6a2a86u;
			t.on_fill = 0xf02a1040u; t.on_line = t.strip_on_line = 0xffb46cf0u; t.on_text = 0xfff2f2f2u;   // (no violet text: it reads as energy)
			t.panel_fill = 0xff100a16u; t.off_fill = 0xa0100a16u; t.dim_rgb = 0x100a16u;
			t.btn_fill = 0xff1c1426u; t.util_fill = 0xc01c1426u; t.menu_btn_fill = 0xf01c1426u;
			t.btn_line = 0xff5a4472u; t.btn_pressed = 0xff30223eu;
			t.off_line = t.util_line_zero = t.slot_free_line = 0xff36274au;
			t.grp_fill = 0xf024163au; t.util_line = 0xff664e80u;
			t.tip_fill = 0xf00e0814u;
			break;
		default:                // Terran: steel frames, the launcher's red line, green toggles
			t.border = t.tip_line = t.menu_btn_line = 0xff6b7880u; t.grp_border = 0xff5a8098u;
			t.inner = 0xff1f272cu; t.backdrop = 0xf00a0e11u; t.edge = 0xff7a2020u;
			t.on_fill = 0xf0123a1fu; t.on_line = t.strip_on_line = 0xff4fd46au; t.on_text = 0xffd6f5ddu;
			t.panel_fill = 0xff0b0f12u; t.off_fill = 0xa00b0f12u; t.dim_rgb = 0x0b0f12u;
			t.btn_fill = 0xff161d22u; t.util_fill = 0xc0161d22u; t.menu_btn_fill = 0xf0161d22u;
			t.btn_line = 0xff46525au; t.btn_pressed = 0xff26323au;
			t.off_line = t.util_line_zero = t.slot_free_line = 0xff2b3338u;
			t.grp_fill = 0xf016252eu; t.util_line = 0xff56646eu;
			t.tip_fill = 0xf00a0e11u;
			break;
		}
		return t;
	}
	// The legacy HUD's theme: exactly what it was (ten race fields + the literals).
	hud_theme_t hud_theme_legacy() const {
		race_t r = (game_local_player >= 0 && game_local_player < 12)
			? st.players[game_local_player].race : race_t::terran;
		switch (r) {
		case race_t::protoss: return {
			0xffe8c040u, 0xff5a4a1cu, 0xf00e1622u, 0xff2f5a80u,
			0xffe8c848u, 0xf0203a60u, 0xff50a0ffu, 0xffbfe0ffu,
			0xffe8c040u, 0xff50a0ffu };
		case race_t::zerg: return {
			0xffb84cf0u, 0xff3a1c4au, 0xf0140a1cu, 0xff6a2a86u,
			0xffcc70ffu, 0xf0301050u, 0xffb060ffu, 0xffe6c8ffu,
			0xffb84cf0u, 0xffb060ffu };
		default: return { // Terran -- classic console (unchanged; last two = the
			// ORIGINAL group-button + QUEUE-strip greens, kept byte-identical)
			0xff5a6468u, 0xff2a3236u, 0xf0141a1eu, 0xff3a464eu,
			0xffe8c828u, 0xf0206040u, 0xff60ff90u, 0xff90ffb0u,
			0xff4a6a80u, 0xff40a070u };
		}
	}

	// Race accent used by the Equipment slots and the production strip so both
	// read as belonging to the selected unit's race.
	uint32_t race_accent(const unit_t* u) const {
		if (!u || !u->unit_type) return 0xffb0b8c0u;
		switch (unit_race(u)) {
		case race_t::zerg: return 0xffe08828u;      // orange
		case race_t::protoss: return 0xff40d0e8u;   // cyan
		default: return 0xffb0b8c0u;                // Terran: grey
		}
	}

	// TEMP DEBUG: render a grid of cmdicons frames [base, base+count) with index
	// labels, to identify command-icon frame numbers. Remove once mapped.
	void draw_cmdicon_debug(uint32_t* data, size_t pitch, int base, int count) {
		load_cmdicons();
		if (!cmdicons_ok) return;
		int cols = 12, cell = 60;
		for (int i = 0; i < count; ++i) {
			int fi = base + i;
			if (fi >= (int)cmdicons_grp.frames.size()) break;
			int col = i % cols, row = i / cols;
			int cx = 40 + col * cell + cell / 2;
			int cy = 40 + row * cell + cell / 2;
			draw_cmdicon(data, pitch, fi, cx, cy);
			char lbl[8];
			snprintf(lbl, sizeof(lbl), "%d", fi);
			draw_text_rgba(data, pitch, cx - 12, cy + 20, lbl, 0xff00ffff);
		}
	}

	// --- Command-button chrome (v0.8): tcmdbtns.grp is the authentic 3x3
	// command-card button background. Frame 0 is the normal button; it shares
	// the ticon.pcx palette (cmdicon_rgb) with the icons. Scaled to each cell.
	grp_t cmdbtns_grp;
	a_vector<uint8_t> cmdbtns_scratch;
	bool cmdbtns_tried = false, cmdbtns_ok = false;

	void load_cmdbtns() {
		if (cmdbtns_tried) return;
		cmdbtns_tried = true;
		if (!load_data_file) return;
		load_cmdicons(); // shares the ticon.pcx palette in cmdicon_rgb
		try {
			a_vector<uint8_t> buf;
			load_data_file(buf, "unit\\cmdbtns\\tcmdbtns.grp");
			if (buf.empty()) return;
			cmdbtns_grp = read_grp_tolerant(buf.data(), buf.size());
			cmdbtns_ok = !cmdbtns_grp.frames.empty()
				&& !cmdbtns_grp.frames[0].data_container.empty();
		} catch (...) {
		}
	}

	// Nearest-neighbor blit of a decoded GRP frame (given palette, index 0
	// transparent) scaled to fill the screen rect r.
	void blit_grp_scaled(uint32_t* data, size_t pitch, grp_t& grp, a_vector<uint8_t>& scratch,
	                     const uint32_t* pal, int frame_index, rect r) {
		if (frame_index < 0 || frame_index >= (int)grp.frames.size()) return;
		auto& f = grp.frames[frame_index];
		int fw = (int)f.size.x, fh = (int)f.size.y;
		if (fw <= 0 || fh <= 0 || f.data_container.empty()) return;
		scratch.assign((size_t)fw * fh, 0);
		draw_frame(f, false, scratch.data(), fw, 0, 0, fw, fh);
		int dw = r.to.x - r.from.x, dh = r.to.y - r.from.y;
		if (dw <= 0 || dh <= 0) return;
		for (int dy = 0; dy < dh; ++dy) {
			int py = r.from.y + dy;
			if (py < 0 || py >= (int)screen_height) continue;
			int sy = dy * fh / dh;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* srow = scratch.data() + (size_t)sy * fw;
			for (int dx = 0; dx < dw; ++dx) {
				int px = r.from.x + dx;
				if (px < 0 || px >= (int)screen_width) continue;
				uint8_t idx = srow[dx * fw / dw];
				if (idx == 0) continue;
				row[px] = 0xff000000u | pal[idx];
			}
		}
	}

	// --- Authentic console frame (v0.7): game\tconsole.pcx is the 640x480 BW
	// UI overlay (bottom console bar + viewport border). Index 0 is the
	// transparent play area. We scale it to the screen and blit it over the
	// game, so the panels below sit within the real console art.
	pcx_image console_pcx;
	uint32_t console_pal[256] = {};
	bool console_tried = false;
	bool console_ok = false;

	void load_console() {
		if (console_tried) return;
		console_tried = true;
		if (!load_data_file) return;
		try {
			a_vector<uint8_t> buf;
			load_data_file(buf, "game\\tconsole.pcx");
			if (buf.empty()) return;
			console_pcx = load_pcx_data(buf);
			if (buf.size() >= 769 && buf[buf.size() - 769] == 0x0c) {
				const uint8_t* pal = buf.data() + buf.size() - 768;
				for (int i = 0; i != 256; ++i) {
					console_pal[i] = ((uint32_t)pal[i * 3 + 0] << 16)
						| ((uint32_t)pal[i * 3 + 1] << 8) | (uint32_t)pal[i * 3 + 2];
				}
				console_ok = console_pcx.width > 0 && console_pcx.height > 0;
			}
		} catch (...) {
		}
	}

	void draw_console(uint32_t* data, size_t pitch) {
		load_console();
		if (!console_ok) return;
		int cw = (int)console_pcx.width, ch = (int)console_pcx.height;
		const uint8_t* src = console_pcx.data.data();
		float s = hud_scale();
		int ox = hud_off_x(), oy = hud_off_y();
		int x_end = ox + (int)(cw * s);
		for (int sy = oy < 0 ? 0 : oy; sy < (int)screen_height; ++sy) {
			int cy = (int)((sy - oy) / s);
			if (cy < 0 || cy >= ch) continue;
			uint32_t* row = data + (size_t)sy * pitch;
			const uint8_t* srow = src + (size_t)cy * cw;
			int sx0 = ox < 0 ? 0 : ox;
			int sx1 = x_end > (int)screen_width ? (int)screen_width : x_end;
			for (int sx = sx0; sx < sx1; ++sx) {
				int cx = (int)((sx - ox) / s);
				if (cx < 0 || cx >= cw) continue;
				uint8_t idx = srow[(size_t)cx];
				if (idx == 0) continue;   // transparent play area
				row[sx] = 0xff000000u | console_pal[idx];
			}
		}
	}

	// Fog of war: darken/black the world where the local player has no vision.
	// The engine already tracks per-tile per-player visible/explored masks
	// (tile.visible/tile.explored); we only had to render them. Runs on the
	// RGBA world (after the indexed->rgba blit, before the console HUD draws on
	// top), over the visible playfield only. Melee only (game_input + a local
	// player); replays keep full vision.
	// In a replay the viewer can watch through a chosen player's fog of war
	// (-1 = full/observer vision, the default).
	int replay_vision_player = -1;

	// Cycle replay vision: observer (-1) -> each player that has units -> back.
	void cycle_replay_vision() {
		for (int i = 0; i < 13; ++i) {
			++replay_vision_player;
			if (replay_vision_player >= 12) { replay_vision_player = -1; return; }
			for (unit_t* u : ptr(st.player_units[replay_vision_player])) { (void)u; return; }
		}
		replay_vision_player = -1;
	}

	// Dev switch (v0.64 item 16). This only skips DRAWING the fog overlay -- the
	// sim's own vision state is untouched, so sight range, detection and
	// determinism all still behave normally and this stays safe in a LAN game.
	// It does not reveal units the player has no vision of, only terrain.
	bool no_fog = false;

	// v0.80 item 11: the handful of strings we author in-game (the launcher has
	// its own table in S.java). English is the source and the default; Lithuanian
	// is the translation, so an unlisted string simply stays English.
	bool lang_lt = false;
	const char* L(const char* en) const {
		if (!lang_lt) return en;
		struct pair { const char* en; const char* lt; };
		static const pair tbl[] = {
			{"MISSION BRIEFING", "MISIJOS INSTRUKTAZAS"},
			{"This map uses StarCraft: Remastered terrain. Set the SC:R files in Settings.", "Siam zemelapiui reikia StarCraft: Remastered reljefo. Nurodyk SC:R failus Nustatymuose."},
			{"Planned building dropped: the spot is blocked.", "Suplanuotas pastatas atsauktas: vieta uzstota."},
			{"OBJECTIVES", "TIKSLAI"},
			{"START MISSION", "PRADETI MISIJA"},
			{"SKIP", "PRALEISTI"},
			{"VICTORY!", "PERGALE!"},
			{"DEFEAT!", "PRALAIMEJIMAS"},
			{"Group assigned", "Grupe priskirta"},
			{"Defeat...", "Pralaimejimas..."},
			{"MISSION OBJECTIVES", "MISIJOS TIKSLAI"},
			{"Not enough room for unit.", "Nera vietos unitui."},
			{"Cannot create more units.", "Negalima sukurti daugiau unitu."},
			// v0.81.003: the disconnect box used to be a bare Lithuanian
			// literal with the diacritics stripped, bypassing L() entirely.
			{"The opponent disconnected or sync was lost.\nReturning to the menu.",
			 "Priesininkas atsijunge arba prarasta sinchronizacija.\nGriztama i meniu."},
			{"Opponent left. The computer has taken over their forces.",
			 "Priesininkas isejo. Jo pajegas perema kompiuteris."},
			{"PAUSED", "PAUZE"},
			{"Return to game", "Grizti i zaidima"},
			{"Save game", "Issaugoti"},
			{"Load game", "Ikelti"},
			{"Help", "Pagalba"},
			{"End Mission", "Baigti misija"},
			{"CONTROLS", "VALDYMAS"},
			{"Back", "Atgal"},
			{"Yes, leave", "Taip, iseiti"},
			{"No, keep playing", "Ne, zaisti toliau"},
			{"Leave the game?", "Palikti zaidima?"},
			{"End the mission?", "Baigti misija?"},
			// Loading screen (ui/loading_screen.h). ASCII only: the Unispace atlas
			// has no Lithuanian diacritics.
			{"FREE FOR ALL", "VISI PRIES VISUS"},
			{"TEAM", "KOMANDA"},
			{"Random", "Atsitiktine"},
			{"CANCEL", "ATSAUKTI"},
			{"Player", "Zaidejas"},
			{"Computer", "Kompiuteris"},
			{"Unknown map", "Nezinomas zemelapis"},
			{"EPISODE", "EPIZODAS"},
			{"MISSION", "MISIJA"},
			{"CAMPAIGN", "KAMPANIJA"},
			{"TUTORIAL", "MOKYMAI"},
			{"SECRET MISSION", "SLAPTA MISIJA"},
			{"Very easy", "Labai lengvas"},
			{"Easy", "Lengvas"},
			{"Medium", "Vidutinis"},
			{"Hard", "Sunkus"},
			{"Bot", "Botas"},
			// Replay player (same ASCII-only rule).
			{"PLAYERS", "ZAIDEJAI"},
			{"VISION", "MATYMAS"},
			{"ARMY", "ARMIJA"},
			{"UPGRADES", "UPGRADE'AI"},
			{"BUILDINGS", "PASTATAI"},
			{"Tap a player", "Bakstelekite zaideja"},
			{"Nothing yet", "Dar nieko"},
			{"REPLACE", "PAKEISTI"},
			{"CLEAR", "ISVALYTI"},
			{"+ ADD", "+ PRIDETI"},
			{"ECONOMY", "EKONOMIKA"},
			{"INCOME / MIN", "PAJAMOS / MIN"},
			{"ARMY VALUE", "ARMIJOS VERTE"},
			{"WORKERS", "DARBININKAI"},
		};
		for (auto& e : tbl) if (!strcmp(e.en, en)) return e.lt;
		return en;
	}

	// The single place that decides whose vision the presentation uses, and
	// whether fog applies at all. Returns -1 to mean "reveal everything".
	//
	// Multiplayer ALWAYS fogs, whatever the dev toggle says: a peer who could
	// switch it off would simply be cheating. The clamp lives here rather than
	// where no_fog is assigned because is_net_game is set later during startup,
	// so a clamp at assignment time would read a stale false.
	int fog_vision_owner() const {
		if (no_fog && !is_net_game) return -1;
		return game_input ? game_local_player : replay_vision_player;
	}

	void draw_fog(uint32_t* data, size_t pitch) {
		int owner = fog_vision_owner();
		if (owner < 0) return;
		int pfb = playfield_bottom_y();
		if (pfb > (int)screen_height) pfb = (int)screen_height;
		int ws = world_scale < 1 ? 1 : world_scale;
		int ts = 32 * ws; // buffer px per map tile (Faze 4.3: fog scales with world)
		auto b = screen_tile_bounds();
		for (size_t ty = b.from.y; ty < b.to.y; ++ty) {
			for (size_t tx = b.from.x; tx < b.to.x; ++tx) {
				xy pos((int)(tx * 32 + 16), (int)(ty * 32 + 16));
				if (player_position_is_visible(owner, pos)) continue;
				bool expl = player_position_is_explored(owner, pos);
				int sx = ((int)(tx * 32) - screen_pos.x) * ws;
				int sy = ((int)(ty * 32) - screen_pos.y) * ws;
				// dim42[i] == i*42/100, precomputed once (bit-exact to the per-pixel
				// divides this replaced -- 3 integer /100 per fogged pixel was a top
				// per-frame cost). Also clip the horizontal span once per row instead
				// of a per-pixel bounds branch. Same pixels, same values.
				static uint8_t dim42[256];
				static bool dim42_ready = false;
				if (!dim42_ready) { for (int i = 0; i < 256; ++i) dim42[i] = (uint8_t)((i * 42) / 100); dim42_ready = true; }
				int px0 = sx < 0 ? 0 : sx;
				int px1 = sx + ts; if (px1 > (int)screen_width) px1 = (int)screen_width;
				for (int y = 0; y < ts; ++y) {
					int py = sy + y;
					if (py < 0 || py >= pfb) continue;
					uint32_t* row = data + (size_t)py * pitch;
					if (!expl) {
						for (int px = px0; px < px1; ++px) row[px] = 0xff000000u; // never seen: black
					} else {
						for (int px = px0; px < px1; ++px) {
							uint32_t c = row[px]; // explored, no vision: dim
							uint32_t r = dim42[(c >> 16) & 0xff];
							uint32_t g = dim42[(c >> 8) & 0xff];
							uint32_t bl = dim42[c & 0xff];
							row[px] = 0xff000000u | (r << 16) | (g << 8) | bl;
						}
					}
				}
			}
		}
	}

	// --- race corner motifs ("tabs") on HUD v2 panels (v0.125) -------
	// A small mark in a panel's top-left corner, inside the frame, in a margin that
	// is already empty: Terran a steel plate with a slanted end and amber hazard
	// stripes, Zerg a spine with three hanging claws, Protoss a gold bar with a blue
	// gem. Three sizes (design units = px at the 924-high tier, scaled like the HUD):
	//   0 full  58 x 6  unit panel, card, tooltips, dialogs
	//   1 short 26 x 6  MENU
	//   2 slim  26 x 3  minimap and portrait frames (inside their 3 px border)
	// Rectangles and triangles only; nothing here is touchable or covers content.
	void hud_px(uint32_t* data, size_t pitch, int x, int y, uint32_t c) {
		if (x >= 0 && y >= 0 && x < (int)screen_width && y < (int)screen_height) data[(size_t)y * pitch + x] = 0xff000000u | (c & 0xffffffu);
	}
	void draw_hud_tab(uint32_t* data, size_t pitch, rect r, int kind) {
		if (ui_legacy) return;
		const hud_theme_t th = hud_theme();
		const int w = hud_h_px(kind == 0 ? 58 : 26), h = hud_h_px(kind == 2 ? 3 : 6);
		const int in = hud2_line_h();                      // just inside the outer line
		const int x0 = r.from.x + in, y0 = r.from.y + in;
		if (r.to.x - r.from.x < w + 4 * in || r.to.y - r.from.y < h + 2 * in) return;
		const race_t race = (game_local_player >= 0 && game_local_player < 12) ? st.players[game_local_player].race : race_t::terran;
		if (race == race_t::zerg) {
			// spine: a bar that tapers to a point, with three claws hanging from it
			const int bar = h >= hud_h_px(6) ? h / 2 : (h * 2 + 2) / 3;
			for (int y = 0; y < bar; ++y) {
				const int len = w - (bar - 1 - y) * 2 - (w / 6) * y / (bar > 1 ? bar - 1 : 1);
				for (int x = 0; x < len; ++x) hud_px(data, pitch, x0 + x, y0 + y, th.border);
			}
			for (int x = 0; x < w * 3 / 4; ++x) hud_px(data, pitch, x0 + x, y0 + bar, th.inner);
			const int claw_h = h - bar, claw_w = claw_h * 2 + 1;
			for (int k = 0; k < 3; ++k) {
				const int cx = x0 + w * (2 + k * 5) / 18;
				for (int y = 0; y < claw_h; ++y) {
					const int half = (claw_w - 1) / 2 - y;          // narrowing downwards, leaning right
					for (int x = -half; x <= half; ++x) hud_px(data, pitch, cx + x + y, y0 + bar + y, th.border);
				}
			}
		} else if (race == race_t::protoss) {
			// gold bar tapering to the right, a blue gem set into its left part
			const int bar = h >= hud_h_px(6) ? h / 2 : h;
			for (int y = 0; y < bar; ++y) {
				const int len = w - y * 3;
				for (int x = 0; x < len; ++x) hud_px(data, pitch, x0 + x, y0 + y, th.border);
			}
			const int gr = h >= hud_h_px(6) ? h / 2 : (h + 1) / 2;         // gem half size
			const int gx = x0 + w / 4, gy = y0 + (h >= hud_h_px(6) ? h / 2 : h / 2);
			for (int y = -gr; y <= gr; ++y) for (int x = -(gr - std::abs(y)) * 2; x <= (gr - std::abs(y)) * 2; ++x) {
				const bool edge = std::abs(x) >= (gr - std::abs(y)) * 2 - 1 || std::abs(y) == gr;
				if (gy + y < y0) continue;
				hud_px(data, pitch, gx + x, gy + y, edge ? th.inner : (y < 0 ? 0xd6e8ffu : th.strip_on_line));
			}
		} else {
			// steel plate with a slanted end and amber hazard stripes
			const int stripe = h >= hud_h_px(6) ? hud_h_px(3) : hud_h_px(2);
			for (int y = 0; y < h; ++y) {
				const int len = w - y;                               // slant: one px per row
				for (int x = 0; x < len; ++x) {
					uint32_t c = th.border;
					const int sx = x - hud_h_px(4) + y;               // stripes lean with the slant
					if (x >= hud_h_px(4) && x < len - hud_h_px(5) && y >= in && y < h - in && sx >= 0 && (sx / stripe) % 2 == 0) c = th.amber;
					else if (x >= hud_h_px(4) && x < len - hud_h_px(5) && y >= in && y < h - in) c = 0x161d22u;
					hud_px(data, pitch, x0 + x, y0 + y, c);
				}
			}
		}
	}
	// Dark-metal square frame (HUD v2). fill = panel interior, two-tone border.
	void hud2_frame(uint32_t* data, size_t pitch, rect r, uint32_t fill = 0) {
		if (r.to.x <= r.from.x || r.to.y <= r.from.y) return;
		hud_theme_t th = hud_theme();
		if (!fill) fill = th.panel_fill;
		fill_rect_rgba(data, pitch, r, fill);
		line_rectangle_rgba(data, pitch, r, th.border);                                  // outer highlight (race-themed)
		line_rectangle_rgba(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.to.x - 1, r.to.y - 1}}, th.inner); // inner shadow
	}

	// HUD v2 native layout. Dark-metal panels anchored to the screen edges; the
	// world renders down to the top of this bar. Colors match the game console
	// (dark metal, gray symbols, yellow button text). Iterating toward parity
	// with the legacy HUD; input hit-tests reuse the same *_area()/panel funcs.
	void draw_hud2(uint32_t* data, size_t pitch) {
		// Faze 4.3: scale in-game HUD text to the FULL HD buffer for the duration of
		// this call (reset on every return path). No-op at SD/HD2.
		hud_scale_text = true;
		diag_in_hud = true; // narrow flag: only real HUD text is in HUD_TEXT_CLIP's scope
		struct hud_text_guard_t { bool* f; bool* g; ~hud_text_guard_t() { *f = false; *g = false; } }
			_hud_text_guard{&hud_scale_text, &diag_in_hud};
		int p = game_local_player;
		// v0.64 item 10: one continuous backdrop spanning the full width behind
		// the bottom bar, so the panels read as parts of a console instead of
		// four cards floating over the map. Its top edge is the Unit Display's
		// top edge, which is the lowest of the bar's panels.
		{
			int top = hud2_bar_top();
			rect band = {{0, top}, {(int)screen_width, (int)screen_height}};
			hud_theme_t th = hud_theme();
			fill_rect_rgba(data, pitch, band, th.backdrop);
			// the accent line: unbroken, full width, one HUD unit thick
			fill_rect_rgba(data, pitch, {{0, top}, {(int)screen_width, top + hud2_line_h()}}, 0xff000000u | (th.edge & 0xffffffu));
		}
		// --- bottom bar panels ---
		hud2_frame(data, pitch, hud2_minimap_panel());
		hud2_frame(data, pitch, hud2_unit_panel());
		hud2_frame(data, pitch, hud2_card_panel());
		draw_hud_tab(data, pitch, hud2_unit_panel(), 0);
		draw_hud_tab(data, pitch, hud2_card_panel(), 0);
		// v0.82.017: the frame wraps the PORTRAIT, not the whole column. It used
		// to be drawn around hud2_portrait_panel(), which is taller than the
		// square video, so the picture floated inside a box with dead space
		// under it. The latency readout below now gets its own box instead.
		{
			hud2_frame(data, pitch, hud2_portrait_frame(), hud_theme().portrait_fill);
			draw_portrait_video(data, pitch, hud2_portrait_video());
			draw_hud_tab(data, pitch, hud2_portrait_frame(), 2);
		}
		// minimap (get_minimap_area() already points at the hud2 panel)
		draw_minimap_scaled(data, pitch);
		draw_hud_tab(data, pitch, hud2_minimap_panel(), 2);
		draw_ping_marker(data, pitch);

		// --- MENU button (thin, above portrait) ---
		{
			rect r = hud2_menu_panel();
			hud_theme_t th = hud_theme();
			fill_rect_rgba(data, pitch, r, hud_pressed(r) ? th.btn_pressed : th.btn_fill);
			line_rectangle_rgba(data, pitch, r, th.border);
			draw_hud_tab(data, pitch, r, 1);
			const char* label = "MENU";
			int tw = (int)strlen(label) * usp_adv();
			draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - tw / 2, (r.from.y + r.to.y) / 2 - usp_h() / 2, label, th.accent);
		}

		// --- Unit Display content ---
		draw_hud2_unit_display(data, pitch);
		draw_sel_page_tabs(data, pitch); // SCBW2 unlimited_selection: 1/2/3 sub-card selectors
		draw_cargo_slots(data, pitch); // tappable loaded-unit slots (transports)

		// --- command card 3x3 (reuses cmdicons) ---
		draw_hud2_card(data, pitch);

		// --- top overlays ---
		if (hud_show_fps) draw_fps(data, pitch);          // top-left
		draw_game_timer(data, pitch);   // top-center
		if (hud_show_apm) draw_apm(data, pitch);          // top-center, below the timer
		draw_hud2_resources(data, pitch); // top-right
		draw_zoom_buttons(data, pitch); // top-left (legacy positions ok for now)
		draw_util_buttons(data, pitch); // IDLE / ARMY, below the zoom buttons
		// (no top-left QUEUE button in HUD v2 - the right-edge strip is the toggle)

		// --- right-edge vertical strips: Groups + Queue ---
		draw_hud2_side_strips(data, pitch);
		draw_group_menu(data, pitch);
		draw_auto_worker_button(data, pitch); // task 2: auto-worker toggle above QUEUE

		// --- Map ping button: a small beveled console-style square just above the
		// minimap (v0.29 item 6). Armed state brightens it. ---
		{
			rect r = hud2_ping_button();
			uint32_t fill = ping_armed ? hud_theme().ping_armed_fill : hud_theme().ping_fill;
			fill_rect_rgba(data, pitch, r, fill);
			// Bevel: light top/left, dark bottom/right, like the console buttons.
			line_rectangle_rgba(data, pitch, r, (ui_legacy ? 0xff401010u : 0xff2a0b0bu));
			for (int x = r.from.x; x < r.to.x; ++x) { if (x >= 0 && x < (int)screen_width && r.from.y >= 0 && r.from.y < (int)screen_height) data[(size_t)r.from.y * pitch + x] = hud_theme().ping_edge; }
			for (int y = r.from.y; y < r.to.y; ++y) { if (y >= 0 && y < (int)screen_height && r.from.x >= 0 && r.from.x < (int)screen_width) data[(size_t)y * pitch + r.from.x] = hud_theme().ping_edge; }
			// A small dot glyph in the centre so it reads as "ping".
			int cx = (r.from.x + r.to.x) / 2, cy = (r.from.y + r.to.y) / 2, dr = hud_h_px(4);
			for (int y = -dr; y <= dr; ++y) for (int x = -dr; x <= dr; ++x) {
				if (x * x + y * y > dr * dr) continue;
				int px = cx + x, py = cy + y;
				if (px >= 0 && px < (int)screen_width && py >= 0 && py < (int)screen_height) data[(size_t)py * pitch + px] = (ui_legacy ? 0xfff0d040u : hud_theme().amber);
			}
		}

		// --- bottom-left: LAN latency + version ---
		// v0.64 item 24: the version sits in the very bottom-left corner at the
		// small text size, out of the playfield. Draw it UNSCALED (native glyph
		// height): the corner overlay is meant to stay small, and the bottom margin
		// below subtracts the native unispace_h -- with HD text scaling on, the glyph
		// would render taller than that margin and its bottom rows would fall off the
		// buffer (caught by the HUD_TEXT_CLIP detector on S8U at the FULL-HD scale).
		if (!hud_version_img.empty()) {
			draw_hud_version_img(data, pitch);
		} else if (!hud_version.empty()) {
			bool _hs = hud_scale_text; hud_scale_text = false;
			draw_text_rgba(data, pitch, 4, (int)screen_height - ui::unispace_h - 3, hud_version.c_str(), 0xff5a626au);
			hud_scale_text = _hs;
		}

		// hud_overlap (Cat 3b, DIAG): the four bottom HUD panels tile without overlapping by
		// design (minimap | portrait+menu column | command card). If a scaling/layout
		// regression makes any pair intersect, emit HUD_OVERLAP. Rects come from the SAME
		// getters the draw + hit-test paths use, so the check tracks the real layout. All
		// four are distinct parents (no intentional nesting), so any intersection is a bug.
		// Read-only; --diag gated; de-duped per pair. Runs only in HUD v2 (not legacy).
		if (!ui_legacy && bwgame::diag::enabled()) {
			struct { const char* id; rect r; } panels[4] = {
				{ "minimap", get_minimap_area() }, { "card", hud2_card_panel() },
				{ "portrait", hud2_portrait_panel() }, { "menu", hud2_menu_panel() } };
			int pair = 0;
			for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j, ++pair) {
				const rect& a = panels[i].r; const rect& b = panels[j].r;
				int ox0 = std::max(a.from.x, b.from.x), oy0 = std::max(a.from.y, b.from.y);
				int ox1 = std::min(a.to.x, b.to.x), oy1 = std::min(a.to.y, b.to.y);
				if (ox1 > ox0 && oy1 > oy0 && diag_once(13, (uint32_t)pair))
					bwgame::diag::emit((long)st.current_frame, "hud", bwgame::diag::Sev::warn,
						"HUD_OVERLAP", "two HUD panels that should tile now intersect",
						std::string("\"a\":\"") + panels[i].id + "\",\"b\":\"" + panels[j].id +
						"\",\"overlap\":[" + std::to_string(ox0) + "," + std::to_string(oy0) + "," +
						std::to_string(ox1 - ox0) + "," + std::to_string(oy1 - oy0) + "]");
			}
		}

		// v0.64 item 11: LAN latency lives in the empty strip under the portrait,
		// coloured by how bad it is. Single player leaves the strip blank rather
		// than showing a meaningless zero.
		{
			rect s = hud2_latency_strip();
			if (s.to.y > s.from.y + 4) {
				// v0.82.017: its own box, drawn exactly like the MENU button, so
				// the readout reads as a control rather than as text loose under
				// the picture.
				fill_rect_rgba(data, pitch, s, hud_theme().btn_fill);
				line_rectangle_rgba(data, pitch, s, hud_theme().border); // #4: match the per-race HUD frame
				char buf[32];
				uint32_t col;
				if (is_net_game) {
					int ms = net_latency_ms;
					col = ms < 80 ? 0xff40ff60u : (ms <= 160 ? 0xffffa030u : 0xffff4040u);
					snprintf(buf, sizeof(buf), "%d ms", ms);
				} else {
					// Single player has no lockstep barrier to measure, so the
					// strip says so rather than showing a meaningless 0 ms.
					col = 0xff5a626au;
					snprintf(buf, sizeof(buf), "%s", "-- ms");
				}
				int tw = (int)strlen(buf) * (ui::unispace_w + 1);
				draw_text_rgba(data, pitch, (s.from.x + s.to.x) / 2 - tw / 2,
					(s.from.y + s.to.y) / 2 - ui::unispace_h / 2, buf, col);
			}
		}

		if (cmdicon_debug_base >= 0) draw_cmdicon_debug(data, pitch, cmdicon_debug_base, 36);
		// --- under-attack minimap flash ---
		draw_under_attack_alert(data, pitch);

		// --- trigger HUD: mission texts, countdown timer, leaderboard ---
		draw_trigger_hud(data, pitch);

		// --- in-game chat history + input bar (v0.30) ---
		draw_chat(data, pitch);

		// --- cost / stats tooltip for the hovered card button ---
		draw_card_tooltip(data, pitch);
		draw_equip_tooltip(data, pitch);

		// Pause menu overlay (draw_hud_rgba returns before its own call for the
		// HUD v2 path, so it must be drawn here or MENU pauses with no window).
		draw_pause_menu(data, pitch);
		(void)p;
	}

	// Compact unit display: name + wireframe + HP/SH/EN, or group grid, or
	// building production - inside hud2_unit_panel(). (Parity WIP.)
	// --- Cargo (loaded-unit) slots: tap to unload a single passenger --------
	static constexpr int max_cargo = 8;

	unit_t* selected_transport_with_cargo() {
		if (!game_input || ui_legacy || game_local_player < 0) return nullptr;
		int own = 0; unit_t* first = nullptr;
		for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) { ++own; if (!first) first = u; } }
		if (own != 1 || !first) return nullptr;
		if (loaded_units(first).empty()) return nullptr;
		return first;
	}

	rect cargo_slot_rect(int i, int n) {
		rect panel = hud2_inner(hud2_unit_panel());
		int ph = panel.to.y - panel.from.y;
		int s = ph * 34 / 100; if (s < 14) s = 14;
		int gap = 4;
		int totalw = n * s + (n - 1) * gap;
		int x0 = panel.to.x - totalw - 4;
		int y0 = (panel.from.y + panel.to.y) / 2 - s / 2;
		int x = x0 + i * (s + gap);
		return {{x, y0}, {x + s, y0 + s}};
	}

	void draw_cargo_slots(uint32_t* data, size_t pitch) {
		unit_t* t = selected_transport_with_cargo();
		if (!t) return;
		load_wireframe();
		int n = 0; for (unit_t* nu : loaded_units(t)) { (void)nu; if (++n >= max_cargo) { n = max_cargo; break; } }
		int i = 0;
		for (unit_t* nu : loaded_units(t)) {
			if (i >= max_cargo) break;
			rect r = cargo_slot_rect(i, n);
			fill_rect_rgba(data, pitch, r, hud_theme().btn_fill);
			line_rectangle_rgba(data, pitch, r, hud_theme().btn_line);
			blit_wireframe_unit(data, pitch, nu, r.from.x + 2, r.from.y + 2, (r.to.x - r.from.x) - 4);
			++i;
		}
	}

	bool cargo_slot_hit(int mx, int my, unit_t*& target) {
		if (hud_v3) return false;
		unit_t* t = selected_transport_with_cargo();
		if (!t) return false;
		int n = 0; for (unit_t* nu : loaded_units(t)) { (void)nu; if (++n >= max_cargo) { n = max_cargo; break; } }
		int i = 0;
		for (unit_t* nu : loaded_units(t)) {
			if (i >= max_cargo) break;
			rect r = cargo_slot_rect(i, n);
			if (mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) { target = nu; return true; }
			++i;
		}
		return false;
	}

	void unload_single(unit_t* passenger) {
		if (!passenger) return;
		game_action_writer w;
		w.u8(41); // unload
		w.u16(get_unit_id(passenger).raw_value);
		submit_game_action(w);
	}

	// Shared geometry for the Unit Display production strip (right column, below
	// the name, above the atk/def badges). Fills out[0]=active thumbnail and
	// out[1..]=queued; returns the count. 0 if the building is not producing.
	// The renderer and the tap hit-test both call this so they never drift.
	int production_slot_rects(unit_t* u, rect* out, int maxn, int* out_islot = nullptr) {
		// v0.82.015: a morphing Zerg unit is not a building, and the gate here
		// used to be building-only -- so an Egg or a Cocoon showed no slot, no
		// bar and no caption, leaving the player unable to see what it was
		// turning into. Any unit with something in its build queue qualifies.
		if (!(u->unit_type->flags & unit_type_t::flag_building) && u->build_queue.empty()) return 0;
		// v0.122: only for our own units. The unit panel also shows a single
		// selected foreign unit (v0.81), and without this gate tapping an enemy
		// building showed what it was training or researching and how far along
		// it was -- information the original never gives, and in a LAN game a
		// free scout. (The slots were never tappable for a foreign unit: the
		// cancel action is owner-checked by the engine.)
		if (u->owner != game_local_player) return 0;
		bool training = !u->build_queue.empty();
		// v0.82.019: a building that is still going up has neither a queue nor a
		// research, so it got no strip at all -- a Protoss structure warping in
		// or a Zerg structure morphing showed no progress anywhere. Its own
		// remaining_build_time is the progress (bwgame.h counts it down on the
		// unit being built, not on the worker).
		bool constructing = !u_completed(u) && (u->unit_type->flags & unit_type_t::flag_building);
		bool teching = u->building.researching_type || u->building.upgrading_type;
		if (!training && !teching && !constructing) return 0;
		rect panel = hud2_inner(hud2_unit_panel());
		int pad = hud_h_px(6); // zoom-locked (constant on-screen at every zoom)
		int PW = panel.to.x - panel.from.x, PH = panel.to.y - panel.from.y;
		int lh = hud_h_px(ui::unispace_h + 2);
		int wf = hud2_wireframe_box();
		// Shifted right by three icon widths, in icon units so both tablets
		// place it the same way.
		int rcx = panel.from.x + pad + wf + pad;
		int rtop = panel.from.y + pad + lh + 3;
		int rbot = panel.to.y - pad - lh - 16;   // stay clear of the atk/def badges
		int regionH = rbot - rtop; if (regionH < 16) regionH = 16;
		// v0.82.019: the queue row always shows its full capacity. bwgame's
		// build_queue is static_vector<...,5> and build_queue_push rejects at
		// size 5, so the cap is FIVE TOTAL -- one active plus four waiting, not
		// one plus five. Empty slots are drawn as bare frames so a player who
		// does not know the game can see how much room is left.
		// v0.82.021: only where a queue is actually possible. Drawing four empty
		// frames next to an Egg or a morphing Hatchery advertised a capacity
		// that does not exist -- build_queue_push only ever reaches five for a
		// finished production BUILDING. An Egg is not a building; a structure
		// still morphing or warping in is not completed; research and upgrades
		// are `teching` and were already exempt.
		bool can_queue = training && u_completed(u)
		    && (u->unit_type->flags & unit_type_t::flag_building);
		int qcount = can_queue ? 4 : (training ? (int)u->build_queue.size() - 1 : 0);
		if (qcount > 4) qcount = 4; if (qcount < 0) qcount = 0;
		// v0.82.016: one size regardless of the queue. It used to shrink from
		// 78% to 52% of the region as soon as a second item was queued, so the
		// whole block -- icon, bar and caption -- visibly jumped smaller for no
		// reason the player could see.
		// Faze 4.7c (user request): production slots are the SAME size as the 3x3
		// card cells and the Equipment icons, scaling together across the tiers.
		// Clamp to the region height so the strip always fits.
		int islot = hud2_card_cell_size();
		if (islot > regionH) islot = regionH;
		if (islot < 8) islot = 8;
		if (out_islot) *out_islot = islot;
		// v0.83.025 (hardware feedback): nudge the whole production block one more
		// icon to the right and half an icon down, so it clears the wireframe/name.
		rcx += islot * 9 / 2;   // 4.5 icon widths
		rtop += islot / 2;      // half an icon lower
		int n = 0;
		if (n < maxn) out[n++] = {{rcx, rtop}, {rcx + islot, rtop + islot}};
		// Queue thumbnails are the same size as the active slot, by request.
		// The progress bar sits on the active slot's bottom edge and carries a
		// 2px outer border, so a 3px gap put it on top of the queue row. Scale
		// the gap with the slot instead of using a fixed few pixels.
		int qs = islot, qx = rcx, qy = rtop + islot + islot / 4;
		for (int i = 0; i < qcount; ++i) {
			if (n >= maxn) break;
			out[n++] = {{qx, qy}, {qx + qs, qy + qs}};
			qx += qs + 4;
		}
		return n;
	}

	// Hit-tests the production thumbnails; returns the build_queue index (0 =
	// active, 1.. = queued) or -1. Only training (build_queue) slots cancel.
	int production_slot_hit(int mx, int my) {
		if (!game_input || ui_legacy || hud_v3 || game_local_player < 0) return -1;
		int own = 0; unit_t* first = nullptr;
		for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) { ++own; if (!first) first = u; } }
		if (own != 1 || !first) return -1;
		if (!(first->unit_type->flags & unit_type_t::flag_building) || first->build_queue.empty()) return -1;
		rect slots[6]; int n = production_slot_rects(first, slots, 6);
		// The queue row now includes empty placeholder frames; only slots that
		// actually hold something can be cancelled.
		if (n > (int)first->build_queue.size()) n = (int)first->build_queue.size();
		for (int i = 0; i < n; ++i) {
			if (mx >= slots[i].from.x && mx < slots[i].to.x && my >= slots[i].from.y && my < slots[i].to.y) return i;
		}
		return -1;
	}

	// Shared geometry for the HUD v2 multi-unit wireframe grid AND the SCBW2 sub-card
	// selectors: the icons are packed with a minimal gap, shrunk ~15% from the fit size,
	// and RIGHT-aligned in the Unit Display (a small margin from the right edge). The
	// space that frees on the LEFT is the 1/2/3 selector column [panel_left, ox).
	struct hud2_grid_geom_t { int ox, oy, cell_w, cell_h, gb, panel_left; };
	hud2_grid_geom_t hud2_group_grid_geom() {
		rect panel = hud2_inner(hud2_unit_panel());
		hud2_grid_geom_t g{};
		const int cols = 6, rows = 2;
		int ph = panel.to.y - panel.from.y;
		g.cell_h = ph / rows;
		int fit = g.cell_h - hud_h_px(4); if (fit < 6) fit = 6;
		g.gb = fit * 85 / 100;                        // shrink icons 15%
		g.cell_w = g.gb + hud_h_px(3);                // minimal gap around the icon
		int grid_w = cols * g.cell_w;
		g.ox = panel.to.x - hud_h_px(4) - grid_w;     // right-align, small margin from edge
		g.panel_left = panel.from.x;
		if (g.ox < g.panel_left) g.ox = g.panel_left;
		g.oy = panel.from.y;
		return g;
	}

	void draw_hud2_unit_display(uint32_t* data, size_t pitch) {
		rect panel = hud2_inner(hud2_unit_panel());
		int own = 0; unit_t* first = nullptr;
		for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) { ++own; if (!first) first = u; } }
		// v0.81: fall back to a single selected unit we do not own -- a mineral
		// field, geyser or enemy unit. The panel used to bail out here, so the
		// v0.77 "Minerals: N" line could never draw for a mineral field: those
		// belong to the neutral player. BW shows the panel for foreign units
		// too; only the owner-gated parts (production queue, command card)
		// stay hidden, and they already check ownership themselves.
		if (!first && current_selection.size() == 1) first = get_unit(current_selection.front());
		// v0.122: the tap-to-focus hit list. Only draw_selection_group() -- the
		// LEGACY HUD's grid -- used to fill it, and draw_hud_rgba returns before
		// reaching that when HUD v2 is on, so in HUD v2 a tap on a grid cell could
		// never focus its unit. The cells are published below, where they are drawn.
		selection_grid_hit.clear();
		if (!first) return;
		load_wireframe();
		int pad = hud_h_px(6); // zoom-locked (constant on-screen at every zoom)
		int box = (panel.to.y - panel.from.y) - pad * 2;
		if (box > (panel.to.x - panel.from.x) / 3) box = (panel.to.x - panel.from.x) / 3;
		// group grid: compressed, shrunk 15%, right-aligned; the freed left column holds
		// the SCBW2 1/2/3 sub-card selectors (drawn separately, only when >12 selected).
		if (own > 1) {
			const int cols = 6, rows = 2;
			hud2_grid_geom_t gg = hud2_group_grid_geom();
			int gpad = hud_h_px(2); // zoom-locked cell gaps
			int gb = gg.gb; if (gb < 6) return;
			int i = 0;
			for (auto uid : current_selection) {
				unit_t* u = get_unit(uid); if (!u || u->owner != game_local_player) continue;
				int col = i % cols, row = i / cols; if (row >= rows) break;
				int cx = gg.ox + col * gg.cell_w, cy = gg.oy + row * gg.cell_h;
				int gx = cx + (gg.cell_w - gb) / 2, gy = cy + gpad;
				selection_grid_hit.push_back({rect{{cx, cy}, {cx + gg.cell_w, cy + gg.cell_h}}, get_unit_id(u)});
				// Faze 4.7: authentic SC:R per-race group cell frame (cmdbtns 13)
				// behind each wireframe; falls back to the plain cell under MPQ.
				race_t grace = (game_local_player >= 0 && game_local_player < 12)
					? st.players[game_local_player].race : unit_race(u);
				if (ddsgrp::frame_t* cf = get_console_frame(grace, 13))
					blit_console_frame(data, pitch, cf, rect{{gx - gpad, gy - gpad}, {gx + gb + gpad, gy + gb + gpad}});
				blit_wireframe_small(data, pitch, u, gx, gy, gb, grpwire_grp);
				// v0.83.004: heroes get the original's white cell border, so a
				// named unit is findable in a screen of identical silhouettes.
				// units.dat's own hero flag (flag_hero, data_types.h:88) decides
				// it -- no list of unit ids to drift out of date.
				if (ut_flag(u, unit_type_t::flag_hero)) {
					rect cell = {{gx - gpad, gy - gpad}, {gx + gb + gpad, gy + gb + gpad}};
					line_rectangle_rgba(data, pitch, cell, 0xffffffffu);
				}
				// SCBW2 group-focus: highlight the focused unit's cell (yellow HUD accent)
				// so it is clear whose 3x3 card + portrait are showing.
				if (get_unit_id(u) == focused_unit_id && current_selection.size() > 1) {
					rect cell = {{gx - gpad, gy - gpad}, {gx + gb + gpad, gy + gb + gpad}};
					line_rectangle_rgba(data, pitch, cell, hud_theme().amber);
				}
				++i;
			}
			return;
		}
		unit_t* u = first;
		int PW = panel.to.x - panel.from.x, PH = panel.to.y - panel.from.y;
		int lh = hud_h_px(ui::unispace_h + 2), adv = usp_adv();
		// Wireframe: a square in the TOP-LEFT, ~55% of the panel height (not the
		// legacy full-height proportion), leaving room for stats below.
		// The wireframe is the panel's subject; geometry in hud2_wireframe_box().
		int wf = hud2_wireframe_box();
		int hpp = unit_hp_percent(u);
		blit_wireframe_unit(data, pitch, u, panel.from.x + pad, panel.from.y + pad, wf);
		// v0.83.032 (hardware feedback): a hero's marker is now the wireframe's own
		// bright OUTERMOST outline layer (white, or neon cyan for shielded Protoss),
		// drawn inside blit_wireframe_unit/draw_wire_rim -- not a rectangular frame.
		// So the old 2px white box here is gone.
		// Name centered over the area right of the wireframe.
		load_unit_names();
		a_string name = unit_name(u->unit_type);
		int rx = panel.from.x + pad + wf + pad;                 // right column x
		if (!name.empty()) {
			int nw = (int)name.size() * adv;
			int cx = (rx + panel.to.x) / 2 - nw / 2;
			draw_text_rgba(data, pitch, cx, panel.from.y + pad, name.c_str(), 0xffd8dee2u);
		}
		// v0.67 items 18.2 / 18.8: the text column under the unit's name. The
		// original stacks the status lines and the rank/kills there, centred in
		// the same column as the name -- ours had "Detector" left-flushed while
		// the name above it was centred, which is why it read as misaligned.
		// "Detector" is one of six status lines in stat_txt.tbl (827-832), so
		// the others are shown here too instead of only that one.
		{
			int ty2 = panel.from.y + pad + lh;
			auto centred = [&](const char* s, uint32_t col) {
				int w = (int)strlen(s) * adv;
				draw_text_rgba(data, pitch, (rx + panel.to.x) / 2 - w / 2, ty2, s, col);
				ty2 += lh;
			};
			// v0.77 item 4: a selected mineral field or geyser reports what is
			// left in it. Authentic, not an SC2 borrowing -- stat_txt.tbl has
			// "Minerals:" (796) and "Vespene Gas:" (797) for exactly this panel.
			bool ore = unit_is_mineral_field(u);
			if (ore || unit_is(u, UnitTypes::Resource_Vespene_Geyser)
			    || unit_is(u, UnitTypes::Terran_Refinery)
			    || unit_is(u, UnitTypes::Protoss_Assimilator)
			    || unit_is(u, UnitTypes::Zerg_Extractor)) {
				char rb[48];
				snprintf(rb, sizeof(rb), "%s %d", ore ? "Minerals:" : "Vespene Gas:",
					(int)u->building.resource.resource_count);
				centred(rb, ore ? 0xff9ad0ffu : 0xff70e090u);
			}
			if (ut_detector(u)) centred("Detector", hud_theme().amber);
			// Nuclear Silo readout (user request): the sim already produces and
			// STORES the nuke (order_NukeTrain -> building.silo.nuke, .ready) and the
			// Ghost's strike binds to it (order_CastNuclearStrike scans silos for
			// nuke && ready) -- but there was no on-screen sign the silo held one, so
			// it read as unwired. Show it in this status column for the silo addon
			// itself OR the Command Center it hangs off. Empty=red, arming=amber,
			// ready=green (matches the Ghost's usable state).
			{
				unit_t* silo = nullptr;
				if (unit_is(u, UnitTypes::Terran_Nuclear_Silo)) silo = u;
				else if (u->building.addon && unit_is(u->building.addon, UnitTypes::Terran_Nuclear_Silo))
					silo = u->building.addon;
				if (silo) {
					if (silo->building.silo.nuke && silo->building.silo.ready)
						centred("Nuke available", 0xff30f030u);       // green: launch-ready
					else if (silo->building.silo.nuke)
						centred("Arming Nuke", hud_theme().amber);           // amber: still building
					else
						centred("Empty", 0xffff4040u);                 // red: no nuke
				}
			}
			if (u_hallucination(u)) centred("Hallucination", 0xff8ad0ffu);
			if (u->parasite_flags) centred("Parasite Detected", 0xffff8080u);
			if (u_disabled(u)) centred("Disabled", 0xffa0a8b0u);
			// stat_txt.tbl also has "Unpowered" (830); left out because the
			// engine exposes no powered/unpowered query and guessing one would
			// mislabel Protoss buildings rather than inform.
			// Rank and kills. v0.83.004 correction to my own note here: the sim
			// did NOT maintain kill_count -- nothing incremented it, so this line
			// could never appear. bwgame.h now credits the killing unit where it
			// already credited the killing player. The promotion thresholds are
			// a separate question: they are not in any .dat file, so rank_increase
			// stays at 0 and the rank line reads the ladder's first step. Do not
			// invent thresholds; measure them or leave it.
			// Ranks come from stat_txt.tbl 550-572; the line is omitted at zero
			// kills, as in the original.
			// v0.83.005: rank and kills are ALWAYS shown (the user's call --
			// a zero is information too), and drawn at 65% so they read as a
			// footnote under the name rather than competing with it.
			auto centred_small = [&](const char* s, uint32_t col) {
				int pct = 75;   // v0.83.007: 65% was legible on my screen, not on a tablet
				int sadv = (ui::unispace_w + 1) * hud_text_pct(pct) / 100;
				int w = (int)strlen(s) * sadv;
				draw_text_rgba_scaled(data, pitch, (rx + panel.to.x) / 2 - w / 2, ty2, s, col, pct);
				ty2 += ui::unispace_h * hud_text_pct(pct) / 100 + hud_h_px(2);
			};
			// v0.83.025 (hardware feedback): rank is a UNIT trait -- buildings
			// don't earn ranks, so the ladder line is unit-only. Kills belong only
			// to things that can actually destroy units: every unit, but among
			// buildings only the armed ones (turret/cannon/sunken/spore/bunker-less
			// combat structures) -- never a depot, pylon or barracks.
			bool is_bldg = ut_building(u);
			const weapon_type_t* kgw = u->unit_type->ground_weapon;
			const weapon_type_t* kaw = u->unit_type->air_weapon;
			bool can_attack = (kgw && kgw->damage_amount > 0) || (kaw && kaw->damage_amount > 0);
			const char* rank = unit_rank_name(u);
			if (rank && !is_bldg) centred_small(rank, 0xffc8b878u);
			if (!is_bldg || can_attack) {
				char kb[32];
				snprintf(kb, sizeof(kb), "Kills: %d", (int)u->kill_count);
				centred_small(kb, 0xffc8b878u);
			}
		}
		// HP/SH/EN stacked under the wireframe, centred on it.
		// v0.82.014: the "HP"/"SH"/"EN" prefixes are gone -- the colour already
		// says which pool a line is (green/blue/violet), so the letters were
		// spending width to repeat it. The lines are also drawn small and
		// anchored to the wireframe's centre rather than the panel's left edge.
		char line[48];
		int small_lh = lh * 4 / 5;
		// v0.82.023: with the bigger wireframe the block could run past the
		// panel and the shield line was clipped. Keep it under the wireframe,
		// but never below the panel's own floor.
		int pools = 1 + (u->unit_type->has_shield ? 1 : 0)
		              + ((u->unit_type->flags & unit_type_t::flag_has_energy) ? 1 : 0);
		int ty = panel.from.y + pad + wf + 2;
		int ty_max = panel.to.y - pad - pools * small_lh;
		if (ty > ty_max) ty = ty_max;
		int wcx = panel.from.x + pad + wf / 2;
		auto pool_line = [&](const char* s, uint32_t col) {
			int w = (int)strlen(s) * adv * 4 / 5;
			draw_text_rgba_scaled(data, pitch, wcx - w / 2, ty, s, col, 80);
			ty += small_lh;
		};
		snprintf(line, sizeof(line), "%d/%d", (int)u->hp.integer_part(), (int)u->unit_type->hitpoints.integer_part());
		pool_line(line, hp_tint(hpp) | 0xff000000u);
		if (u->unit_type->has_shield) {
			snprintf(line, sizeof(line), "%d/%d", (int)u->shield_points.integer_part(), u->unit_type->shield_points);
			pool_line(line, 0xff5cb0ffu);
		}
		if (u->unit_type->flags & unit_type_t::flag_has_energy) {
			snprintf(line, sizeof(line), "%d/%d", (int)u->energy.integer_part(), (int)unit_max_energy(u).integer_part());
			pool_line(line, 0xffc884ffu);
		}
		// attack / defence with yellow value badges, bottom-right of the panel.
		// v0.67 item 18.6: the manual calls this the Equipment display --
		// "Armour, Weapons or Special Equipment and their levels of upgrade" --
		// so the upgrade bonus is shown separately as "base+bonus" instead of
		// being folded away or omitted. Multi-hit weapons (Zealot, Hydralisk
		// scarabs and the like) show their factor, which HUD v2 was dropping
		// even though the legacy console already had it.
		const weapon_type_t* gw = u->unit_type->ground_weapon;
		const weapon_type_t* aw = u->unit_type->air_weapon;
		const weapon_type_t* w = gw && gw->damage_amount > 0 ? gw : (aw && aw->damage_amount > 0 ? aw : nullptr);
		int bx = rx, by = panel.to.y - pad - lh - 12 * hud_us(), bs = 14 * hud_us();
		auto stat_badge = [&](int& x, const char* lbl, const char* val) {
			draw_text_rgba(data, pitch, x, by - lh, lbl, 0xffb8c0c4u);
			int vw = (int)strlen(val) * adv;
			int bw2 = vw + 6 * hud_us(); if (bw2 < bs) bw2 = bs;
			fill_rect_rgba(data, pitch, {{x, by}, {x + bw2, by + bs}}, hud_theme().amber);
			draw_text_rgba(data, pitch, x + bw2 / 2 - vw / 2, by + (bs - usp_h()) / 2, val, 0xff101010u);
			int lw = (int)strlen(lbl) * adv;
			x += (bw2 > lw ? bw2 : lw) + 12 * hud_us();
		};
		(void)stat_badge;
		// v0.82.008 (design E1): the Equipment display shows the unit's actual
		// upgrade GROUPS as cmdicons with their level, not the words atk/def.
		// The group is per unit, not per race -- upgrades.dat has seven armour
		// and eight weapon groups, so a Zergling (Melee Attacks) and a
		// Hydralisk (Missile Attacks) legitimately show different icons. All
		// sixteen already map to distinct frames in upgrade_icon(); this only
		// had to be wired up. Protoss gains a third icon, Plasma Shields, which
		// is exactly why the reference shows 3 for a Probe and 2 for an SCV.
		equip_hit.clear();
		{
			// v0.82.014: +15% on the cell, and the whole row shifted right by one
			// and a half icon widths. Offsets are expressed in icon widths, not
			// pixels or millimetres, so the S7+ and the S8 Ultra place them the
			// same way despite different DPI.
			// Faze 4.7c (user request): Equipment icons are the SAME size as the 3x3
			// card cells (the one HUD icon anchor), so all three match and scale
			// together across SD/HD2/FULL HD. Clamp to half the panel height as a
			// safety so the row always fits.
			int es = hud2_card_cell_size();
			int es_max = (panel.to.y - panel.from.y) / 2; if (es_max < 12) es_max = 12;
			if (es > es_max) es = es_max;
			int ex = rx + es * 3 / 2, ey = panel.to.y - pad - es;
			uint32_t accent = race_accent(u);
			race_t prace = (u->owner < 12) ? st.players[u->owner].race : unit_race(u);
			auto equip = [&](UpgradeTypes up, int icon_override, const weapon_type_t* wpn = nullptr) {
				if (ex + es > panel.to.x) return;
				int icon = icon_override >= 0 ? icon_override : upgrade_icon(up);
				// Not every unit belongs to an upgrade group -- buildings have no
				// armour group, and upgrades.dat leaves those entries' icon at 0.
				// Drawing the slot anyway gave a Nexus an empty box with a "0"
				// badge next to its shields.
				if (icon <= 0) return;
				int lvl = player_upgrade_level(u->owner, up);
				// Grey, not the command card's gold: the Equipment icons are
				// etched metal in the original.
				// Icon inset slightly so it sits INSIDE the frame bezel below.
				draw_cmdicon_fit(data, pitch, icon, rect{{ex, ey}, {ex + es, ey + es}},
					equip_grey, 10);
				// Faze 4.7 (user request): the authentic SC:R Equipment slot frame is
				// cmdbtns frame 12 (per race), drawn OVER the icon as a bezel.
				ddsgrp::frame_t* eqframe = get_console_frame(prace, 12);
				char lv[8];
				snprintf(lv, sizeof(lv), "%d", lvl);
				uint32_t lvcol = lvl > 0 ? hud_theme().amber : 0xff98a0a8u;
				if (eqframe) {
					blit_console_frame(data, pitch, eqframe, rect{{ex, ey}, {ex + es, ey + es}});
					// The frame carries its own BLACK FIELD in the notched bottom-right
					// corner (measured via the grp-toolkit oracle: clean black area
					// centred at ~0.77x / 0.83y of the cell). Drop the BARE number there
					// -- no box, no border. Size it as a fraction of the cell so SD /
					// HD2 / FULL HD match; disable hud_scale_text so the fraction is not
					// scaled a second time.
					bool save = hud_scale_text; hud_scale_text = false;
					int nh = es * 20 / 100; if (nh < 6) nh = 6;
					int pct = nh * 100 / ui::unispace_h; if (pct < 40) pct = 40;
					int nw = (int)strlen(lv) * (ui::unispace_w + 1) * pct / 100;
					int nx = ex + es * 77 / 100 - nw / 2;
					int ny = ey + es * 83 / 100 - nh / 2;
					draw_text_rgba_scaled(data, pitch, nx, ny, lv, lvcol, pct);
					hud_scale_text = save;
				} else {
					// MPQ / no SC:R frame: keep the HUD v2 badge box + race border.
					int lw = (int)strlen(lv) * adv;
					rect bg{{ex + es - lw - 7 * hud_us(), ey + es - usp_h() - 5 * hud_us()},
					        {ex + es, ey + es}};
					fill_rect_rgba(data, pitch, bg, 0xff101418u);
					draw_border_rgba(data, pitch, bg, 0xff4c5860u);
					draw_text_rgba(data, pitch, bg.from.x + 3 * hud_us(), bg.from.y + 2 * hud_us(), lv, lvcol);
					draw_border_rgba(data, pitch, rect{{ex - 1, ey - 1}, {ex + es + 1, ey + es + 1}}, accent);
				}
				equip_hit.push_back({rect{{ex, ey}, {ex + es, ey + es}}, (int)up, wpn});
				ex += es + 6;
			};
			// v0.82.009: the weapon slot shows the WEAPON's own icon, not the
			// icon of the upgrade that improves it. weapons.dat carries a
			// dedicated icon per weapon (Gauss Rifle 323, C-10 Canister Rifle
			// 324, ...) -- measured with `aidump equip` against the user's MPQ.
			// Using damage_upgrade's icon collapsed every Terran infantry unit
			// onto the same "Terran Infantry Weapons" frame. Armour and shields
			// have no such per-unit asset, so those stay on the upgrade icon.
			// Authentic SC order: armour, then shields, then the weapon.
			equip(u->unit_type->armor_upgrade, -1);
			if (u->unit_type->has_shield) equip(UpgradeTypes::Protoss_Plasma_Shields, -1);
			if (w) equip(w->damage_upgrade, w->icon, w);
		}
		// A producing building keeps all of the above (wireframe/name/HP/def) and
		// shows its production/research strip in the right column below the name.
		draw_production_strip(data, pitch, u);
	}

	// Draws the production/research strip (active icon + progress bar + timer,
	// plus queued thumbnails) inside the Unit Display right column. No-op unless
	// the building is training or teching. Geometry via production_slot_rects.
	void draw_production_strip(uint32_t* data, size_t pitch, unit_t* u) {
		rect slots[6]; int islot = 0;
		int n = production_slot_rects(u, slots, 6, &islot);
		if (n == 0) return;
		rect panel = hud2_inner(hud2_unit_panel());
		int pad = hud_h_px(6); // zoom-locked (constant on-screen at every zoom)
		bool training = !u->build_queue.empty();
		bool constructing = !u_completed(u) && (u->unit_type->flags & unit_type_t::flag_building);
		// A worker walking to a build site keeps the target in its build_queue
		// but has no unit under construction yet. Its OWN remaining_build_time is
		// zero -- it is a finished unit -- so the old
		//   rem = current_build_unit ? ... : u->remaining_build_time
		// scored it as done and the bar sat at 100% the whole way there. Show
		// nothing until there is something to measure.
		// v0.82.020: the first version of this guard tested "not a building and
		// no current_build_unit", which also caught a Zerg Egg -- an Egg is not
		// a building and builds nothing, it IS the thing being built -- so the
		// morph lost its bar. Test what actually matters instead: is there any
		// timer to read? The walking worker has neither, the Egg has its own.
		if (training && !u->current_build_unit && u->remaining_build_time == 0) return;
		// Active slot [0]: unit wireframe (training) or tech cmdicon (research).
		// v0.82.015: the slot frame, the bar fill and the bar's outer border all
		// take the selected unit's race colour, so the strip reads as belonging
		// to that race rather than to the HUD.
		uint32_t accent = race_accent(u);
		race_t prace = u->owner < 12 ? st.players[u->owner].race : unit_race(u);
		rect a = slots[0];
		fill_rect_rgba(data, pitch, a, hud_theme().btn_fill);
		line_rectangle_rgba(data, pitch, a, accent);
		int total = 0, rem = 0;
		const char* caption = nullptr;
		if (constructing && !training) {
			// A structure going up on its own: Protoss warp-in, or a Zerg
			// building mid-morph. The progress is its own remaining_build_time.
			load_cmdicons();
			if (cmdicons_ok) draw_cmdicon_fit(data, pitch, (int)u->unit_type->id, a);
			total = u->unit_type->build_time;
			rem = u->remaining_build_time;
			caption = prace == race_t::zerg ? "Evolving"
			        : prace == race_t::protoss ? "Warping" : "Building";
		} else if (training) {
			const unit_type_t* act = u->build_queue[0];
			// v0.67 item 18.3: the original draws cmdicons here, not wireframes
			// (we already did for research/upgrade -- unit training was the
			// inconsistent one).
			load_cmdicons();
			if (cmdicons_ok) draw_cmdicon_fit(data, pitch, (int)act->id, a);
			else blit_wireframe_type(data, pitch, act, a.from.x + 2, a.from.y + 2, islot - 4, 0x30f030u);
			total = act->build_time;
			rem = u->current_build_unit ? u->current_build_unit->remaining_build_time : u->remaining_build_time;
			// v0.82.015: per-race wording, by request. Note this DEPARTS from
			// the original: stat_txt.tbl has Building / Morphing / Evolving but
			// no "Training" or "Warping" string, so BW leaves unit production
			// captionless and lets the icon speak. Keeping the deviation
			// recorded rather than silently presenting it as authentic.
			// v0.82.021: Zerg never says "Under Construction" -- a Hatchery
			// becoming a Lair is a creature changing, not a site being built.
			// Structures evolve, units morph.
			bool to_building = (act->flags & unit_type_t::flag_building) != 0
			    || (u->unit_type->flags & unit_type_t::flag_building) != 0;
			if (prace == race_t::zerg) caption = to_building ? "Evolving" : "Morphing";
			else if (!u_completed(u) && (u->unit_type->flags & unit_type_t::flag_building))
				caption = prace == race_t::protoss ? "Warping" : "Under Construction";
			else if (act->flags & unit_type_t::flag_building) caption = "Building";
			else caption = prace == race_t::protoss ? "Warping" : "Training";
		} else {
			int frame;
			if (u->building.researching_type) {
				total = u->building.researching_type->research_time;
				frame = tech_icon(u->building.researching_type->id);
				// Zerg research reads as "Evolving" by request; the other two
				// keep stat_txt.tbl 769 "Researching".
				caption = prace == race_t::zerg ? "Evolving" : "Researching";
			} else {
				total = upgrade_time_cost(u->owner, u->building.upgrading_type);
				frame = upgrade_icon(u->building.upgrading_type->id);
				caption = prace == race_t::zerg ? "Evolving" : "Upgrading";   // 766
			}
			rem = u->building.upgrade_research_time;
			load_cmdicons();
			// v0.83.016: _fit (zoom-invariant, scales to the slot) instead of the
			// native-size draw_cmdicon that grew with pinch-zoom.
			if (frame >= 0 && cmdicons_ok) draw_cmdicon_fit(data, pitch, frame, a);
		}
		int done = total - rem; if (done < 0) done = 0; if (total > 0 && done > total) done = total;
		// v0.67 item 18.5: the bar used to run all the way to the panel edge,
		// which is what made it look enormous next to a small icon. It is now
		// sized relative to the icon (three icons wide, capped by the space
		// left) and its height is a fixed fraction of the icon, so the pair
		// stays in proportion at any panel size.
		int bx = a.to.x + hud_h_px(8); // zoom-locked gap
		int avail = panel.to.x - pad - bx;
		int bw = islot * 3; if (bw > avail) bw = avail;
		int bh = islot / 5; if (bh < 5) bh = 5;
		// v0.82.019: sits on the slot's bottom edge instead of floating at its
		// vertical centre, so icon and bar share a baseline.
		int byy = a.to.y - bh;
		if (bw > 8) {
			// v0.67 item 18.4: caption above the bar, left-aligned with it.
			if (caption) {
				// Centred over the bar with a clear gap: it used to be left
				// aligned and only 3px up, so with the outer border added it sat
				// on top of the bar.
				// Faze 4.7c: caption text scales with the tier (hud_scale_text), so
				// its height/width metrics must too, or it overlaps the bar and
				// mis-centres at FULL HD.
				int cy = byy - usp_h() - hud_h_px(6) - islot / 10;
				if (cy < a.from.y) cy = a.from.y;
				int cwid = (int)strlen(caption) * usp_adv();
				draw_text_rgba(data, pitch, bx + bw / 2 - cwid / 2, cy, caption, 0xffb8c0c4u);
			}
			fill_rect_rgba(data, pitch, {{bx, byy}, {bx + bw, byy + bh}}, 0xff202020u);
			int fillw = total > 0 ? bw * done / total : 0;
			// Segmented fill: discrete blocks rather than one sliding bar, in
			// the race's colour. Terran green, Protoss cyan, Zerg orange.
			uint32_t fillcol = prace == race_t::zerg ? 0xffe08828u
			                 : prace == race_t::protoss ? 0xff40d0e8u : 0xff40ff70u;
			int seg = bh, gapw = bh / 3; if (gapw < 1) gapw = 1;
			for (int sx = 0; sx + seg <= fillw; sx += seg + gapw)
				fill_rect_rgba(data, pitch, {{bx + sx, byy}, {bx + sx + seg, byy + bh}}, fillcol);
			line_rectangle_rgba(data, pitch, {{bx, byy}, {bx + bw, byy + bh}}, hud_theme().btn_line);
			// Extra outer border in the race colour, one pixel clear of the bar.
			draw_border_rgba(data, pitch, {{bx - 2, byy - 2}, {bx + bw + 2, byy + bh + 2}}, accent);
			// Countdown timer at the bar's top-right corner (seconds remaining).
			int secs = (rem + 23) / 24; if (secs < 0) secs = 0;
			char t[8]; snprintf(t, sizeof(t), "%ds", secs);
			// Seconds remaining sit just past the bar's right end, so they no
			// longer share a line with the caption above it.
			int adv = usp_adv();
			int tx = bx + bw + hud_h_px(6);
			if (tx + (int)strlen(t) * adv > panel.to.x - pad) tx = bx + bw - (int)strlen(t) * adv;
			draw_text_rgba(data, pitch, tx, byy + bh / 2 - usp_h() / 2, t, 0xffd8dce0u);
		}
		// Queued thumbnails, then the remaining capacity as bare frames. Showing
		// the empty slots means a player who does not know the game can see how
		// much room is left instead of having to guess.
		for (int i = 1; i < n; ++i) {
			bool filled = i < (int)u->build_queue.size();
			fill_rect_rgba(data, pitch, slots[i], hud_theme().btn_fill);
			line_rectangle_rgba(data, pitch, slots[i], filled ? accent : hud_theme().slot_free_line);
			if (!filled) continue;
			const unit_type_t* qt = u->build_queue[i];
			if (cmdicons_ok) draw_cmdicon_fit(data, pitch, (int)qt->id, slots[i]);
			else blit_wireframe_type(data, pitch, qt, slots[i].from.x + 2, slots[i].from.y + 2, (slots[i].to.x - slots[i].from.x) - 4, 0x30f030u);
		}
	}

	// A fading ping ring on the minimap for ~48 frames: an expanding ring with a
	// black edge and a dot at the pinged spot.
	void draw_ping_marker(uint32_t* data, size_t pitch) {
		if (ping_pos.x < 0) return;
		int age = (int)st.current_frame - ping_frame;
		if (age < 0 || age >= 48) { return; }
		rect mm = get_minimap_area();
		if (mm.to.x <= mm.from.x) return;
		int mx = mm.from.x + (int)((int64_t)ping_pos.x * (mm.to.x - mm.from.x) / (int)game_st.map_width);
		int my = mm.from.y + (int)((int64_t)ping_pos.y * (mm.to.y - mm.from.y) / (int)game_st.map_height);
		const int t = mark_t(), e = mark_e();
		int rad = (3 + age / 4) * e;           // expanding ring
		uint32_t col = (age & 4) ? 0xffff4040u : 0xffffd040u; // blink
		for (int pass = 0; pass != 2; ++pass) {
			const int size = pass ? t : t + 2 * e, off = t / 2 + (pass ? 0 : e);
			const uint32_t c = pass ? col : 0xff000000u;
			for (int a = 0; a < 360; a += 4) {
				double rr = a * 3.14159265 / 180.0;
				int px = mx + (int)std::lround(rad * cos(rr)), py = my + (int)std::lround(rad * sin(rr));
				mark_box(data, pitch, px - off, py - off, px - off + size, py - off + size, c, (int)screen_height);
			}
			mark_box(data, pitch, mx - off, my - off, mx - off + size, my - off + size, c, (int)screen_height);
		}
	}

	// HUD v2 resources (top-right): minerals / gas / supply, color-coded.
	void draw_hud2_resources(uint32_t* data, size_t pitch) {
		int p = game_local_player;
		size_t race = (size_t)st.players[p].race; if (race > 2) race = 0;
		int su = st.supply_used[p][race].integer_part();
		int sm = st.supply_available[p][race].integer_part(); if (sm > 200) sm = 200;
		// v0.82.035: the authentic game\icons.grp icons instead of "M"/"G"/"S".
		// HUD v2 never even called load_icons() -- only the legacy console did --
		// so the icons could not have appeared here however the file was parsed.
		// That was a second, independent reason on top of the format problem.
		load_icons();
		bool ico = icons_ok && icons_raw.size() >= 7;
		// icons.grp resource frames, MEASURED with `aidump icondump 0 16
		// game\icons.grp` (12 frames, all 14x14 except 11):
		//   0        mineral crystal (blue)
		//   1  2  3  gas: zerg organic / terran drum / protoss crystal
		//   4  5  6  supply: zergling / marine / zealot silhouette
		//   7  8     empty (share frame 9's offset, no data)
		//   9        energy bolt        10  blank swatch
		//   11       53x14 green->red gradient bar
		// So gas = 1 + race and supply = 4 + race in the engine's race order
		// (zerg 0, terran 1, protoss 2). Verified against the artwork, not
		// assumed from the pattern.
		int f_min = 0, f_gas = 1 + (int)race, f_sup = 4 + (int)race;
		// Faze 4.7d: prefer HD resource icons (game/icons.dds.grp) over the SD
		// game\icons.grp when HD assets are active (the SD ones were 14x14).
		bool hd_ico = get_hd_resource_icon(f_min) != nullptr;
		bool any_ico = hd_ico || ico;
		char m[24], g[24], s[32];
		snprintf(m, sizeof(m), any_ico ? "%d" : "M %d", st.current_minerals[p]);
		snprintf(g, sizeof(g), any_ico ? "%d" : "G %d", st.current_gas[p]);
		if (any_ico) snprintf(s, sizeof(s), "%d/%d", su, sm);
		else snprintf(s, sizeof(s), "S %d/%d", su, sm);
		int us = hud_us();
		int adv = usp_adv(), gap = hud_h_px(14);             // zoom-locked
		int ih = usp_h() + hud_h_px(2), isp = hud_h_px(4); // HD icon display height + icon gap
		int frames3[3] = { f_min, f_gas, f_sup };
		int icon_w_total = 0;
		if (hd_ico) {
			for (int k = 0; k < 3; ++k) { auto* h = get_hd_resource_icon(frames3[k]); icon_w_total += (h ? h->w * ih / h->h : 0) + isp; }
		} else if (ico) {
			icon_w_total = (icons_raw[0].w * us + isp) * 3;
		}
		int wtot = ((int)strlen(m) + (int)strlen(g) + (int)strlen(s)) * adv + gap * 2 + icon_w_total;
		int x = (int)screen_width - wtot - 10 * us, y = 6 * us;
		auto seg = [&](int frame, const char* txt, uint32_t col) {
			if (hd_ico) {
				if (auto* h = get_hd_resource_icon(frame)) {
					int iy = y + (usp_h() - ih) / 2;
					blit_hd_icon(data, pitch, *h, x, iy, ih);
					x += h->w * ih / h->h + isp;
				}
			} else if (ico && frame < (int)icons_raw.size()) {
				auto& f = icons_raw[frame];
				int iy = y + (usp_h() - f.h * us) / 2; if (iy < 0) iy = 0;
				blit_raw_icon(data, pitch, f, x, iy, us);
				x += f.w * us + isp;
			}
			draw_text_rgba(data, pitch, x, y, txt, col);
			x += (int)strlen(txt) * adv + gap;
		};
		seg(f_min, m, 0xff6cc0ffu);
		seg(f_gas, g, 0xff5fe07au);
		// Red when supply-blocked (at or over the cap), so the player notices
		// they cannot produce until they build more supply.
		seg(f_sup, s, su >= sm ? 0xffff5050u : 0xffdfe7efu);
	}

	// HUD v2 command card (3x3) inside hud2_card_panel(). Equal gaps between the
	// cells AND from the frame edge, both axes (the panel is square).
	// Faze 4.7c (user request): the 3x3 card cell size is the ONE HUD icon-size
	// anchor -- Equipment icons and production slots use it too so all three match
	// and scale together across SD/HD2/FULL HD (the card panel scales with the
	// tier). Returns the square cell edge in buffer px.
	int hud2_card_cell_size() {
		if (hud_v3) return d3(64);
		rect c = hud2_inner(hud2_card_panel());
		int W = c.to.x - c.from.x, H = c.to.y - c.from.y;
		int side = W < H ? W : H; if (side < 12) side = 12;
		int g = side / 20; if (g < 2) g = 2;
		int cell = (side - 4 * g) / 3; if (cell < 6) cell = 6;
		return cell;
	}
	rect hud2_card_cell(int i) {
		if (hud_v3) return hud3_dock_cell(i);
		rect c = hud2_inner(hud2_card_panel());
		int W = c.to.x - c.from.x, H = c.to.y - c.from.y;
		int side = W < H ? W : H;
		int g = side / 20; if (g < 2) g = 2;         // equal gap
		int cell = (side - 4 * g) / 3;                // 3 cells + 4 gaps
		int ox = c.from.x + (W - (3 * cell + 4 * g)) / 2 + g;
		int oy = c.from.y + (H - (3 * cell + 4 * g)) / 2 + g;
		int col = i % 3, row = i / 3;
		int x = ox + col * (cell + g), y = oy + row * (cell + g);
		return {{x, y}, {x + cell, y + cell}};
	}
	void draw_hud2_card(uint32_t* data, size_t pitch) {
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		if (buttons.empty()) return;
		load_cmdicons();
		const hud_theme_t cth = hud_theme();
		for (size_t i = 0; i != buttons.size(); ++i) {
			// Fixed slots mean the card legitimately has holes; an empty slot
			// draws nothing at all, like the original's blank button positions.
			if (buttons[i].action == cardact_none) continue;
			rect r = hud2_card_cell((int)i);
			bool armed = (buttons[i].action == cardact_attack && attack_armed)
				|| (buttons[i].action == cardact_ability_target && ability_armed_order == buttons[i].arg);
			fill_rect_rgba(data, pitch, r, armed ? cth.armed_fill : (hud_pressed(r) ? cth.btn_pressed : cth.btn_fill));
			line_rectangle_rgba(data, pitch, r, armed ? cth.armed_line : cth.btn_line);
			int icon = card_icon_frame(buttons[i].action, buttons[i].arg);
			if (icon >= 0 && cmdicons_ok) {
				// v0.83.016: fit the icon to the (zoom-invariant) cell rather than
				// blitting it at native cmdicons size. draw_cmdicon() draws a fixed
				// buffer-pixel icon, and pinch-zoom resizes the render buffer, so a
				// native-size icon grew/shrank with zoom while the panel around it
				// stayed put -- the "icons still zoom" bug. _fit scales to the cell,
				// which is a constant fraction of the buffer, so it stays put too.
				// v0.83.05x (user request): the 3x3 card icons render as BRIGHT
				// YELLOW line-art, not the muted gold/grey they read as before.
				// Faze 4.7b (user request): building icons (Build + Advanced, plus
				// Zerg building morphs and Terran addons) read as compressed with the
				// default inset, so give them a smaller margin to fill the button.
				int mgn = 12;
				switch (buttons[i].action) {
				case cardact_build: case cardact_morph_building: case cardact_addon: mgn = 3; break;
				default: break;
				}
				draw_cmdicon_fit(data, pitch, icon, r, false, mgn, cth.icon_tint);
			} else {
				// Abilities/research/upgrade have no authentic icon mapped yet;
				// draw the text label so the button is still usable.
				draw_card_label(data, pitch, r, buttons[i].label);
			}
			// Requirement not met: dim the whole cell so it reads as unavailable
			// (the engine would reject the order). Authentic SC greys these out.
			if (!buttons[i].available) {
				blend_rect_rgba(data, pitch, r, cth.dim_rgb, 150);
				line_rectangle_rgba(data, pitch, r, cth.dim_line);
			}
			// Multi-level upgrade: show the NEXT level in the top-left corner, so a
			// tiered upgrade reads as "Lvl 2 available" rather than looking stuck.
			if (buttons[i].action == cardact_upgrade && game_local_player >= 0) {
				UpgradeTypes ut = (UpgradeTypes)buttons[i].arg;
				int maxlv = player_max_upgrade_level(game_local_player, ut);
				if (maxlv > 1) {
					char lv[4]; snprintf(lv, sizeof lv, "%d", player_upgrade_level(game_local_player, ut) + 1);
					int bs = hud_h_px(ui::unispace_h + 3);
					fill_rect_rgba(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.from.x + 1 + bs, r.from.y + 1 + bs}}, 0xff101a20u);
					draw_text_rgba(data, pitch, r.from.x + 3 * hud_us(), r.from.y + 3 * hud_us(), lv, cth.amber);
				}
			}
		}
	}

	// Draws a command-card button's text label, centered and wrapped/shrunk to
	// fit the cell. Used for buttons without a cmdicons frame.
	void draw_card_label(uint32_t* data, size_t pitch, rect r, const char* label) {
		int n = (int)strlen(label);
		int cw = usp_adv();
		int avail = r.to.x - r.from.x - 4;
		// Split into up to two lines on the natural width so long names fit.
		int per_line = avail / cw; if (per_line < 1) per_line = 1;
		if (n <= per_line) {
			int x = (r.from.x + r.to.x) / 2 - n * cw / 2;
			int y = (r.from.y + r.to.y) / 2 - usp_h() / 2;
			draw_text_rgba(data, pitch, x, y, label, hud_theme().amber);
		} else {
			// Break near the middle so both lines are balanced.
			int split = n / 2;
			char l1[16], l2[16];
			int a = split < 15 ? split : 15;
			memcpy(l1, label, a); l1[a] = 0;
			int b = (n - split) < 15 ? (n - split) : 15;
			memcpy(l2, label + split, b); l2[b] = 0;
			int y = (r.from.y + r.to.y) / 2 - usp_h() - 1;
			draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - (int)strlen(l1) * cw / 2, y, l1, hud_theme().amber);
			draw_text_rgba(data, pitch, (r.from.x + r.to.x) / 2 - (int)strlen(l2) * cw / 2, y + usp_h() + 2, l2, hud_theme().amber);
		}
	}

	// Cost / stats tooltip for the command-card button under the pointer,
	// drawn as a small panel just above the card grid. Works in both HUDs.
	void draw_card_tooltip(uint32_t* data, size_t pitch) {
		if (!game_input || card_hover_index < 0) return;
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		if (card_hover_index >= (int)buttons.size()) return;
		auto& b = buttons[card_hover_index];
		if (b.action == cardact_none) return; // empty fixed slot has no tooltip
		char l0[40] = {0}, l1[40] = {0}, l2[40] = {0}, l3[48] = {0}, l0buf[40] = {0};
		int mineral = 0, gas = 0, supply = 0, btime = 0;
		const char* name = b.label;
		const char* effect = nullptr; // what a research/upgrade does (SC wiki wording)
		a_string nm;
		switch (b.action) {
		case cardact_train: case cardact_build: case cardact_morph: case cardact_morph_building: case cardact_addon: {
			auto* ut = get_unit_type((UnitTypes)b.arg);
			if (ut) {
				nm = unit_name(ut); name = nm.c_str();
				mineral = ut->mineral_cost; gas = ut->gas_cost;
				supply = (int)ut->supply_required.integer_part();
				btime = ut->build_time;
				const char* req = unit_requirement((UnitTypes)b.arg);
				if (req) snprintf(l3, sizeof l3, "Need: %s", req);
				effect = unit_flavor((UnitTypes)b.arg); // #7/#8: what it's for
			}
			break;
		}
		case cardact_ability: case cardact_ability_target: {
			// #9: name + one-line flavor (area/single, friend/foe) for spells and
			// toggles. Unknown abilities keep their terse card label, no flavor.
			const char* an = ability_name(b.action, b.arg);
			if (an) name = an;
			effect = ability_desc(b.action, b.arg);
			break;
		}
		case cardact_research: {
			auto* t = get_tech_type((TechTypes)b.arg);
			if (t) { mineral = t->mineral_cost; gas = t->gas_cost; btime = t->research_time; }
			const char* on = tech_official_name((TechTypes)b.arg);
			if (on) name = on; // official name instead of the terse card code
			effect = tech_effect((TechTypes)b.arg);
			const char* req = tech_requirement((TechTypes)b.arg);
			if (req) snprintf(l3, sizeof l3, "Need: %s", req);
			break;
		}
		case cardact_upgrade: {
			auto* up = get_upgrade_type((UpgradeTypes)b.arg);
			const char* on = upgrade_official_name((UpgradeTypes)b.arg);
			if (on) name = on; // official name instead of the terse card code
			effect = upgrade_effect((UpgradeTypes)b.arg);
			if (up) {
				// Multi-level upgrades keep appearing until max; show the NEXT
				// level and its scaled cost so it's clear it advanced, not stuck.
				int cur = game_local_player >= 0 ? player_upgrade_level(game_local_player, (UpgradeTypes)b.arg) : 0;
				int maxlv = game_local_player >= 0 ? player_max_upgrade_level(game_local_player, (UpgradeTypes)b.arg) : 1;
				mineral = upgrade_mineral_cost(game_local_player, up);
				gas = upgrade_gas_cost(game_local_player, up);
				btime = upgrade_time_cost(game_local_player, up);
				if (maxlv > 1) { snprintf(l0buf, sizeof l0buf, "%s Lvl %d", on ? on : b.label, cur + 1); name = l0buf; }
			}
			const char* req = upgrade_requirement((UpgradeTypes)b.arg);
			if (req) snprintf(l3, sizeof l3, "Need: %s", req);
			break;
		}
		default: break;
		}
		snprintf(l0, sizeof l0, "%s", name);
		if (mineral || gas) snprintf(l1, sizeof l1, "Min %d  Gas %d", mineral, gas);
		if (supply && btime) snprintf(l2, sizeof l2, "Sup %d   %ds", supply, btime / 24);
		else if (supply) snprintf(l2, sizeof l2, "Sup %d", supply);
		else if (btime) snprintf(l2, sizeof l2, "%ds", btime / 24);

		// Effect line ("what this does") wrapped to at most two lines so the
		// tooltip panel stays compact. Split on the last space at/before col 32.
		char l4[48] = {0}, l5[48] = {0};
		if (effect) {
			int el = (int)strlen(effect);
			if (el <= 32) {
				snprintf(l4, sizeof l4, "%s", effect);
			} else {
				int split = 32;
				while (split > 0 && effect[split] != ' ') --split;
				bool at_space = split > 0 && effect[split] == ' ';
				if (!at_space) split = 32;
				int a = split < 47 ? split : 47;
				memcpy(l4, effect, a); l4[a] = 0;
				snprintf(l5, sizeof l5, "%s", effect + (at_space ? split + 1 : split));
			}
		}

		int cw = usp_adv(), lh = hud_h_px(ui::unispace_h + 3);
		int lines = 1 + (l1[0] ? 1 : 0) + (l2[0] ? 1 : 0) + (l3[0] ? 1 : 0) + (l4[0] ? 1 : 0) + (l5[0] ? 1 : 0);
		int maxlen = (int)strlen(l0);
		if ((int)strlen(l1) > maxlen) maxlen = (int)strlen(l1);
		if ((int)strlen(l2) > maxlen) maxlen = (int)strlen(l2);
		if ((int)strlen(l3) > maxlen) maxlen = (int)strlen(l3);
		if ((int)strlen(l4) > maxlen) maxlen = (int)strlen(l4);
		if ((int)strlen(l5) > maxlen) maxlen = (int)strlen(l5);
		const int tabp = ui_legacy ? 0 : hud_h_px(6);   // v0.125: room for the race tab above the first line
		int pw = maxlen * cw + hud_h_px(12), ph = lines * lh + hud_h_px(8) + tabp; // zoom-locked
		rect card = card_area();
		int px = card.from.x;
		int py = card.from.y - ph - 4;
		if (px + pw > (int)screen_width) px = (int)screen_width - pw - 2;
		if (px < 2) px = 2;
		if (py < 2) py = 2;
		rect panel = {{px, py}, {px + pw, py + ph}};
		fill_rect_rgba(data, pitch, panel, hud_theme().tip_fill);
		line_rectangle_rgba(data, pitch, panel, hud_theme().tip_line);
		draw_hud_tab(data, pitch, panel, 0);
		int ty = py + hud_h_px(4) + tabp;
		int tx = px + hud_h_px(6);
		draw_text_rgba(data, pitch, tx, ty, l0, hud_theme().amber); ty += lh;
		if (l1[0]) { draw_text_rgba(data, pitch, tx, ty, l1, 0xffd8dce0u); ty += lh; }
		if (l2[0]) { draw_text_rgba(data, pitch, tx, ty, l2, 0xffd8dce0u); ty += lh; }
		if (l3[0]) { draw_text_rgba(data, pitch, tx, ty, l3, 0xffe8a850u); ty += lh; }
		// Effect in a distinct cyan so it reads as a description, not a stat.
		if (l4[0]) { draw_text_rgba(data, pitch, tx, ty, l4, 0xff90c8e0u); ty += lh; }
		if (l5[0]) { draw_text_rgba(data, pitch, tx, ty, l5, 0xff90c8e0u); ty += lh; }
	}

	// v0.83.016 (C1): tap-hold on an Equipment icon (Unit Display) shows what
	// that upgrade is -- official name, the owner's current level, and its
	// effect. Reuses the same helpers the command-card tooltip does. The zones
	// (equip_hit) were drawn since v0.82 but never read, so a hold showed
	// nothing; this is the reader.
	void draw_equip_tooltip(uint32_t* data, size_t pitch) {
		if (!game_input || equip_hover_upgrade < 0) return;
		rect slot{}; bool found = false; const weapon_type_t* hov_weapon = nullptr;
		for (auto& e : equip_hit) if (e.upgrade == equip_hover_upgrade) { slot = e.r; hov_weapon = e.weapon; found = true; break; }
		if (!found) return;
		// Faze 4.7: keep the tooltip self-consistent regardless of caller -- scale the
		// glyphs to the FULL HD buffer AND size the panel with the same us factor, or
		// the 2x text overflows a 1x-sized frame (the "text runs outside the box" bug).
		int us = hud_us();
		bool _prev_hst = hud_scale_text; // restore, not force-false: may be called from draw_hud2
		hud_scale_text = true;
		struct g_t { bool* f; bool p; ~g_t() { *f = p; } } _g{&hud_scale_text, _prev_hst};
		UpgradeTypes up = (UpgradeTypes)equip_hover_upgrade;
		char l0[48], l1[32] = {0};
		const char* on = upgrade_official_name(up);
		if (!on && hov_weapon) {
			// Worker weapon (Fusion Cutter / Particle Beam / Spines): no upgrade, so
			// show the weapon's own name (stat_txt via weapons.dat label) + damage,
			// not the meaningless "Upgrade / Not researched" the upgrade path gave.
			load_unit_names();
			a_string wn;
			if (unit_names_ok && hov_weapon->label > 0) {
				try {
					a_string s = unit_names_tbl[(size_t)hov_weapon->label];
					size_t a = 0, b = s.size();
					while (a < b && (unsigned char)s[a] < 0x20) ++a;
					while (b > a && (unsigned char)s[b - 1] < 0x20) --b;
					for (size_t i = a; i < b; ++i) wn += s[i];
				} catch (...) {}
			}
			snprintf(l0, sizeof l0, "%s", wn.empty() ? "Weapon" : wn.c_str());
			int dmg = hov_weapon->damage_amount, factor = hov_weapon->bullet_count;
			if (factor > 1) snprintf(l1, sizeof l1, "Damage: %d x%d", dmg, factor);
			else snprintf(l1, sizeof l1, "Damage: %d", dmg);
		} else {
			snprintf(l0, sizeof l0, "%s", on ? on : "Upgrade");
			if (game_local_player >= 0) {
				int cur = player_upgrade_level(game_local_player, up);
				int maxlv = player_max_upgrade_level(game_local_player, up);
				// English, per user request: this HD/SC:R path showed Lithuanian keys
				// ("Neištirta"/"Lygis:") because L() has no English mapping for them.
				if (maxlv > 1) snprintf(l1, sizeof l1, "Level: %d / %d", cur, maxlv);
				else snprintf(l1, sizeof l1, "%s", cur > 0 ? "Researched" : "Not researched");
			}
		}
		// Effect wrapped to two lines (same split as the card tooltip).
		const char* effect = upgrade_effect(up);
		char l2[48] = {0}, l3[48] = {0};
		if (effect) {
			int el = (int)strlen(effect);
			if (el <= 32) snprintf(l2, sizeof l2, "%s", effect);
			else {
				int split = 32; while (split > 0 && effect[split] != ' ') --split;
				bool at_space = split > 0 && effect[split] == ' '; if (!at_space) split = 32;
				int a = split < 47 ? split : 47; memcpy(l2, effect, a); l2[a] = 0;
				snprintf(l3, sizeof l3, "%s", effect + (at_space ? split + 1 : split));
			}
		}
		// Zoom-locked: cw/lh track the (now zoom-compensated) glyphs so the panel
		// never overflows under pinch-zoom. (us kept for the small px/ty offsets.)
		int cw = usp_adv(), lh = hud_h_px(ui::unispace_h + 3);
		int lines = 1 + (l1[0] ? 1 : 0) + (l2[0] ? 1 : 0) + (l3[0] ? 1 : 0);
		int maxlen = (int)strlen(l0);
		if ((int)strlen(l1) > maxlen) maxlen = (int)strlen(l1);
		if ((int)strlen(l2) > maxlen) maxlen = (int)strlen(l2);
		if ((int)strlen(l3) > maxlen) maxlen = (int)strlen(l3);
		const int tabp = ui_legacy ? 0 : hud_h_px(6);   // v0.125: room for the race tab above the first line
		int pw = maxlen * cw + hud_h_px(12), ph = lines * lh + hud_h_px(8) + tabp;
		int px = slot.from.x, py = slot.from.y - ph - 4;
		if (px + pw > (int)screen_width) px = (int)screen_width - pw - 2;
		if (px < 2) px = 2;
		if (py < 2) py = slot.to.y + 4;   // no room above -> below the icon
		rect panel = {{px, py}, {px + pw, py + ph}};
		fill_rect_rgba(data, pitch, panel, hud_theme().tip_fill);
		line_rectangle_rgba(data, pitch, panel, hud_theme().tip_line);
		draw_hud_tab(data, pitch, panel, 0);
		int ty = py + 4 * us + tabp;
		draw_text_rgba(data, pitch, px + 6 * us, ty, l0, hud_theme().amber); ty += lh;
		if (l1[0]) { draw_text_rgba(data, pitch, px + 6 * us, ty, l1, 0xffd8dce0u); ty += lh; }
		if (l2[0]) { draw_text_rgba(data, pitch, px + 6 * us, ty, l2, 0xff90c8e0u); ty += lh; }
		if (l3[0]) { draw_text_rgba(data, pitch, px + 6 * us, ty, l3, 0xff90c8e0u); ty += lh; }
	}

	// HUD v2: Groups (0-9) horizontal row above the Unit Display + Queue on the
	// right edge (v0.29 item 6). Groups use group_button_area() so draw == hit-test.
	void draw_hud2_side_strips(uint32_t* data, size_t pitch) {
		// v0.64 item 25: control groups are ten discrete buttons rather than one
		// segmented bar, and QUEUE is an eleventh button of the same height at
		// the right end of the row instead of a strip on the screen edge.
		hud_theme_t th = hud_theme();
		for (int n = 0; n < 10; ++n) {
			rect b = group_button_area(n);
			if (b.to.x <= b.from.x) continue;
			// task 8(b)(d): a live group shows [ mini wireframe (always full-life) |
			// member count ] and NO group number; the wireframe + count identify it
			// until it fully clears (prune_control_groups empties it -> back to the
			// numbered empty slot). Count updates as members die / are appended.
			unit_t* first = control_group_first(n);
			int live = control_group_live_count(n);
			bool filled = first && live > 0;
			// v0.125: the group number stays visible when the group is filled (it used
			// to be replaced by the wireframe), and the group whose units are exactly
			// the current selection is shown green -- for every race.
			const bool current = filled && group_is_current(n);
			fill_rect_rgba(data, pitch, b, hud_pressed(b) ? th.btn_pressed : current ? 0xff123a1fu : filled ? th.grp_fill : th.off_fill);
			line_rectangle_rgba(data, pitch, b, current ? 0xff4fd46au : filled ? th.grp_border : th.off_line);
			if (current) line_rectangle_rgba(data, pitch, {{b.from.x + 1, b.from.y + 1}, {b.to.x - 1, b.to.y - 1}}, 0xff4fd46au);
			if (filled) {
				int bh = b.to.y - b.from.y;
				// number | wireframe  count
				const int numw = usp_adv() + hud_h_px(4);
				{
					char nl[2] = { (char)('0' + n), 0 };
					draw_text_rgba(data, pitch, b.from.x + hud_h_px(3), (b.from.y + b.to.y) / 2 - usp_h() / 2, nl, current ? 0xffd6f5ddu : 0xff9098a0u);
					fill_rect_rgba(data, pitch, {{b.from.x + numw, b.from.y + hud_h_px(3)}, {b.from.x + numw + 1, b.to.y - hud_h_px(3)}}, current ? 0xff4fd46au : th.off_line);
				}
				int box = bh - hud_h_px(2); if (box < 6) box = 6;
				if (box > (b.to.x - b.from.x) - numw - 2 * usp_adv()) box = (b.to.x - b.from.x) - numw - 2 * usp_adv(); // leave room for the number and the count
				if (box < 6) box = 6;
				int wx = b.from.x + numw + hud_h_px(2), wy = b.from.y + (bh - box) / 2;
				blit_wireframe_type(data, pitch, first->unit_type, wx, wy, box, wire_palette(first, 100));
				char cnt[4]; snprintf(cnt, sizeof cnt, "%d", live > 99 ? 99 : live);
				int tw = (int)strlen(cnt) * usp_adv();
				int rx = wx + box;
				int cx = (rx + b.to.x) / 2 - tw / 2; if (cx < rx) cx = rx;
				draw_text_rgba(data, pitch, cx, (b.from.y + b.to.y) / 2 - usp_h() / 2, cnt, th.accent);
			} else {
				char lbl[2] = { (char)('0' + n), 0 };
				draw_text_rgba(data, pitch, (b.from.x + b.to.x) / 2 - usp_adv() / 2, (b.from.y + b.to.y) / 2 - usp_h() / 2, lbl, 0xff9098a0u);
			}
		}
		rect q = hud2_queue_strip();
		bool on = order_queued();
		fill_rect_rgba(data, pitch, q, 0xff000000u | ((on ? th.on_fill : hud_pressed(q) ? th.btn_pressed : th.btn_fill) & 0xffffffu));
		for (int t = 0; t < hud2_line_h(); ++t)
			line_rectangle_rgba(data, pitch, {{q.from.x + t, q.from.y + t}, {q.to.x - t, q.to.y - t}}, 0xff000000u | (th.edge & 0xffffffu));
		if (on) { int t = hud2_line_h(); line_rectangle_rgba(data, pitch, {{q.from.x + t, q.from.y + t}, {q.to.x - t, q.to.y - t}}, th.strip_on_line); }
		{
			// Horizontal now that the button is a normal row cell; the label is
			// abbreviated to whatever fits rather than being clipped.
			const char* lbl = (q.to.x - q.from.x) >= hud_h_px(5 * (ui::unispace_w + 1) + 6) ? "SHIFT" : "S";   // it does both Shift jobs now
			int tw = (int)strlen(lbl) * usp_adv();
			draw_text_rgba(data, pitch, (q.from.x + q.to.x) / 2 - tw / 2,
				(q.from.y + q.to.y) / 2 - usp_h() / 2, lbl, on ? th.on_text : th.accent);
		}
	}

	// World-space building-placement overlay, drawn in BOTH HUD styles: the
	// Protoss psi-matrix guide (powered tiles tinted when placing a building
	// that needs power) plus the tile-snapped green/red placement rectangle.
	void draw_placement_overlay(uint32_t* data, size_t pitch) {
		if (!placing_type || game_local_player < 0) return;
		int p = game_local_player;
		auto w2s = [&](xy w) {
			return xy((int)((fp16::integer(w.x - screen_pos.x) * view_scale).integer_part()),
			          (int)((fp16::integer(w.y - screen_pos.y) * view_scale).integer_part()));
		};
		// Psi field: tint powered tiles so the player sees where a powered
		// building may go (Pylon coverage). Only for buildings that need power.
		if (ut_flag(placing_type, unit_type_t::flag_requires_psionic_matrix)) {
			int tw = (int)game_st.map_tile_width, th = (int)game_st.map_tile_height;
			int x0 = screen_pos.x / 32, y0 = screen_pos.y / 32;
			int x1 = (screen_pos.x + (int)view_width) / 32 + 1;
			int y1 = (screen_pos.y + playfield_view_height()) / 32 + 1;
			if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > tw) x1 = tw; if (y1 > th) y1 = th;
			for (int ty = y0; ty < y1; ++ty) for (int tx = x0; tx < x1; ++tx) {
				if (!is_in_psionic_matrix(p, xy(tx * 32 + 16, ty * 32 + 16))) continue;
				xy a = w2s(xy(tx * 32 + 1, ty * 32 + 1)), b = w2s(xy(tx * 32 + 31, ty * 32 + 31));
				rect rr{a, b};
				// Faze 4.7: rr is SCREEN-space (w2s scales by view_scale), so it must
				// be clipped against playfield_bottom_y() (screen px), NOT
				// playfield_view_height() (world units = /view_scale). At FULL HD the
				// latter is half the buffer, which culled the bottom half of the psi
				// field -- only the top of the Pylon power zone rendered.
				if (rr.to.x <= 0 || rr.to.y <= 0 || rr.from.x >= (int)screen_width || rr.from.y >= playfield_bottom_y()) continue;
				// Translucent green wash (~28%) so the terrain shows through, with
				// a 1px tile gap so the powered area reads as a grid, not a slab.
				blend_rect_rgba(data, pitch, rr, 0x2ec96eu, 72);
			}
		}
		if (placing_pos.x >= 0) {
			unit_t* builder = first_own_selected_unit();
			bool ok = builder && can_place_building(builder, p, placing_type, placing_pos, true, true);
			uint32_t color = ok ? 0xff30e030u : 0xffe03030u;
			xy half = xy(placing_type->placement_size.x / 2, placing_type->placement_size.y / 2);
			rect r{w2s(placing_pos - half), w2s(placing_pos + half)};
			if (r.to.x > 0 && r.to.y > 0 && r.from.x < (int)screen_width && r.from.y < (int)screen_height) {
				// v0.67 item 20: a translucent ghost of the building itself, so
				// the player can see WHAT they are placing and how much room it
				// takes, not just an empty box. Drawn from the same wireframe
				// blitter the Unit Display uses (the sprite pipeline needs a
				// real unit, which does not exist yet at placement time), tinted
				// by whether the spot is legal.
				int gw2 = r.to.x - r.from.x, gh2 = r.to.y - r.from.y;
				int side = gw2 < gh2 ? gw2 : gh2;
				if (side > 8) {
					blend_rect_rgba(data, pitch, r, ok ? 0x2ec96eu : 0xc93030u, 40);
					blit_wireframe_type(data, pitch, placing_type,
						r.from.x + (gw2 - side) / 2, r.from.y + (gh2 - side) / 2, side,
						ok ? 0x60ff80u : 0xff6060u);
				}
				// v0.125: one faint line per map tile, so the footprint reads in
				// tiles; a cross when the spot is not legal; the shared 2 px frame.
				{
					const xy o = placing_pos - half;
					const uint32_t gc = ok ? 0x60ff80u : 0xff6060u;
					for (int gx = 32; gx < (int)placing_type->placement_size.x; gx += 32) {
						xy a = w2s(o + xy(gx, 0));
						blend_rect_rgba(data, pitch, {{a.x, r.from.y}, {a.x + mark_e(), r.to.y}}, gc, 110);
					}
					for (int gy = 32; gy < (int)placing_type->placement_size.y; gy += 32) {
						xy a = w2s(o + xy(0, gy));
						blend_rect_rgba(data, pitch, {{r.from.x, a.y}, {r.to.x, a.y + mark_e()}}, gc, 110);
					}
				}
				if (!ok) {
					const int in = mark_t() + mark_e();
					draw_line2(data, pitch, r.from.x + in, r.from.y + in, r.to.x - 1 - in, r.to.y - 1 - in, color);
					draw_line2(data, pitch, r.to.x - 1 - in, r.from.y + in, r.from.x + in, r.to.y - 1 - in, color);
				}
				mark_rect(data, pitch, r, color);
			}
		}
	}

	// #1(b): a translucent footprint of the building each own worker is walking
	// out to construct, drawn at the build site with a faint line from the worker,
	// so a pending/queued build is visible ("it's waiting") before the real
	// building sprite appears. Pure UI -- reads order_target.pos (the site) and
	// build_queue.front() (the type). The ghost self-clears once construction
	// starts and the real sprite takes over.
	void draw_pending_build_shadows(uint32_t* data, size_t pitch) {
		if (game_local_player < 0) return;
		auto w2s = [&](xy w) {
			return xy((int)((fp16::integer(w.x - screen_pos.x) * view_scale).integer_part()),
			          (int)((fp16::integer(w.y - screen_pos.y) * view_scale).integer_part()));
		};
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (!u->sprite || !u->unit_type) continue;
			if (!(unit_is(u, UnitTypes::Terran_SCV) || unit_is(u, UnitTypes::Protoss_Probe) || unit_is(u, UnitTypes::Zerg_Drone))) continue;
			if (u->build_queue.empty()) continue;
			const unit_type_t* bt = u->build_queue.front();
			if (!bt || !ut_building(bt)) continue;
			xy site = u->order_target.pos;
			if (site == xy()) continue;
			xy half = xy(bt->placement_size.x / 2, bt->placement_size.y / 2);
			rect r{w2s(site - half), w2s(site + half)};
			int gw2 = r.to.x - r.from.x, gh2 = r.to.y - r.from.y;
			if (r.to.x <= 0 || r.to.y <= 0 || r.from.x >= (int)screen_width || r.from.y >= playfield_bottom_y()) continue; // screen-space clip -> screen-px bound (FULL HD)
			int side = gw2 < gh2 ? gw2 : gh2;
			if (side <= 8) continue;
			// Dashed line from the worker to the site (which worker builds what);
			// dashed = not there yet.
			xy a = w2s(u->sprite->position), b = w2s(site);
			draw_line2(data, pitch, a.x, a.y, b.x, b.y, 0xc0b040u, true, playfield_bottom_y()); // screen-space clip (FULL HD)
			blend_rect_rgba(data, pitch, r, 0xc0a020u, 32);
			blit_wireframe_type(data, pitch, bt, r.from.x + (gw2 - side) / 2, r.from.y + (gh2 - side) / 2, side, 0xd0b040u);
			mark_rect(data, pitch, r, 0xd0b040u, true, playfield_bottom_y());
		}
		// #1(a): queued builds not yet issued (worker still finishing a prior one)
		// -- a dimmer ghost so the player sees the whole plan.
		for (auto& pb : build_queue_ui) {
			auto* bt = get_unit_type((UnitTypes)pb.type_id);
			if (!bt) continue;
			xy half = xy(bt->placement_size.x / 2, bt->placement_size.y / 2);
			rect r{w2s(pb.pos - half), w2s(pb.pos + half)};
			int gw2 = r.to.x - r.from.x, gh2 = r.to.y - r.from.y;
			if (r.to.x <= 0 || r.to.y <= 0 || r.from.x >= (int)screen_width || r.from.y >= playfield_bottom_y()) continue; // screen-space clip -> screen-px bound (FULL HD)
			int side = gw2 < gh2 ? gw2 : gh2;
			if (side <= 8) continue;
			blend_rect_rgba(data, pitch, r, 0x806018u, 24);
			blit_wireframe_type(data, pitch, bt, r.from.x + (gw2 - side) / 2, r.from.y + (gh2 - side) / 2, side, 0x907828u);
			mark_rect(data, pitch, r, 0xa08830u, true, playfield_bottom_y());
		}
	}

	// v0.72 item 19 (SC2-style convenience, not BW-authentic): remaining
	// resources over patches and geysers, and worker saturation over gas
	// buildings and mineral clusters.
	//
	// The gas cap is fixed at 3 because that is the SC2 convention this feature
	// is borrowed from -- BW has no hard limit, only diminishing returns past
	// about three. Mineral clusters use 2 per patch as the saturation target
	// (BW's practical optimum) and recompute the target as patches mine out.
	static constexpr int gas_worker_cap = 3;
	static constexpr int minerals_per_patch = 2;

	// v0.77 item 5: count workers ASSIGNED to these resources, not workers
	// currently touching them. order_target.unit is the current action, so a
	// worker carrying cargo home targets the base and used to drop off the
	// count, making the number flicker as the cycle went round. The engine
	// keeps the assignment in worker.target_resource_unit, which survives the
	// return trip; that is the number the player actually wants.
	int workers_gathering_from(const a_vector<unit_t*>& targets) {
		if (game_local_player < 0) return 0;
		int n = 0;
		for (unit_t* w : ptr(st.player_units[game_local_player])) {
			if (w->owner != game_local_player) continue;
			auto id = w->unit_type->id;
			if (id != UnitTypes::Terran_SCV && id != UnitTypes::Protoss_Probe && id != UnitTypes::Zerg_Drone) continue;
			unit_t* t = w->worker.target_resource_unit;
			if (!t) continue;
			for (unit_t* r : targets) if (r == t) { ++n; break; }
		}
		return n;
	}

	// v0.82.017: two independent toggles. One switch used to do both jobs --
	// the amount left in a patch AND how many workers are on it -- so a player
	// who wanted one had to accept the other.
	void draw_resource_overlay(uint32_t* data, size_t pitch) {
		if ((!resource_overlay && !saturation_overlay) || game_local_player < 0) return;
		int pfb = playfield_bottom_y();
		auto w2s = [&](xy w) {
			return xy((int)((fp16::integer(w.x - screen_pos.x) * view_scale).integer_part()),
			          (int)((fp16::integer(w.y - screen_pos.y) * view_scale).integer_part()));
		};
		a_vector<unit_t*> patches, gas, halls;
		rect view{screen_pos, screen_pos + xy(view_width, playfield_view_height())};
		int rv = fog_vision_owner();
		for (unit_t* u : find_units(view)) {
			// Only label what is actually on screen: with fog on, a patch you
			// merely explored once is hidden now, so labelling it would leak
			// the same information the sprite pass no longer shows.
			if (rv >= 0 && !(u->sprite->visibility_flags & (1 << rv))) continue;
			if (unit_is_mineral_field(u)) patches.push_back(u);
			else if (unit_is(u, UnitTypes::Resource_Vespene_Geyser)
			         || unit_is(u, UnitTypes::Terran_Refinery)
			         || unit_is(u, UnitTypes::Protoss_Assimilator)
			         || unit_is(u, UnitTypes::Zerg_Extractor)) gas.push_back(u);
			// open1161HD (user request): own town halls anchor the mineral-line
			// saturation badge, so it hangs over the CC/Nexus/Hatch instead of over
			// the patches. Gas stays on its geyser.
			else if (u->owner == game_local_player && u_completed(u)
			         && (unit_is(u, UnitTypes::Terran_Command_Center)
			             || unit_is(u, UnitTypes::Protoss_Nexus)
			             || unit_is(u, UnitTypes::Zerg_Hatchery)
			             || unit_is(u, UnitTypes::Zerg_Lair)
			             || unit_is(u, UnitTypes::Zerg_Hive))) halls.push_back(u);
		}
		// Faze 4.7b (user request): this is a WORLD overlay, so its text + offsets
		// must track the world scale (view_scale). At FULL HD (view_scale=2) the
		// patches are 2x on screen but the label stayed 1x, reading as tiny. No-op
		// at HD2/SD (view_scale=1).
		int vs = hud_us(); // == world/view scale (int); FULL HD = 2, HD2/SD = 1
		auto label = [&](unit_t* u, const char* s, uint32_t col, int dy) {
			xy p = w2s(u->position);
			int tw = (int)strlen(s) * (ui::unispace_w + 1) * vs;
			int x = p.x - tw / 2, y = p.y + dy * vs;
			if (y < 0 || y >= pfb || x < 0 || x + tw >= (int)screen_width) return;
			draw_text_rgba(data, pitch, x, y, s, col, 2 * vs);
		};
		char buf[32];
		if (resource_overlay) for (unit_t* u : patches) {
			snprintf(buf, sizeof(buf), "%d", (int)u->building.resource.resource_count);
			label(u, buf, 0xff9ad0ffu, -22);
		}
		for (unit_t* u : gas) {
			// v0.82.022: nothing at all over a refinery that is still going up.
			// It already carries its own build progress bar, and neither figure
			// is actionable before completion -- nothing can mine yet. v0.82.021
			// only suppressed the amount and left the saturation badge, which is
			// why the overlap survived.
			if (!u_completed(u)) continue;
			if (resource_overlay) {
				snprintf(buf, sizeof(buf), "%d", (int)u->building.resource.resource_count);
				label(u, buf, 0xff70e090u, -26);
			}
			// Gas buildings (ours) also report worker saturation.
			if (saturation_overlay && u->owner == game_local_player
			    && (u->unit_type->flags & unit_type_t::flag_building)) {
				a_vector<unit_t*> one{u};
				int n = workers_gathering_from(one);
				snprintf(buf, sizeof(buf), "%d/%d", n, gas_worker_cap);
				label(u, buf, n >= gas_worker_cap ? 0xffffd060u : 0xffb8c0c8u, -12);
			}
		}
		// Mineral clusters: one badge over the cluster centre.
		if (saturation_overlay && !patches.empty()) {
			a_vector<a_vector<unit_t*>> groups;
			cluster_units(patches, 32 * 6, [](unit_t*) { return true; }, groups);
			for (auto& g : groups) {
				if (g.size() < 2) continue;
				int n = workers_gathering_from(g);
				int cap = (int)g.size() * minerals_per_patch;
				if (n == 0) continue;
				xy c(0, 0);
				for (unit_t* u : g) c += u->position;
				c /= (int)g.size();
				snprintf(buf, sizeof(buf), "%d/%d", n, cap);
				// open1161HD (user request): hang the badge over the OWN town hall
				// that mines this cluster, not over the patches. Pick the nearest own
				// hall within ~16 tiles of the cluster centre; if none (unclaimed
				// cluster / hall off-screen), fall back to the cluster centre.
				xy anchor = c; int anchor_up = 40; int best_d = 32 * 16;
				for (unit_t* h : halls) {
					int d = xy_length(h->position - c);
					if (d < best_d) { best_d = d; anchor = h->position;
						anchor_up = h->unit_type->placement_size.y / 2 + 12; }
				}
				xy p = w2s(anchor);
				// Faze 4.7: the mineral-cluster badge is the most common saturation
				// label; it was the one path my 4.7b change missed, so it stayed 1x
				// (tiny) at FULL HD while the gas/amount labels scaled. Match them.
				int tw = (int)strlen(buf) * (ui::unispace_w + 1) * vs;
				int by = p.y - anchor_up * vs;
				if (by < 0 || by >= pfb) continue;
				uint32_t col = n < cap ? 0xff70e090u : (n == cap ? 0xffffd060u : 0xffff7060u);
				draw_text_rgba(data, pitch, p.x - tw / 2, by, buf, col, 2 * vs);
			}
		}
	}
	bool resource_overlay = true;
	// v0.82.017: worker saturation, split out of resource_overlay.
	bool saturation_overlay = true;
	// v0.82.016: the Equipment display draws its icons desaturated so they read
	// as etched metal. Applied ON TOP of whatever icon pack is active -- doing it
	// only on the paletted path left the slots gold under an HD or user pack,
	// which is the usual case. Off for a colour icon pack that wants its own art.
	bool equip_grey = true;

	void draw_hud_rgba(uint32_t* data, size_t pitch) {
		if (lobby_mode) {
			if (loading_mode) draw_loading_screen(data, pitch);
			else draw_lobby_screen(data, pitch);
		} else if (!loading_cache.px.empty()) {
			loading_cache.release(); // LAN loading screen is over: drop its full-screen cache
		}
		if (!game_input || game_local_player < 0) return;
		// World-space overlays belong to both HUD styles.
		draw_resource_overlay(data, pitch);
		draw_order_queue_lines(data, pitch);
		draw_rally_indicator(data, pitch);
		draw_nuke_indicator(data, pitch);
		draw_pending_build_shadows(data, pitch);
		draw_placement_overlay(data, pitch);
		// HUD v2 (native, responsive) takes over the whole HUD when not legacy.
		if (!ui_legacy) { if (hud_v3) draw_hud3(data, pitch); else draw_hud2(data, pitch); return; }
		draw_console(data, pitch);
		draw_minimap_scaled(data, pitch);
		draw_game_timer(data, pitch);
		draw_fps(data, pitch);
		draw_queue_button(data, pitch);
		draw_zoom_buttons(data, pitch);
		draw_util_buttons(data, pitch);
		int p = game_local_player;
		size_t race = (size_t)st.players[p].race;
		if (race > 2) race = 0;
		int supply_used = st.supply_used[p][race].integer_part();
		int supply_max = st.supply_available[p][race].integer_part();
		if (supply_max > 200) supply_max = 200;
		// Resources sit top-right in BW: mineral / gas / supply, each an optional
		// authentic icon (game\icons.grp) + a color-coded value. Supply turns red
		// when maxed. Icon frame indices are provisional pending tablet bug-pass
		// verification; without the icons the colored values still lay out.
		load_icons();
		const char* mp = icons_ok ? "" : "M ";
		const char* gp = icons_ok ? "" : "G ";
		const char* sp = icons_ok ? "" : "S ";
		char mstr[32], gstr[32], sstr[48];
		snprintf(mstr, sizeof(mstr), "%s%d", mp, st.current_minerals[p]);
		snprintf(gstr, sizeof(gstr), "%s%d", gp, st.current_gas[p]);
		snprintf(sstr, sizeof(sstr), "%s%d/%d", sp, supply_used, supply_max);
		const int IC_MIN = 0, IC_GAS = 1, IC_SUP = 2; // game\icons.grp resource frames
		int adv = ui::unispace_w + 1;
		int icw = icons_ok ? 22 : 0; // approx icon box + gap per segment
		int gap = 16;
		int wm = icw + (int)strlen(mstr) * adv;
		int wg = icw + (int)strlen(gstr) * adv;
		int ws = icw + (int)strlen(sstr) * adv;
		int x = (int)screen_width - (wm + gap + wg + gap + ws) - 10;
		int y = 6;
		uint32_t sup_color = supply_used > supply_max ? 0xffff5050u : 0xffdfe7ef;
		x = draw_res_segment(data, pitch, x, y, icons_ok ? IC_MIN : -1, mstr, 0xff6cc0ff); // mineral cyan
		x += gap;
		x = draw_res_segment(data, pitch, x, y, icons_ok ? IC_GAS : -1, gstr, 0xff5fe07a); // gas green
		x += gap;
		draw_res_segment(data, pitch, x, y, icons_ok ? IC_SUP : -1, sstr, sup_color);

		// (Placement preview + psi field are drawn by draw_placement_overlay,
		// common to both HUD styles.)
		if (placing_type) {
			draw_text_rgba(data, pitch, 8, 26, "LAIKYKITE PIRSTA ANT VIETOS - STATYTI (CANCEL - ATSISAKYTI)", 0xffffd050);
		} else if (attack_armed) {
			draw_text_rgba(data, pitch, 8, 26, "ATTACK: BAKSTELEKITE TAIKINI", 0xffff8050);
		}

		// Selected-unit wireframe portrait (bottom-center).
		draw_wireframe_panel(data, pitch);
		draw_menu_button(data, pitch);
		draw_portrait_slot(data, pitch);
		draw_group_buttons(data, pitch);
		draw_sel_page_tabs(data, pitch);   // SCBW2 unlimited_selection: SC2 sub-card tabs

		// Command card.
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		if (!buttons.empty()) {
			load_cmdbtns();
			for (size_t i = 0; i != buttons.size(); ++i) {
				if (buttons[i].action == cardact_none) continue; // fixed-slot gap
				auto r = card_button_rect((int)i);
				// Authentic tcmdbtns.grp button background over the cell. If the
				// asset is missing we draw nothing and let the console art's own
				// recessed slot show through (also authentic) rather than covering
				// it with a flat fill.
				if (cmdbtns_ok) blit_grp_scaled(data, pitch, cmdbtns_grp, cmdbtns_scratch, cmdicon_rgb, 0, r);
				bool highlight = buttons[i].action == cardact_attack && attack_armed;
				if (highlight) line_rectangle_rgba(data, pitch, r, 0xffffc040);
				int cx = (r.from.x + r.to.x) / 2, cy = (r.from.y + r.to.y) / 2;
				int icon = card_icon_frame(buttons[i].action, buttons[i].arg);
				if (icon >= 0) load_cmdicons();
				if (icon >= 0 && cmdicons_ok) {
					// Authentic unit/building icon, scaled to fill the (large)
					// console button and centered on the icon content. Native-size
					// draw_cmdicon() left the icon tiny and offset to one side here.
					draw_cmdicon_fit(data, pitch, icon, r);
				} else {
					// Generic command (STOP/MOVE/BUILD/...) has no unit icon; keep
					// a short centered label until command->cmdicon frames are mapped.
					int tw = (int)strlen(buttons[i].label) * (ui::unispace_w + 1);
					draw_text_rgba(data, pitch, cx - tw / 2, cy - ui::unispace_h / 2, buttons[i].label, 0xffe8e8e8);
				}
				// Requirement not met: dim the cell (engine would reject).
				if (!buttons[i].available) blend_rect_rgba(data, pitch, r, 0x101418u, 150);
			}
		}
		draw_under_attack_alert(data, pitch);
		draw_trigger_hud(data, pitch);
		draw_card_tooltip(data, pitch);
		draw_equip_tooltip(data, pitch);
		draw_pause_menu(data, pitch);
	}

	// --- Game action submission -----------------------------------------
	// All in-game commands are serialized into the BW action wire format
	// (the same bytes read_action parses). In single player they are
	// executed locally right away; in a network game game_action_submit is
	// set and the bytes go through the lockstep layer, which schedules and
	// executes them on BOTH clients at frame+latency. One code path, so
	// local and network games behave identically.
	std::function<void(const uint8_t*, size_t)> game_action_submit;
	// Optional tap for live replay recording (single-device games; network
	// games record through the sync layer instead).
	std::function<void(const uint8_t*, size_t)> game_action_record;

	struct game_action_writer {
		// 96, not 64: the chat action (92) alone is 1 + 1 + 80 bytes.
		static_vector<uint8_t, 96> buf;
		void u8(uint8_t v) { buf.push_back(v); }
		void u16(uint16_t v) {
			buf.push_back((uint8_t)v);
			buf.push_back((uint8_t)(v >> 8));
		}
		void i16(int16_t v) { u16((uint16_t)v); }
		void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
	};

	void submit_game_action(const game_action_writer& w) {
		if (w.buf.empty()) return;
		const int opcode = w.buf[0];
		if (game_action_submit) {
			game_action_submit(w.buf.data(), w.buf.size());
		} else {
			if (game_action_record) game_action_record(w.buf.data(), w.buf.size());
			data_loading::data_reader_le r(w.buf.data(), w.buf.data() + w.buf.size());
			read_action(game_local_player, r);
		}
		// HUD v3: a one-shot SHIFT ends with the order it was for
		// v0.126.003: ...but NOT with a building (opcode 12). The first building of a
		// SHIFT sequence is sent directly; when that switched a one-tap SHIFT off, the
		// second building was no longer queued and REPLACED the first while the worker
		// was still walking to it. A build sequence keeps SHIFT on until
		// another kind of order or a tap on the key ends it. (Planned builds issued
		// later by process_build_queue come through here too and must not end it.)
		if (opcode == 20 || opcode == 21) hud3_order_sent();
	}

	// Serializes a select action (opcode 9) for the UI selection (own
	// units only, max 12). Returns the number of units, 0 if none.
	size_t submit_select_action() {
		static_vector<unit_t*, 12> sel;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u) continue;
			if (u->owner != game_local_player) continue;
			sel.push_back(u);
			if (sel.size() == 12) break;
		}
		if (sel.empty()) return 0;
		game_action_writer w;
		w.u8(9);
		w.u8((uint8_t)sel.size());
		for (unit_t* u : sel) w.u16(get_unit_id(u).raw_value);
		submit_game_action(w);
		return sel.size();
	}

	// Serializes select for a single specific unit.
	void submit_select_single(unit_t* u) {
		game_action_writer w;
		w.u8(9);
		w.u8(1);
		w.u16(get_unit_id(u).raw_value);
		submit_game_action(w);
	}

	// SCBW2 auto-worker house rule (SP touch-comfort assist). qol.auto_worker is only
	// ever set in single-player (android_main applies qol only when !net && !replay),
	// so the flag being on implies SP. Injects the SAME recorded select+train / morph
	// actions a tapping player would, via the UI submit path (opcode 31 for Terran/
	// Protoss build-queue, opcode 35 morph for a Zerg larva), then restores the human's
	// on-screen selection -- so a saved SP replay reproduces it exactly, like the group-
	// rally fan. Throttled on the deterministic sim frame; saturates the economy to
	// ~16 workers per base, then stops so it never floods the army out.
	void qol_auto_worker_tick() {
		if (!st.game->qol.auto_worker) return;   // feature enabled in settings...
		if (!auto_worker_active) return;         // ...AND the in-game button toggled on (task 2)
		if (!game_input || game_local_player < 0) return;
		if (st.current_frame % 12 != 0) return;   // gentle cadence, not every frame
		int p = game_local_player;
		race_t race = st.players[p].race;
		UnitTypes worker_id = race == race_t::terran ? UnitTypes::Terran_SCV
		                    : race == race_t::protoss ? UnitTypes::Protoss_Probe
		                    : UnitTypes::Zerg_Drone;
		const unit_type_t* worker = get_unit_type(worker_id);
		if (!has_available_supply_for(p, worker)) return;              // no supply room (no error voice)
		if (st.current_minerals[p] < worker->mineral_cost) return;
		int issued = 0;
		if (race == race_t::zerg) {
			// One drone at a time: if a larva is already morphing into a Drone (an
			// egg whose queued morph is the Drone), wait for it to hatch before
			// consuming another larva -- so the auto rule never wipes the larva pool.
			unit_t* larva = nullptr;
			for (unit_t* u : ptr(st.player_units[p])) {
				if (!u->sprite) continue;
				if (unit_is_egg(u) && !u->build_queue.empty() && u->build_queue.front() == worker) return;
				if (!larva && unit_is(u, UnitTypes::Zerg_Larva)) larva = u;
			}
			if (!larva) return;
			submit_select_single(larva);
			game_action_writer w; w.u8(35); w.u16((uint16_t)(int)worker_id); submit_game_action(w); // morph
			++issued;
		} else {
			// Terran/Protoss: keep EVERY base producing -- train one worker at each
			// grounded, completed, idle (empty-queue) depot, so bases run in parallel
			// (one in-progress each). Budget BOTH minerals and supply locally (in fp,
			// units-agnostic) across the loop so we don't spam trains the sim would
			// just reject when several depots share a tight resource pool; the toggle
			// (not a cap) is the stop control.
			int budget = st.current_minerals[p];
			size_t rc = (size_t)race;
			auto sup_room = st.supply_available[p][rc] - st.supply_used[p][rc];
			for (unit_t* u : ptr(st.player_units[p])) {
				if (!u->sprite || !ut_resource_depot(u)) continue;
				if (!u_completed(u) || u_flying(u) || !u->build_queue.empty()) continue;
				if (budget < worker->mineral_cost) break;
				if (sup_room < worker->supply_required) break;
				submit_select_single(u);
				game_action_writer w; w.u8(31); w.u16((uint16_t)(int)worker_id); submit_game_action(w); // train
				budget -= worker->mineral_cost;
				sup_room -= worker->supply_required;
				++issued;
			}
		}
		if (!issued) return;
		// Restore the human's selection in the engine. If they had nothing selected,
		// submit_select_action() emits nothing and the engine would be left selecting
		// our producer -- emit an explicit deselect (select 0) so engine == empty UI.
		if (!submit_select_action()) { game_action_writer d; d.u8(9); d.u8(0); submit_game_action(d); }
		if (bwgame::diag::enabled())
			bwgame::diag::emit((long)st.current_frame, "qol", bwgame::diag::Sev::info, "AUTO_WORKER",
				"auto-queued worker(s)", "\"issued\":" + std::to_string(issued));
	}

	// #5: with several identical production buildings selected, the engine's
	// train/research/upgrade actions only fire for a SINGLE selected unit
	// (action_train/research/upgrade call get_single_selected_unit, which returns
	// null when >1 are selected) -- so a group tap did nothing. Pick the identical
	// building with the shortest build queue (the soonest-free slot); the caller
	// select-singles it, issues the action, then restores the group selection so
	// the UI stays multi-selected and the next tap re-routes fresh.
	unit_t* production_route_target() {
		unit_t* primary = first_own_selected_unit();
		if (!primary) return nullptr;
		unit_t* best = nullptr; size_t best_q = 999; int count = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player) continue;
			if (u->unit_type != primary->unit_type) continue;
			++count;
			size_t q = u->build_queue.size();
			if (q < best_q) { best_q = q; best = u; }
		}
		return count > 1 ? best : nullptr; // null: 0/1 building -> use the normal path
	}
	template<class F>
	bool submit_production_routed(F build_action) {
		// SCBW2 "MBS train" house rule: fan the production order to EVERY identical
		// selected building (SC2 multi-building-select), not just one. Purely UI-side
		// -- each building is select-singled and issued the same valid train/research
		// order, then the group selection is restored -- so no sim-core change and it
		// stays deterministic per the negotiated qol flag. Buildings whose queue is
		// full simply no-op in the sim. Needs multi_building_select to have kept the
		// group; off by default (vanilla routes to a single building below).
		if (st.game->qol.mbs_train) {
			unit_t* primary = first_own_selected_unit();
			if (primary) {
				int issued = 0;
				for (auto uid : current_selection) {
					unit_t* u = get_unit(uid);
					if (!u || u->owner != game_local_player) continue;
					if (u->unit_type != primary->unit_type) continue; // identical buildings only
					submit_select_single(u);
					game_action_writer w; build_action(w); submit_game_action(w);
					++issued;
				}
				if (issued) { submit_select_action(); return true; } // restore the group selection
			}
		}
		if (unit_t* rt = production_route_target()) {
			submit_select_single(rt);
			game_action_writer w; build_action(w); submit_game_action(w);
			submit_select_action(); // restore the multi-selection in the engine
			return true;
		}
		if (submit_select_action()) {
			game_action_writer w; build_action(w); submit_game_action(w);
			return true;
		}
		return false;
	}

	unit_t* first_own_selected_unit() {
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (u && u->owner == game_local_player) return u;
		}
		return nullptr;
	}

	// SCBW2 group-focus: tapping a unit in the multi-selection wireframe grid FOCUSES
	// it -- the command card + portrait switch to that unit so its abilities are
	// usable, while the whole group stays selected (great with smart_cast: focus a
	// High Templar in a zealot+templar ball and Psi Storm still routes to the best
	// caster). Drives ONLY the card/portrait, never the order/primary logic. Valid
	// only while the focused unit is still in the selection; reset on reselect.
	unit_id focused_unit_id{};
	unit_t* focus_or_primary() {
		unit_t* f = get_unit(focused_unit_id);
		if (f && f->owner == game_local_player && current_selection_is_selected(f)) return f;
		return first_own_selected_unit();
	}

	// v0.83.001: the neutral counterpart, for the selection "what" sound.
	// Owner 11 is the map's neutral slot; critters are flagged neutral by the
	// engine's own map loader regardless of the owner byte, so test both.
	unit_t* first_neutral_selected_unit() {
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || !u->unit_type) continue;
			if (u->owner == 11 || unit_is_critter(u->unit_type)) return u;
		}
		return nullptr;
	}

	xy screen_to_world(int mx, int my) {
		xy pos = screen_pos + xy((fp16::integer(mx) / view_scale).integer_part(), (fp16::integer(my) / view_scale).integer_part());
		if (pos.x < 0) pos.x = 0;
		if (pos.y < 0) pos.y = 0;
		if (pos.x >= (int)game_st.map_width) pos.x = (int)game_st.map_width - 1;
		if (pos.y >= (int)game_st.map_height) pos.y = (int)game_st.map_height - 1;
		return pos;
	}

	// True if (mx,my) lands on a HUD control that must ABSORB the touch instead
	// of turning into a world order: command card, equipment icons, production
	// strip, cargo slots, and the queue/zoom/util/group/ping buttons. This is the
	// same zone set that mouse_button_down uses to suppress a drag-select, so a
	// long-press respects exactly the same chrome. The MINIMAP is deliberately
	// EXCLUDED -- a long-press on it is a legitimate "order to that spot".
	// (#10: without this, a hold over the equipment icons fell through to
	// issue_default_order_at and moved the selection to the terrain behind the
	// HUD; the equip tooltip flashed on press, then the order stole it.)
	bool hud_control_at(int mx, int my) {
		if (!game_input) return false;
		if (hud_v3 && hud3_rect_at(mx, my)) return true;   // the v3 elements, 8 dp dead zone (minimap excluded)
		// The bottom console is an opaque, full-width panel: a tap ANYWHERE on it
		// (portrait, unit display, empty strip -- not just an interactive control)
		// must be treated as a HUD hit so no world order is ever issued at the map
		// point hidden behind it. Without this a tap on the portrait fell through
		// and placed a rally/move behind the HUD (user-reported). Controls overlaid
		// ON the playfield (zoom/ping/resource bar, top corners) are still caught by
		// the explicit hit-tests below, which sit above playfield_bottom_y().
		if (my >= playfield_bottom_y()) return true;
		int zd, gn, ud; unit_t* cg = nullptr;
		rect pbd = ui_legacy ? rect{} : hud2_ping_button();
		bool ping_dn = !ui_legacy && mx >= pbd.from.x && mx < pbd.to.x && my >= pbd.from.y && my < pbd.to.y;
		return ping_dn || auto_worker_button_hit(mx, my) || queue_button_hit(mx, my) || zoom_button_hit(mx, my, zd)
		    || util_button_hit(mx, my, ud) || group_button_hit(mx, my, gn)
		    || sel_page_tab_hit_test(mx, my)
		    || cargo_slot_hit(mx, my, cg) || production_slot_hit(mx, my) >= 0
		    || card_hit_test(mx, my) >= 0 || equip_hit_test(mx, my) >= 0
		    || selection_grid_unit_at(mx, my) != nullptr;
	}

	// #11: units that don't fight in the front line -- workers, Medic, transports,
	// detectors, the Science Vessel. On a touchscreen there's no fast micro to pull
	// them out of harm's way, so on a group move/attack-move they trail behind the
	// fighters instead of leading. Deliberately NOT authentic BW. Spellcasters
	// (Templar, Defiler, Queen, Ghost, Arbiter) count as fighters and stay with
	// the army.
	static bool is_support_unit(const unit_t* u) {
		if (!u || !u->unit_type) return false;
		switch (u->unit_type->id) {
		case UnitTypes::Terran_SCV: case UnitTypes::Protoss_Probe: case UnitTypes::Zerg_Drone:
		case UnitTypes::Terran_Medic: case UnitTypes::Terran_Science_Vessel:
		case UnitTypes::Zerg_Overlord: case UnitTypes::Protoss_Observer:
		case UnitTypes::Protoss_Shuttle: case UnitTypes::Terran_Dropship:
			return true;
		default: return false;
		}
	}

	// Issue a ground move (or attack-move) to the selection, sending support units
	// to a point pulled back toward the group's own centroid so they trail the
	// fighters. Returns true if it handled the order (mixed selection); false when
	// the selection is homogeneous, so the caller uses the normal single-order path.
	bool issue_group_move_split(xy target, bool attack, bool queue) {
		static_vector<unit_t*, 12> fighters, support;
		long long sx = 0, sy = 0; int n = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player || !u->sprite) continue;
			sx += u->sprite->position.x; sy += u->sprite->position.y; ++n;
			if (is_support_unit(u)) support.push_back(u); else fighters.push_back(u);
			if (fighters.size() + support.size() >= 12) break;
		}
		if (n == 0 || support.empty() || fighters.empty()) return false; // homogeneous
		xy centroid((int)(sx / n), (int)(sy / n));
		xy d = target - centroid;
		int len = (int)std::sqrt((double)((long long)d.x * d.x + (long long)d.y * d.y));
		xy rear = target;
		if (len > 8) {
			int off = 96; if (off > len / 2) off = len / 2;
			rear = xy(target.x - d.x * off / len, target.y - d.y * off / len);
			rear.x = std::max(0, std::min((int)game_st.map_width - 1, rear.x));
			rear.y = std::max(0, std::min((int)game_st.map_height - 1, rear.y));
		}
		auto emit = [&](static_vector<unit_t*, 12>& set, xy p) {
			game_action_writer s; s.u8(9); s.u8((uint8_t)set.size());
			for (unit_t* u : set) s.u16(get_unit_id(u).raw_value);
			submit_game_action(s);
			game_action_writer w;
			w.u8(attack ? 21 : 20);
			w.i16((int16_t)p.x); w.i16((int16_t)p.y);
			w.u16(0); w.u16((uint16_t)(int)UnitTypes::None);
			if (attack) w.u8((uint8_t)(int)Orders::AttackMove);
			w.u8(queue ? 1 : 0);
			submit_game_action(w);
		};
		emit(fighters, target);
		emit(support, rear);
		submit_select_action(); // restore the full selection in the engine
		return true;
	}

	// SCBW2 group_rally house rule (REPLAY-SAFE): rally every selected building, by
	// fanning a per-building select-single + rally over the UI's current_selection.
	// Each emitted action is a valid single-building select+rally in the recorded
	// stream, so it replays correctly regardless of qol -- unlike the sim-level
	// multi_building_select flag, which changes how a recorded select is interpreted
	// and so would need replay-header persistence (P5). Works even with
	// multi_building_select off, because it drives the UI list, not the sim selection.
	// Returns true if it handled the order (a >1 grounded-building selection).
	// Engages ONLY for a selection of purely own grounded buildings: a MIXED
	// selection (buildings + mobile units) defers to the vanilla path below, which
	// still issues the order to the mobile units -- fanning here would silently drop
	// their move order.
	bool try_group_rally(xy pos, unit_t* target) {
		// open1161HD QoL: DEFAULT-ON for all modes (single/MP/campaign). This is a
		// pure UI-fan -- it re-issues ONE ordinary single-building select+rally action
		// per selected building into the recorded/broadcast command stream (below), so
		// replays and LAN peers stay in sync with no sim flag or header persistence.
		// The old `qol.group_rally` gate (and its SP-only application) is dropped.
		bwgame::static_vector<unit_id, 12> blds;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player) continue;
			bool grounded_building = (u->unit_type->flags & unit_type_t::flag_building) && !u_flying(u);
			if (!grounded_building) return false;   // mixed selection -> vanilla path
			blds.push_back(uid);
		}
		if (blds.size() < 2) return false;
		int order = target ? (int)Orders::RallyPointUnit : (int)Orders::RallyPointTile;
		for (auto uid : blds) {
			unit_t* u = get_unit(uid);
			if (!u) continue;
			submit_select_single(u);
			game_action_writer w;
			w.u8(21); w.i16((int16_t)pos.x); w.i16((int16_t)pos.y);
			w.u16(target ? get_unit_id(target).raw_value : 0);
			w.u16((uint16_t)(int)UnitTypes::None);
			w.u8((uint8_t)order); w.u8(0);
			submit_game_action(w);
		}
		submit_select_action(); // restore the group selection in the engine
		if (bwgame::diag::enabled()) {
			diag_rally_units.clear();
			for (auto uid : blds) diag_rally_units.push_back(uid);
			diag_rally_pos = pos;
			diag_rally_target_id = target ? (int)get_unit_id(target).raw_value : -1;
			diag_rally_check_frame = (int)st.current_frame + 4;
		}
		return true;
	}

	// SCBW2 smart-cast: when an energy spell is cast with >=2 capable casters in the
	// selection, route the cast to the single highest-energy caster instead of the
	// whole group, so the others keep their energy (the SC2 behaviour). Replay-safe:
	// select-single the caster, emit the same order opcode, restore the group -- every
	// emitted action is an ordinary recorded/broadcast action. Applies ONLY to
	// tech spells that cost energy (order->tech_type set, energy_cost>0): Repair, Move,
	// Patrol, spider Mines and Nuke have no energy cost and fall through to the vanilla
	// whole-group order. Returns true if it handled the cast.
	bool try_smart_cast(int order_id, xy pos, unit_t* target) {
		if (!st.game->qol.smart_cast) return false;
		const order_type_t* ot = get_order_type((Orders)order_id);
		if (!ot || ot->tech_type == TechTypes::None) {
			if (bwgame::diag::enabled()) bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::warn,
				"SMART_CAST_SKIP", "order is not a tech spell", "\"order\":" + std::to_string(order_id) + ",\"reason\":\"no_tech\"");
			return false;
		}
		const tech_type_t* tech = get_tech_type(ot->tech_type);
		if (!tech || tech->energy_cost <= 0) {   // energy spells only
			if (bwgame::diag::enabled()) bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::warn,
				"SMART_CAST_SKIP", "tech has no energy cost", "\"order\":" + std::to_string(order_id) +
				",\"tech\":" + std::to_string((int)ot->tech_type) + ",\"reason\":\"no_energy_cost\"");
			return false;
		}
		unit_t* best = nullptr; int casters = 0;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player) continue;
			if (!unit_can_use_tech(u, tech)) continue;       // right caster + researched
			++casters;
			if (!best || u->energy > best->energy) best = u;
		}
		// <2 capable casters: the vanilla whole-group order already casts only the one
		// capable unit (the rest no-op on the tech check), so nothing is wasted -- leave
		// it to the normal path.
		if (!best || casters < 2) {
			if (bwgame::diag::enabled()) bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::warn,
				"SMART_CAST_SKIP", "fewer than 2 capable casters selected",
				"\"casters\":" + std::to_string(casters) + ",\"reason\":\"casters<2\"");
			return false;
		}
		if (bwgame::diag::enabled()) bwgame::diag::emit((long)st.current_frame, "cast", bwgame::diag::Sev::info,
			"SMART_CAST_FIRE", "routed cast to the best-energy caster",
			"\"casters\":" + std::to_string(casters) + ",\"best\":" + std::to_string((int)get_unit_id(best).raw_value));
		submit_select_single(best);
		game_action_writer w;
		w.u8(21); // order
		w.i16((int16_t)pos.x); w.i16((int16_t)pos.y);
		w.u16(target ? get_unit_id(target).raw_value : 0);
		w.u16((uint16_t)(int)UnitTypes::None);
		w.u8((uint8_t)order_id); w.u8(order_queued() ? 1 : 0);
		submit_game_action(w);
		submit_select_action();   // restore the group selection in the engine
		return true;
	}

	void issue_default_order_at(int mx, int my) {
		if (!game_input || game_local_player < 0) return;
		if (hud_control_at(mx, my)) return;
		xy pos;
		unit_t* target = nullptr;
		auto minimap = get_minimap_area();
		if (mx >= minimap.from.x && mx < minimap.to.x && my >= minimap.from.y && my < minimap.to.y) {
			// Long-press on the minimap: order to that spot.
			pos = minimap_to_map(mx, my);
		} else {
			pos = screen_to_world(mx, my);
			target = select_get_unit_at(pos);
		}

		// SCBW2 unlimited_selection: when the wide set is bigger than the active
		// sub-card, fan the contextual order over the WHOLE set (<=12 batches). Uses the
		// same opcode the vanilla path below would (21 rally for a grounded-building
		// primary, else 20 default order). Takes precedence over the <=12 finesse paths
		// (group-move split, group-rally, smart-cast) which only see the active page.
		if (unlimited_sel_on() && wide_selection.size() > current_selection.size()) {
			bool q = order_queued();
			// Split the wide set by unit class so each gets the RIGHT opcode even in a
			// mixed selection: grounded buildings take a rally order (21), everything else
			// the contextual default order (20). Fanning each list in <=12 batches avoids
			// sending a rally-tile to mobile units (or a move to a building).
			a_vector<unit_id> blds, mob;
			for (auto uid : wide_selection) {
				unit_t* u = get_unit(uid); if (!u) continue;
				bool is_b = (u->unit_type->flags & unit_type_t::flag_building) && !u_flying(u);
				(is_b ? blds : mob).push_back(uid);
			}
			a_vector<unit_id> saved; saved.swap(current_selection);
			auto fan = [&](a_vector<unit_id>& list, bool building) {
				for (size_t i = 0; i < list.size(); i += 12) {
					size_t j = i + 12 < list.size() ? i + 12 : list.size();
					current_selection.assign(list.begin() + i, list.begin() + j);
					if (!submit_select_action()) continue;
					game_action_writer w;
					if (building) {
						w.u8(21); w.i16((int16_t)pos.x); w.i16((int16_t)pos.y);
						w.u16(target ? get_unit_id(target).raw_value : 0);
						w.u16((uint16_t)(int)UnitTypes::None);
						w.u8((uint8_t)(int)(target ? Orders::RallyPointUnit : Orders::RallyPointTile)); w.u8(0);
					} else {
						w.u8(20); w.i16((int16_t)pos.x); w.i16((int16_t)pos.y);
						w.u16(target ? get_unit_id(target).raw_value : 0);
						w.u16((uint16_t)(int)UnitTypes::None); w.u8(q ? 1 : 0);
					}
					submit_game_action(w);
				}
			};
			fan(mob, false);
			fan(blds, true);
			current_selection.swap(saved);
			submit_select_action();   // restore engine selection to the active sub-card
			unit_t* primary = first_own_selected_unit();
			if (primary && !((primary->unit_type->flags & unit_type_t::flag_building) && !u_flying(primary)))
				play_unit_yes(primary);
			return;
		}

		// #11: a plain group move to open ground (no target unit) sends support
		// units to the rear. Only for mobile selections; a building selection sets
		// a rally point below, and an order onto a unit (attack/gather) is left
		// intact. Done before the group select so its own subset selects apply.
		{
			unit_t* p0 = first_own_selected_unit();
			bool mobile = p0 && !((p0->unit_type->flags & unit_type_t::flag_building) && !u_flying(p0));
			if (mobile && !target && issue_group_move_split(pos, false, order_queued())) {
				play_unit_yes(p0);
				return;
			}
		}

		// SCBW2 group rally: fan the rally over all selected buildings (replay-safe).
		// Silent, like the vanilla single-building rally below (buildings don't ack).
		if (try_group_rally(pos, target)) return;

		if (!submit_select_action()) return;

		// A production building's long-press sets its rally point instead of a
		// (pointless) move order; trained units then head to that spot/unit.
		unit_t* primary = first_own_selected_unit();
		// A lifted Terran building flies like an air unit, so it takes a real
		// move order (opcode 20) rather than a rally point.
		bool is_building = primary && (primary->unit_type->flags & unit_type_t::flag_building) && !u_flying(primary);

		game_action_writer w;
		if (is_building) {
			w.u8(21); // issue order
			w.i16((int16_t)pos.x);
			w.i16((int16_t)pos.y);
			w.u16(target ? get_unit_id(target).raw_value : 0);
			w.u16((uint16_t)(int)UnitTypes::None);
			w.u8((uint8_t)(int)(target ? Orders::RallyPointUnit : Orders::RallyPointTile));
			w.u8(0); // not queued
			// DIAG: remember which own buildings we just asked to rally, to verify
			// a few sim frames later that ALL of them (not just the primary) got it.
			if (bwgame::diag::enabled()) {
				diag_rally_units.clear();
				for (auto uid : current_selection) {
					unit_t* su = get_unit(uid);
					if (su && su->owner == game_local_player
					    && (su->unit_type->flags & unit_type_t::flag_building) && !u_flying(su))
						diag_rally_units.push_back(uid);
				}
				diag_rally_pos = pos;
				diag_rally_target_id = target ? (int)get_unit_id(target).raw_value : -1;
				diag_rally_check_frame = (int)st.current_frame + 3;
			}
		} else {
			w.u8(20); // default order (right click)
			w.i16((int16_t)pos.x);
			w.i16((int16_t)pos.y);
			w.u16(target ? get_unit_id(target).raw_value : 0);
			w.u16((uint16_t)(int)UnitTypes::None);
			w.u8(order_queued() ? 1 : 0); // queue if queue-mode/S-Pen button
		}
		submit_game_action(w);
		if (!is_building) play_unit_yes(primary); // "yes sir" acknowledgment
	}

	// --- Command card -------------------------------------------------
	// A contextual 3x3 button grid (bottom right). Content depends on the
	// primary selected unit; buttons are text labels rendered with the
	// embedded font (authentic cmdicons.grp icons are a later polish).

	struct card_button_t {
		const char* label;
		int action; // cardact_*
		int arg;    // UnitTypes / Orders value, depending on action
		// Requirement gate (v0.26): false = tech prerequisites not met, so the
		// engine would reject this order. Drawn dimmed. Computed by
		// apply_card_availability() after the grid is filled. Cost/supply is a
		// separate concern (buttons stay lit but their cost text can go red).
		bool available = true;
	};
	enum {
		cardact_none, cardact_stop, cardact_attack, cardact_menu_build,
		cardact_menu_adv, cardact_menu_back, cardact_train, cardact_build,
		cardact_morph, cardact_morph_building, cardact_select_larva,
		cardact_place_cancel, cardact_place_confirm,
		// v0.13 ability system:
		cardact_ability,        // arg = action opcode; no target (toggles/casts on selection)
		cardact_ability_target, // arg = Orders value; armed target order (spell/patrol/land)
		cardact_liftoff,        // opcode 47 issued at the building's own position
		cardact_research,       // arg = TechTypes value
		cardact_upgrade,        // arg = UpgradeTypes value
		cardact_cancel,         // arg = cancel opcode (24/25/32/46/49/51/52)
		cardact_addon,          // arg = addon UnitTypes; auto-placed beside the building
		cardact_land,           // enter placement mode to land a lifted building (opcode 12)
		cardact_nydus_exit      // v0.83.034: place a Nydus Canal exit (opcode 12, BuildNydusExit)
	};

	int card_menu = 0; // 0 = context, 1 = build basic, 2 = build advanced
	// task 3: which build submenu (1/2) to reopen after a placement when QUEUE is
	// on, so the player can compose a build order without re-navigating BUILD->list.
	int build_menu_return = 0;
	bool attack_armed = false;
	// When >= 0, the next map/minimap tap issues order(21) with this Orders
	// value at the tapped target (armed spell / patrol / land, like attack).
	int ability_armed_order = -1;
	// Command-card button currently under the pointer (stylus hover or a
	// press-hold) — drives the cost/requirement tooltip. -1 = none.
	int card_hover_index = -1;
	const unit_type_t* placing_type = nullptr;
	const order_type_t* placing_order = nullptr;
	xy placing_pos{-1, -1};
	// When true, placement lands the selected lifted building (the building is
	// its own "builder"); otherwise a worker constructs a new building.
	bool placing_land = false;
	// v0.83.034: like placing_land, the selected unit is its own builder, but
	// the order is a normal build (Nydus Canal placing its exit) rather than a
	// liftoff landing -- so the afford check still applies.
	bool placing_self = false;

	// #1(a): UI-side build queue. When queue-mode is on and the chosen worker is
	// already busy building, the placement is stored here instead of interrupting
	// the current build; process_build_queue() auto-issues each pending build (via
	// the normal opcode-12 action) once that worker is free. This is determinism-
	// safe: it is automated INPUT through the ordinary action stream (identical to
	// a human queuing), and the engine still afford-checks at issue time (place_
	// building aborts + last_error if broke). Ghosts for pending builds are drawn
	// by draw_pending_build_shadows.
	struct pending_build_t {
		unit_id worker; int type_id; xy pos; const order_type_t* order;
		int blocked_since = -1;   // frame the spot was first found blocked when its turn came (-1: not blocked)
	};
	a_vector<pending_build_t> build_queue_ui;

	// True if this worker is currently occupied with a build (walking to a site or
	// constructing), so another build should queue behind it rather than interrupt.
	bool worker_busy_building(const unit_t* u) {
		if (!u) return false;
		if (!u->build_queue.empty()) return true;
		switch (u->order_type->id) {
		case Orders::PlaceBuilding: case Orders::ConstructingBuilding:
		case Orders::PlaceProtossBuilding: case Orders::CreateProtossBuilding:
		case Orders::DroneStartBuild: case Orders::DroneBuild: case Orders::BuildNydusExit:
			return true;
		default: return false;
		}
	}

	// Auto-issue queued builds whose worker has become free, and prune dead/lost
	// entries. Called once per update().
	void process_build_queue() {
		if (build_queue_ui.empty() || game_local_player < 0) return;
		// Track which workers already got an order issued this pass, so we issue at
		// most one build per free worker per frame (keeps ordering stable).
		for (size_t i = 0; i < build_queue_ui.size();) {
			auto& pb = build_queue_ui[i];
			unit_t* w = get_unit(pb.worker);
			auto* bt = get_unit_type((UnitTypes)pb.type_id);
			if (!w || w->owner != game_local_player || unit_dead(w) || !bt) {
				build_queue_ui.erase(build_queue_ui.begin() + i); continue; // worker gone
			}
			// v0.126.002: a Drone that became its building is still a living unit of
			// ours, but no longer a worker -- its plan ends there (the build order used
			// to be sent to the morphing structure).
			if (!(unit_is(w, UnitTypes::Terran_SCV) || unit_is(w, UnitTypes::Protoss_Probe) || unit_is(w, UnitTypes::Zerg_Drone))) {
				build_queue_ui.erase(build_queue_ui.begin() + i); continue;
			}
			if (worker_busy_building(w)) { ++i; continue; } // still busy -> keep waiting
			// Worker is free: issue this build now (engine validates spot + afford).
			if (!can_place_building(w, game_local_player, bt, pb.pos, false, false)) {
				// v0.126.002: a blocked spot is usually a unit walking through (a worker
				// on its way to the minerals, the builder itself stepping off the last
				// site). The plan used to be DROPPED the instant that happened -- the
				// building silently vanished from the sequence. Wait for the spot to
				// clear; give up only if it stays blocked for ten seconds.
				if (pb.blocked_since < 0) pb.blocked_since = (int)st.current_frame;
				if ((int)st.current_frame - pb.blocked_since < 24 * 10) { ++i; continue; }
				show_hud_message(L("Planned building dropped: the spot is blocked."));
				build_queue_ui.erase(build_queue_ui.begin() + i); continue; // spot lost
			}
			submit_select_single(w);
			xy_t<size_t> tile;
			tile.x = (size_t)std::max(0, pb.pos.x - bt->placement_size.x / 2) / 32u;
			tile.y = (size_t)std::max(0, pb.pos.y - bt->placement_size.y / 2) / 32u;
			game_action_writer w2;
			w2.u8(12); // build
			w2.u8((uint8_t)(int)pb.order->id);
			w2.u16((uint16_t)tile.x);
			w2.u16((uint16_t)tile.y);
			w2.u16((uint16_t)pb.type_id);
			submit_game_action(w2);
			build_queue_ui.erase(build_queue_ui.begin() + i);
		}
	}

	static constexpr int card_cols = 3;
	static constexpr int card_rows = 3;
	// Grid geometry in console space (tconsole.pcx is 640x480), mapped to the
	// screen with the same non-uniform scale draw_console uses, so the 3x3
	// command buttons line up with the console's command slot. Tunable.
	static constexpr float card_cs_x0 = 503.0f;  // grid top-left, console space
	static constexpr float card_cs_y0 = 359.0f;
	static constexpr float card_cs_btn = 40.0f;  // button size, console space
	static constexpr float card_cs_gap = 2.0f;

	rect card_cell_console(int col, int row) {
		float x = card_cs_x0 + col * (card_cs_btn + card_cs_gap);
		float y = card_cs_y0 + row * (card_cs_btn + card_cs_gap);
		xy a = console_to_screen(x, y);
		return {a, {a.x + console_len(card_cs_btn), a.y + console_len(card_cs_btn)}};
	}

	rect card_area() {
		// Uses card_button_rect so it is correct in both the legacy and HUD v2
		// layouts (both branch inside card_button_rect).
		rect a = card_button_rect(0);
		rect b = card_button_rect(card_cols * card_rows - 1);
		return {{a.from.x, a.from.y}, {b.to.x, b.to.y}};
	}

	rect card_button_rect(int index) {
		if (!ui_legacy) return hud2_card_cell(index);
		return card_cell_console(index % card_cols, index / card_cols);
	}

	// Fill the 3x3 command grid for the current selection, then mark which
	// buttons the engine would actually accept (tech prerequisites met) so the
	// UI can dim the rest. The engine remains the authority — it silently
	// rejects illegal orders — this only mirrors that state visually.
	void card_buttons(static_vector<card_button_t, 9>& r) {
		fill_card_buttons(r);
		place_card_buttons(r);
		apply_card_availability(r);
	}

	// v0.66 item 13: pin buttons to fixed grid slots.
	//
	// The original is not packed: rez/statbtn<race>.bin is a dialog template
	// with nine fixed controls (verified), which
	// is why real command cards have visible gaps. We were packing in discovery
	// order, so a button's position moved whenever an ability appeared or a tech
	// finished, and the generic tail (STOP/ATTACK/HOLD/PATROL) was appended last
	// and silently dropped by the `r.size() < 9` guards -- an ability-rich unit
	// could lose its Attack button entirely.
	//
	// Layout: row 1 is Move/Stop/Attack for anything mobile; workers get
	// Repair/Build/Advanced in row 2; other units get their abilities from slot
	// 3 on, active before passive; slot 8 belongs to Cancel, then Lift Off/Land.
	//
	// v0.122: measured with `loadscreen_uicheck card` -- six commands could not be
	// reached at all because slot 8 was kept empty for a Cancel that those units
	// never have, and Cancel itself lost slot 8 to Lift Off:
	//   - Cancel takes slot 8 and Lift Off / Land slot 7 (a second Cancel, e.g.
	//     the addon's, falls back to 7, 6) -- a producing Terran building keeps both;
	//   - everything else may use slot 8 when no Cancel-type button took it
	//     (the larva's ninth morph, the Queen's and the Defiler's fourth spell);
	//   - a worker's free slot 3 (no Repair on a Probe / Drone) is used, and
	//     Return Cargo is placed before Hold / Patrol, which the original's
	//     worker card does not have at all.
	void place_card_buttons(static_vector<card_button_t, 9>& r) {
		if (r.empty()) return;
		card_button_t grid[9] = {};
		bool used[9] = {};
		auto put = [&](int slot, const card_button_t& b) {
			if (slot < 0 || slot > 8 || used[slot]) return false;
			grid[slot] = b; used[slot] = true; return true;
		};
		auto is = [&](const card_button_t& b, int act, int arg) {
			return b.action == act && b.arg == arg;
		};

		static_vector<card_button_t, 9> rest;
		bool worker = false, mobile = false;
		for (auto& b : r) {
			if (b.action == cardact_menu_build || b.action == cardact_menu_adv) worker = true;
			if (b.action == cardact_stop) mobile = true;
		}
		for (auto& b : r) {
			if (is(b, cardact_ability_target, (int)Orders::Move)) { put(0, b); continue; }
			if (b.action == cardact_stop) { put(1, b); continue; }
			if (b.action == cardact_attack) { put(2, b); continue; }
			if (b.action == cardact_cancel || b.action == cardact_place_cancel
			    || b.action == cardact_liftoff || b.action == cardact_land
			    || b.action == cardact_menu_back) continue;   // placed from the end, below
			if (worker) {
				if (is(b, cardact_ability_target, (int)Orders::Repair)) { put(3, b); continue; }
				if (b.action == cardact_menu_build) { put(4, b); continue; }
				if (b.action == cardact_menu_adv) { put(5, b); continue; }
			} else if (mobile) {
				if (is(b, cardact_ability, 43)) { put(3, b); continue; }                       // Hold
				if (is(b, cardact_ability_target, (int)Orders::Patrol)) { put(4, b); continue; } // Patrol
			}
			rest.push_back(b);
		}
		// Passive entries (research/upgrade) go last, as requested.
		auto passive = [](const card_button_t& b) {
			return b.action == cardact_research || b.action == cardact_upgrade;
		};
		// Cancel-type buttons from the last slot backwards: Cancel first, so it is
		// never the one that gets pushed out.
		for (int pass = 0; pass != 2; ++pass) {
			for (auto& b : r) {
				const bool cancel = b.action == cardact_cancel || b.action == cardact_place_cancel || b.action == cardact_menu_back;
				const bool lift = b.action == cardact_liftoff || b.action == cardact_land;
				if (pass == 0 ? !cancel : !lift) continue;
				// Lift Off / Land always sit in slot 7, so they do not jump when a
				// Cancel appears next to them.
				for (int k = pass == 0 ? 8 : 7; k >= 6; --k) if (put(k, b)) break;
			}
		}
		if (worker) {
			// Return Cargo ahead of the abilities and of Hold / Patrol
			std::stable_partition(rest.begin(), rest.end(), [&](const card_button_t& b) { return is(b, cardact_ability, 30); });
		}
		int slot = mobile ? 3 : 0;
		for (int pass = 0; pass != 2; ++pass) {
			for (auto& b : rest) {
				if ((pass == 1) != passive(b)) continue;
				while (slot < 9 && used[slot]) ++slot;
				if (slot >= 9) break;
				put(slot++, b);
			}
		}
		r.clear();
		for (int i = 0; i != 9; ++i) {
			if (used[i]) r.push_back(grid[i]);
			else r.push_back({"", cardact_none, 0});
		}
	}

	void apply_card_availability(static_vector<card_button_t, 9>& r) {
		if (game_local_player < 0) return;
		unit_t* u = first_own_selected_unit();
		if (!u) return;
		int owner = u->owner;
		for (auto& b : r) {
			switch (b.action) {
			case cardact_build:
				b.available = unit_can_build(u, get_unit_type((UnitTypes)b.arg));
				// #6: QUEUE build-order. unit_can_build() returns false for a Terran SCV
				// mid-ConstructingBuilding (bwgame.h), so its build buttons greyed out
				// while it worked -- the player couldn't line up the next building. With
				// QUEUE on, keep a busy worker's build buttons tappable; the tap queues
				// via build_queue_ui and issues once the worker is free. Only bites for
				// Terran (a Protoss probe is free after warp-in, a Zerg drone is consumed
				// -> neither is a busy, still-selected worker).
				if (!b.available && queue_mode && worker_busy_building(u))
					b.available = true;
				break;
			case cardact_train:
			case cardact_morph:
			case cardact_morph_building:
			case cardact_addon:
				b.available = unit_can_build(u, get_unit_type((UnitTypes)b.arg));
				break;
			case cardact_research:
				b.available = unit_can_research(u, get_tech_type((TechTypes)b.arg), owner);
				break;
			case cardact_upgrade:
				b.available = unit_can_upgrade(u, get_upgrade_type((UpgradeTypes)b.arg), owner);
				break;
			default:
				break;
			}
		}
	}

	// v0.83.033: the base combat type whose command card a HERO unit should
	// present. Hero units (Sarah Kerrigan, Jim Raynor, Edmund Duke, Tassadar, ...)
	// have their own UnitTypes ids distinct from the base unit, so the ability
	// switch in fill_card_buttons -- keyed on the raw id -- never matched them and
	// every hero showed only Move/Stop/Attack. This folds a hero onto its base so
	// it inherits that unit's ability buttons. It reuses the engine's own
	// hero-folding predicates (unit_is_ghost() etc. already include the hero
	// variants); the few base types that have no such predicate are matched by id.
	// A hero with no special base card returns its own id and falls through to the
	// generic row, unchanged.
	UnitTypes card_hero_base_type(const unit_t* u) {
		auto* ut = u->unit_type;
		if (unit_is_ghost(ut)) return UnitTypes::Terran_Ghost;
		if (unit_is_vulture(ut)) return UnitTypes::Terran_Vulture;
		if (unit_is_wraith(ut)) return UnitTypes::Terran_Wraith;
		if (unit_is_marine(ut)) return UnitTypes::Terran_Marine;
		if (unit_is_firebat(ut)) return UnitTypes::Terran_Firebat;
		if (unit_is_sieged_tank(ut)) return UnitTypes::Terran_Siege_Tank_Siege_Mode;
		if (unit_is_unsieged_tank(ut)) return UnitTypes::Terran_Siege_Tank_Tank_Mode;
		if (unit_is_defiler(ut)) return UnitTypes::Zerg_Defiler;
		if (unit_is_queen(ut)) return UnitTypes::Zerg_Queen;
		if (unit_is_arbiter(ut)) return UnitTypes::Protoss_Arbiter;
		if (unit_is_carrier(ut)) return UnitTypes::Protoss_Carrier;
		if (unit_is_reaver(ut)) return UnitTypes::Protoss_Reaver;
		switch (ut->id) {
		case UnitTypes::Hero_Arcturus_Mengsk:
		case UnitTypes::Hero_Hyperion:
		case UnitTypes::Hero_Norad_II:
		case UnitTypes::Hero_Gerard_DuGalle:
			return UnitTypes::Terran_Battlecruiser;
		case UnitTypes::Hero_Magellan:
			return UnitTypes::Terran_Science_Vessel;
		case UnitTypes::Hero_Yggdrasill:
			return UnitTypes::Zerg_Overlord;
		case UnitTypes::Hero_Tassadar:
		case UnitTypes::Hero_Aldaris:
			return UnitTypes::Protoss_High_Templar;
		case UnitTypes::Hero_Raszagal:
			return UnitTypes::Protoss_Corsair;
		default:
			return ut->id;
		}
	}

	void fill_card_buttons(static_vector<card_button_t, 9>& r) {
		r.clear();
		if (placing_type) {
			// Placement is confirmed by long-pressing the target tile (like
			// issuing a unit order), so only a cancel button is needed.
			r.push_back({"CANCEL", cardact_place_cancel, 0});
			return;
		}
		unit_t* u = focus_or_primary();   // SCBW2 group-focus: a grid-tapped unit's card
		if (!u) return;
		auto ut = u->unit_type->id;
		bool worker = ut == UnitTypes::Terran_SCV || ut == UnitTypes::Protoss_Probe || ut == UnitTypes::Zerg_Drone;
		// v0.83.033: fold a hero onto its base combat type so it presents that
		// unit's abilities (see card_hero_base_type). Buildings are never heroes,
		// so this leaves the build/train/upgrade switches below untouched.
		if (ut_hero(u->unit_type)) ut = card_hero_base_type(u);

		// v0.122: a morphing egg / cocoon takes no orders but can be cancelled
		// (opcode 25 -> cancel_building_unit refunds and restores the unit). It
		// used to show Move / Stop / Attack / Hold / Patrol, none of which do
		// anything, and no way to cancel.
		if (ut == UnitTypes::Zerg_Egg || ut == UnitTypes::Zerg_Lurker_Egg || ut == UnitTypes::Zerg_Cocoon) {
			r.push_back({"CANCEL", cardact_cancel, 25});
			return;
		}

		if (card_menu != 0 && worker) {
			// Build menus. The engine validates cost/tech, so unavailable
			// buildings simply do nothing when tapped.
			auto add = [&](const char* label, UnitTypes t) {
				if (r.size() < 8) r.push_back({label, cardact_build, (int)t});
			};
			if (ut == UnitTypes::Terran_SCV) {
				if (card_menu == 1) {
					add("CC", UnitTypes::Terran_Command_Center);
					add("DEPOT", UnitTypes::Terran_Supply_Depot);
					add("REFINERY", UnitTypes::Terran_Refinery);
					add("BARRACKS", UnitTypes::Terran_Barracks);
					add("EBAY", UnitTypes::Terran_Engineering_Bay);
					add("TURRET", UnitTypes::Terran_Missile_Turret);
					add("ACADEMY", UnitTypes::Terran_Academy);
					add("BUNKER", UnitTypes::Terran_Bunker);
				} else {
					add("FACTORY", UnitTypes::Terran_Factory);
					add("STARPORT", UnitTypes::Terran_Starport);
					add("SCIENCE", UnitTypes::Terran_Science_Facility);
					add("ARMORY", UnitTypes::Terran_Armory);
				}
			} else if (ut == UnitTypes::Protoss_Probe) {
				if (card_menu == 1) {
					add("NEXUS", UnitTypes::Protoss_Nexus);
					add("PYLON", UnitTypes::Protoss_Pylon);
					add("ASSIM", UnitTypes::Protoss_Assimilator);
					add("GATEWAY", UnitTypes::Protoss_Gateway);
					add("FORGE", UnitTypes::Protoss_Forge);
					add("CANNON", UnitTypes::Protoss_Photon_Cannon);
					add("CYBER", UnitTypes::Protoss_Cybernetics_Core);
					add("BATTERY", UnitTypes::Protoss_Shield_Battery);
				} else {
					add("ROBOTICS", UnitTypes::Protoss_Robotics_Facility);
					add("STARGATE", UnitTypes::Protoss_Stargate);
					add("CITADEL", UnitTypes::Protoss_Citadel_of_Adun);
					add("ARCHIVES", UnitTypes::Protoss_Templar_Archives);
					add("OBSERV", UnitTypes::Protoss_Observatory);
					add("BEACON", UnitTypes::Protoss_Fleet_Beacon);
					add("TRIBUNAL", UnitTypes::Protoss_Arbiter_Tribunal);
					add("SUPPORT", UnitTypes::Protoss_Robotics_Support_Bay);
				}
			} else {
				if (card_menu == 1) {
					add("HATCHERY", UnitTypes::Zerg_Hatchery);
					add("EXTRACTOR", UnitTypes::Zerg_Extractor);
					add("POOL", UnitTypes::Zerg_Spawning_Pool);
					add("EVO", UnitTypes::Zerg_Evolution_Chamber);
					add("COLONY", UnitTypes::Zerg_Creep_Colony);
					add("HYDRADEN", UnitTypes::Zerg_Hydralisk_Den);
				} else {
					add("SPIRE", UnitTypes::Zerg_Spire);
					add("QUEENNEST", UnitTypes::Zerg_Queens_Nest);
					add("DEFMOUND", UnitTypes::Zerg_Defiler_Mound);
					add("ULTRACAV", UnitTypes::Zerg_Ultralisk_Cavern);
					add("NYDUS", UnitTypes::Zerg_Nydus_Canal);
				}
			}
			r.push_back({"BACK", cardact_menu_back, 0});
			return;
		}

		auto train = [&](const char* label, UnitTypes t) {
			if (r.size() < 9) r.push_back({label, cardact_train, (int)t});
		};
		auto morph = [&](const char* label, UnitTypes t) {
			if (r.size() < 9) r.push_back({label, cardact_morph, (int)t});
		};
		auto ability = [&](const char* label, int opcode) {
			if (r.size() < 9) r.push_back({label, cardact_ability, opcode});
		};
		auto researched = [&](TechTypes t) {
			// v0.83.033: a hero wields its abilities without the tech being
			// researched -- the engine agrees (unit_can_use_tech bypasses the tech
			// requirement for ut_hero), so mirror that here or a hero's tech-gated
			// spells (Lockdown, Cloak, Yamato, Psi Storm, ...) would stay hidden.
			if (ut_hero(u->unit_type)) return true;
			return game_local_player >= 0 && player_has_researched(game_local_player, t);
		};
		auto research = [&](const char* label, TechTypes t) {
			// Drop a tech that is already researched or currently being researched —
			// it must not linger in the grid (progress shows in the Unit Display).
			if (game_local_player >= 0 && (player_has_researched(game_local_player, t) || player_is_researching(game_local_player, t))) return;
			if (r.size() < 9) r.push_back({label, cardact_research, (int)t});
		};
		auto upgrade = [&](const char* label, UpgradeTypes t) {
			// Drop an upgrade already at its maximum level or currently upgrading
			// (multi-level upgrades stay until the last tier is reached).
			if (game_local_player >= 0 && (player_upgrade_level(game_local_player, t) >= player_max_upgrade_level(game_local_player, t) || player_is_upgrading(game_local_player, t))) return;
			if (r.size() < 9) r.push_back({label, cardact_upgrade, (int)t});
		};
		auto spell = [&](const char* label, Orders o) {
			if (r.size() < 9) r.push_back({label, cardact_ability_target, (int)o});
		};

		switch (ut) {
		case UnitTypes::Terran_Command_Center:
			train("SCV", UnitTypes::Terran_SCV);
			break;
		case UnitTypes::Terran_Nuclear_Silo:
			// Build a nuke (the Ghost's NUKE spell has nothing to launch without
			// this). The silo is a selectable addon; engine validates prereqs.
			train("NUKE", UnitTypes::Terran_Nuclear_Missile);
			break;
		case UnitTypes::Zerg_Infested_Command_Center:
			train("INFESTED", UnitTypes::Zerg_Infested_Terran);
			break;
		case UnitTypes::Terran_Barracks:
			train("MARINE", UnitTypes::Terran_Marine);
			train("FIREBAT", UnitTypes::Terran_Firebat);
			train("MEDIC", UnitTypes::Terran_Medic);
			train("GHOST", UnitTypes::Terran_Ghost);
			break;
		case UnitTypes::Terran_Factory:
			train("VULTURE", UnitTypes::Terran_Vulture);
			train("TANK", UnitTypes::Terran_Siege_Tank_Tank_Mode);
			train("GOLIATH", UnitTypes::Terran_Goliath);
			break;
		case UnitTypes::Terran_Starport:
			train("WRAITH", UnitTypes::Terran_Wraith);
			train("DROPSHIP", UnitTypes::Terran_Dropship);
			train("VESSEL", UnitTypes::Terran_Science_Vessel);
			train("VALKYRIE", UnitTypes::Terran_Valkyrie);
			train("BCRUISER", UnitTypes::Terran_Battlecruiser);
			break;
		case UnitTypes::Protoss_Nexus:
			train("PROBE", UnitTypes::Protoss_Probe);
			break;
		case UnitTypes::Protoss_Gateway:
			train("ZEALOT", UnitTypes::Protoss_Zealot);
			train("DRAGOON", UnitTypes::Protoss_Dragoon);
			train("TEMPLAR", UnitTypes::Protoss_High_Templar);
			train("DARK-T", UnitTypes::Protoss_Dark_Templar);
			break;
		case UnitTypes::Protoss_Robotics_Facility:
			train("SHUTTLE", UnitTypes::Protoss_Shuttle);
			train("REAVER", UnitTypes::Protoss_Reaver);
			train("OBSERVER", UnitTypes::Protoss_Observer);
			break;
		case UnitTypes::Protoss_Stargate:
			train("SCOUT", UnitTypes::Protoss_Scout);
			train("CORSAIR", UnitTypes::Protoss_Corsair);
			train("CARRIER", UnitTypes::Protoss_Carrier);
			train("ARBITER", UnitTypes::Protoss_Arbiter);
			break;
		case UnitTypes::Zerg_Larva:
			morph("DRONE", UnitTypes::Zerg_Drone);
			morph("OVERLORD", UnitTypes::Zerg_Overlord);
			morph("ZERGLING", UnitTypes::Zerg_Zergling);
			morph("HYDRA", UnitTypes::Zerg_Hydralisk);
			morph("MUTA", UnitTypes::Zerg_Mutalisk);
			morph("SCOURGE", UnitTypes::Zerg_Scourge);
			morph("QUEEN", UnitTypes::Zerg_Queen);
			morph("ULTRA", UnitTypes::Zerg_Ultralisk);
			morph("DEFILER", UnitTypes::Zerg_Defiler);
			break;
		case UnitTypes::Zerg_Hatchery:
			r.push_back({"LARVA", cardact_select_larva, 0});
			r.push_back({"LAIR", cardact_morph_building, (int)UnitTypes::Zerg_Lair});
			break;
		case UnitTypes::Zerg_Lair:
			r.push_back({"LARVA", cardact_select_larva, 0});
			r.push_back({"HIVE", cardact_morph_building, (int)UnitTypes::Zerg_Hive});
			break;
		case UnitTypes::Zerg_Hive:
			r.push_back({"LARVA", cardact_select_larva, 0});
			break;
		case UnitTypes::Zerg_Creep_Colony:
			r.push_back({"SUNKEN", cardact_morph_building, (int)UnitTypes::Zerg_Sunken_Colony});
			r.push_back({"SPORE", cardact_morph_building, (int)UnitTypes::Zerg_Spore_Colony});
			break;
		case UnitTypes::Zerg_Spire:
			// v0.83.034: Spire -> Greater Spire, parallel to Hatchery->Lair and
			// Lair->Hive. Composes with the Spire upgrade case below (FLY-WPN/
			// FLY-ARM stay). The engine validates the Hive prerequisite.
			r.push_back({"GSPIRE", cardact_morph_building, (int)UnitTypes::Zerg_Greater_Spire});
			break;
		case UnitTypes::Zerg_Nydus_Canal:
			// v0.83.034: build the canal's exit (a second, linked Nydus). Only
			// while this canal has no exit yet -- the engine (order_BuildNydusExit)
			// links exactly one. Placed on any creep via the placement flow.
			if (!u->building.nydus.exit)
				r.push_back({"EXIT", cardact_nydus_exit, 0});
			break;
		default:
			break;
		}

		// Research / upgrade buttons per tech building. Composes with the train
		// switch above (e.g. Lair keeps LARVA + adds overlord upgrades). The
		// engine validates prerequisites/resources, so buttons stay simple.
		switch (ut) {
		// --- Terran ---
		case UnitTypes::Terran_Academy:
			research("STIM", TechTypes::Stim_Packs);
			upgrade("U-238", UpgradeTypes::U_238_Shells);
			research("RESTOR", TechTypes::Restoration);
			research("OPTICAL", TechTypes::Optical_Flare);
			upgrade("CADUCEUS", UpgradeTypes::Caduceus_Reactor);
			break;
		case UnitTypes::Terran_Engineering_Bay:
			upgrade("INF-WPN", UpgradeTypes::Terran_Infantry_Weapons);
			upgrade("INF-ARM", UpgradeTypes::Terran_Infantry_Armor);
			break;
		case UnitTypes::Terran_Armory:
			upgrade("VEH-WPN", UpgradeTypes::Terran_Vehicle_Weapons);
			upgrade("VEH-ARM", UpgradeTypes::Terran_Vehicle_Plating);
			upgrade("SHP-WPN", UpgradeTypes::Terran_Ship_Weapons);
			upgrade("SHP-ARM", UpgradeTypes::Terran_Ship_Plating);
			break;
		case UnitTypes::Terran_Machine_Shop:
			research("SIEGE", TechTypes::Tank_Siege_Mode);
			research("MINES", TechTypes::Spider_Mines);
			upgrade("ION", UpgradeTypes::Ion_Thrusters);
			upgrade("CHARON", UpgradeTypes::Charon_Boosters);
			break;
		case UnitTypes::Terran_Control_Tower:
			research("CLOAK", TechTypes::Cloaking_Field);
			upgrade("APOLLO", UpgradeTypes::Apollo_Reactor);
			break;
		case UnitTypes::Terran_Science_Facility:
			research("EMP", TechTypes::EMP_Shockwave);
			research("IRRAD", TechTypes::Irradiate);
			upgrade("TITAN", UpgradeTypes::Titan_Reactor);
			break;
		case UnitTypes::Terran_Physics_Lab:
			research("YAMATO", TechTypes::Yamato_Gun);
			upgrade("COLOSSUS", UpgradeTypes::Colossus_Reactor);
			break;
		case UnitTypes::Terran_Covert_Ops:
			research("CLOAK", TechTypes::Personnel_Cloaking);
			research("LOCKDOWN", TechTypes::Lockdown);
			upgrade("OCULAR", UpgradeTypes::Ocular_Implants);
			upgrade("MOEBIUS", UpgradeTypes::Moebius_Reactor);
			break;
		// --- Protoss ---
		case UnitTypes::Protoss_Forge:
			upgrade("GND-WPN", UpgradeTypes::Protoss_Ground_Weapons);
			upgrade("GND-ARM", UpgradeTypes::Protoss_Ground_Armor);
			upgrade("SHIELDS", UpgradeTypes::Protoss_Plasma_Shields);
			break;
		case UnitTypes::Protoss_Cybernetics_Core:
			upgrade("AIR-WPN", UpgradeTypes::Protoss_Air_Weapons);
			upgrade("AIR-ARM", UpgradeTypes::Protoss_Air_Armor);
			upgrade("SINGUL", UpgradeTypes::Singularity_Charge);
			break;
		case UnitTypes::Protoss_Citadel_of_Adun:
			upgrade("LEGS", UpgradeTypes::Leg_Enhancements);
			break;
		case UnitTypes::Protoss_Templar_Archives:
			research("STORM", TechTypes::Psionic_Storm);
			research("HALLUC", TechTypes::Hallucination);
			upgrade("AMULET", UpgradeTypes::Khaydarin_Amulet);
			research("MINDCTL", TechTypes::Mind_Control);
			research("MAELSTR", TechTypes::Maelstrom);
			break;
		case UnitTypes::Protoss_Robotics_Support_Bay:
			upgrade("SCARAB", UpgradeTypes::Scarab_Damage);
			upgrade("CAPACITY", UpgradeTypes::Reaver_Capacity);
			upgrade("DRIVE", UpgradeTypes::Gravitic_Drive);
			break;
		case UnitTypes::Protoss_Observatory:
			upgrade("SENSOR", UpgradeTypes::Sensor_Array);
			upgrade("BOOSTERS", UpgradeTypes::Gravitic_Boosters);
			break;
		case UnitTypes::Protoss_Fleet_Beacon:
			upgrade("CARRIER", UpgradeTypes::Carrier_Capacity);
			research("DISRUPT", TechTypes::Disruption_Web);
			upgrade("APIAL", UpgradeTypes::Apial_Sensors);
			upgrade("THRUST", UpgradeTypes::Gravitic_Thrusters);
			break;
		case UnitTypes::Protoss_Arbiter_Tribunal:
			research("RECALL", TechTypes::Recall);
			research("STASIS", TechTypes::Stasis_Field);
			upgrade("CORE", UpgradeTypes::Khaydarin_Core);
			break;
		// --- Zerg ---
		case UnitTypes::Zerg_Evolution_Chamber:
			upgrade("MELEE", UpgradeTypes::Zerg_Melee_Attacks);
			upgrade("MISSILE", UpgradeTypes::Zerg_Missile_Attacks);
			upgrade("CARAPACE", UpgradeTypes::Zerg_Carapace);
			break;
		case UnitTypes::Zerg_Spawning_Pool:
			upgrade("METABOL", UpgradeTypes::Metabolic_Boost);
			upgrade("ADRENAL", UpgradeTypes::Adrenal_Glands);
			break;
		case UnitTypes::Zerg_Hydralisk_Den:
			upgrade("MUSCULAR", UpgradeTypes::Muscular_Augments);
			upgrade("GROOVED", UpgradeTypes::Grooved_Spines);
			research("LURKER", TechTypes::Lurker_Aspect);
			break;
		case UnitTypes::Zerg_Spire:
		case UnitTypes::Zerg_Greater_Spire:
			upgrade("FLY-WPN", UpgradeTypes::Zerg_Flyer_Attacks);
			upgrade("FLY-ARM", UpgradeTypes::Zerg_Flyer_Carapace);
			break;
		case UnitTypes::Zerg_Queens_Nest:
			research("ENSNARE", TechTypes::Ensnare);
			research("BROOD", TechTypes::Spawn_Broodlings);
			upgrade("GAMETE", UpgradeTypes::Gamete_Meiosis);
			break;
		case UnitTypes::Zerg_Defiler_Mound:
			research("PLAGUE", TechTypes::Plague);
			research("CONSUME", TechTypes::Consume);
			upgrade("METASYN", UpgradeTypes::Metasynaptic_Node);
			break;
		case UnitTypes::Zerg_Ultralisk_Cavern:
			upgrade("ANABOLIC", UpgradeTypes::Anabolic_Synthesis);
			upgrade("CHITIN", UpgradeTypes::Chitinous_Plating);
			break;
		case UnitTypes::Zerg_Hatchery:
			// v0.122: Burrowing had no research button anywhere, so the BURROW
			// ability of the Zerg ground units could never be unlocked by a player.
			research("BURROW", TechTypes::Burrowing);
			break;
		case UnitTypes::Zerg_Lair:
		case UnitTypes::Zerg_Hive:
			research("BURROW", TechTypes::Burrowing);
			upgrade("VENTRAL", UpgradeTypes::Ventral_Sacs);
			upgrade("ANTENNAE", UpgradeTypes::Antennae);
			upgrade("PNEUMA", UpgradeTypes::Pneumatized_Carapace);
			break;
		default:
			break;
		}

		// Terran addon construction (only when no addon is attached yet). The
		// engine auto-places the addon beside the building; we pass the addon
		// tile derived from the building position.
		if (ut_building(u) && !u->building.addon) {
			auto addon = [&](const char* label, UnitTypes t) {
				if (r.size() < 9) r.push_back({label, cardact_addon, (int)t});
			};
			switch (ut) {
			case UnitTypes::Terran_Command_Center:
				addon("COMSAT", UnitTypes::Terran_Comsat_Station);
				addon("SILO", UnitTypes::Terran_Nuclear_Silo);
				break;
			case UnitTypes::Terran_Factory:
				addon("MACHSHOP", UnitTypes::Terran_Machine_Shop);
				break;
			case UnitTypes::Terran_Starport:
				addon("CTRLTOWER", UnitTypes::Terran_Control_Tower);
				break;
			case UnitTypes::Terran_Science_Facility:
				addon("PHYSICS", UnitTypes::Terran_Physics_Lab);
				addon("COVERTOPS", UnitTypes::Terran_Covert_Ops);
				break;
			default:
				break;
			}
		}

		// Building-cast abilities (the per-unit ability switch below runs only for
		// non-buildings, so addon buildings that cast need their own path).
		if (ut == UnitTypes::Terran_Comsat_Station) spell("SCAN", Orders::CastScannerSweep);

		// Terran buildings that can lift off / land (the engine validates the
		// exact type and whether the addon/upgrade state permits it).
		if (ut_building(u)) {
			switch (ut) {
			case UnitTypes::Terran_Command_Center:
			case UnitTypes::Terran_Barracks:
			case UnitTypes::Terran_Factory:
			case UnitTypes::Terran_Starport:
			case UnitTypes::Terran_Science_Facility:
				// (Engineering Bay cannot lift off in Brood War - removed.)
				if (u_flying(u)) {
					// Landing snaps a building preview to the tile grid and is
					// confirmed by long-press (opcode 12/build with BuildingLand),
					// exactly like construction — a raw order(21) is rejected by
					// the engine for BuildingLand.
					if (r.size() < 9) r.push_back({placing_land ? "LAND!" : "LAND", cardact_land, 0});
				} else {
					if (r.size() < 9) r.push_back({"LIFTOFF", cardact_liftoff, 0});
				}
				break;
			default:
				break;
			}
			// Cancel whatever the building is currently producing / teching.
			// cancel_research(49)/upgrade(51)/build_queue(32) match the active
			// task; the engine validates the rest.
			if (u->building.researching_type) {
				if (r.size() < 9) r.push_back({"CANCEL", cardact_cancel, 49});
			} else if (u->building.upgrading_type) {
				if (r.size() < 9) r.push_back({"CANCEL", cardact_cancel, 51});
			} else if (!u->build_queue.empty()) {
				if (r.size() < 9) r.push_back({"CANCEL", cardact_cancel, 32});
			} else if (!u_completed(u)) {
				// Building still under construction, or morphing to a higher tier
				// (Hatchery->Lair, Creep->Sunken/Spore): cancel to recover
				// resources. Opcode 24/25 both call the same engine cancel; 25
				// covers the Zerg morph. Previously a misclicked Lair was
				// unrecoverable.
				if (r.size() < 9) r.push_back({"CANCEL", cardact_cancel, unit_race(u) == race_t::zerg ? 25 : 24});
			}
			// An addon still under construction can be cancelled from its parent.
			if (u->building.addon && !u_completed(u->building.addon)) {
				if (r.size() < 9) r.push_back({"CANCEL+", cardact_cancel, 52});
			}
			// A loaded Bunker gets Unload-All (individual passengers can also be
			// tapped in the cargo strip). Its rally point is set by long-press,
			// like other buildings, so unloaded units have a gather spot.
			if (unit_is(u, UnitTypes::Terran_Bunker) && !loaded_units(u).empty()) {
				if (r.size() < 9) r.push_back({"UNLOAD", cardact_ability, 40});
			}
		}

		// Mobile units get generic commands plus per-type abilities. The engine
		// validates tech/energy/state, so a tapped ability simply no-ops when
		// unavailable; we still gate the researched toggles for authentic UX.
		if (!ut_building(u)) {
			switch (ut) {
			case UnitTypes::Terran_Siege_Tank_Tank_Mode:
				ability("SIEGE", 38);
				break;
			case UnitTypes::Terran_Siege_Tank_Siege_Mode:
				ability("UNSIEGE", 37);
				break;
			case UnitTypes::Terran_SCV:
				spell("REPAIR", Orders::Repair); // also reachable via long-press
				break;
			case UnitTypes::Terran_Vulture:
				if (researched(TechTypes::Spider_Mines)) spell("MINES", Orders::PlaceMine);
				break;
			case UnitTypes::Terran_Battlecruiser:
				if (researched(TechTypes::Yamato_Gun)) spell("YAMATO", Orders::FireYamatoGun);
				break;
			case UnitTypes::Terran_Marine:
			case UnitTypes::Terran_Firebat:
				if (researched(TechTypes::Stim_Packs)) ability("STIM", 54);
				break;
			case UnitTypes::Terran_Wraith:
				if (researched(TechTypes::Cloaking_Field))
					ability(u_cloaked(u) ? "DECLOAK" : "CLOAK", u_cloaked(u) ? 34 : 33);
				break;
			case UnitTypes::Terran_Ghost:
				if (researched(TechTypes::Personnel_Cloaking))
					ability(u_cloaked(u) ? "DECLOAK" : "CLOAK", u_cloaked(u) ? 34 : 33);
				if (researched(TechTypes::Lockdown)) spell("LOCKDOWN", Orders::CastLockdown);
				// Nuclear strike (needs a completed nuke in a silo; engine validates).
				spell("NUKE", Orders::CastNuclearStrike);
				// v0.83: abort a strike already in flight. Opcode 46 is NOT the
				// silo's build cancel (that is 32) -- action_cancel_nuke() only
				// touches a Ghost holding a Nuclear Missile in connected_unit,
				// i.e. one currently painting the target. So the button belongs
				// here, and only while that link exists.
				if (u->connected_unit
				    && unit_is(u->connected_unit, UnitTypes::Terran_Nuclear_Missile)) {
					if (r.size() < 9) r.push_back({"ABORT", cardact_cancel, 46});
				}
				break;
			case UnitTypes::Terran_Science_Vessel:
				spell("MATRIX", Orders::CastDefensiveMatrix);
				if (researched(TechTypes::EMP_Shockwave)) spell("EMP", Orders::CastEMPShockwave);
				if (researched(TechTypes::Irradiate)) spell("IRRADIATE", Orders::CastIrradiate);
				break;
			case UnitTypes::Terran_Medic:
				if (researched(TechTypes::Restoration)) spell("RESTORE", Orders::CastRestoration);
				if (researched(TechTypes::Optical_Flare)) spell("OPTICAL", Orders::CastOpticalFlare);
				break;
			case UnitTypes::Terran_Dropship:
			case UnitTypes::Protoss_Shuttle:
			case UnitTypes::Zerg_Overlord:
				ability("UNLOAD", 40);
				break;
			case UnitTypes::Protoss_High_Templar:
				if (researched(TechTypes::Psionic_Storm)) spell("STORM", Orders::CastPsionicStorm);
				if (researched(TechTypes::Hallucination)) spell("HALLUC", Orders::CastHallucination);
				ability("ARCHON", 42); // merge two templar into an Archon
				break;
			case UnitTypes::Protoss_Dark_Templar:
				ability("D-ARCHON", 90); // merge two dark templar into a Dark Archon
				break;
			case UnitTypes::Protoss_Arbiter:
				if (researched(TechTypes::Recall)) spell("RECALL", Orders::CastRecall);
				if (researched(TechTypes::Stasis_Field)) spell("STASIS", Orders::CastStasisField);
				break;
			case UnitTypes::Protoss_Corsair:
				if (researched(TechTypes::Disruption_Web)) spell("D-WEB", Orders::CastDisruptionWeb);
				break;
			case UnitTypes::Protoss_Carrier:
				ability("INTER", 39); // build Interceptor (train_fighter)
				break;
			case UnitTypes::Protoss_Reaver:
				ability("SCARAB", 39); // build Scarab (train_fighter)
				break;
			case UnitTypes::Protoss_Dark_Archon:
				if (researched(TechTypes::Mind_Control)) spell("MINDCTL", Orders::CastMindControl);
				spell("FEEDBACK", Orders::CastFeedback);
				if (researched(TechTypes::Maelstrom)) spell("MAELSTRM", Orders::CastMaelstrom);
				break;
			case UnitTypes::Zerg_Defiler:
				if (researched(TechTypes::Burrowing))
					ability(u_burrowed(u) ? "UNBURROW" : "BURROW", u_burrowed(u) ? 45 : 44);
				spell("DARKSWRM", Orders::CastDarkSwarm);
				if (researched(TechTypes::Plague)) spell("PLAGUE", Orders::CastPlague);
				spell("CONSUME", Orders::CastConsume);
				break;
			case UnitTypes::Zerg_Queen:
				if (researched(TechTypes::Ensnare)) spell("ENSNARE", Orders::CastEnsnare);
				if (researched(TechTypes::Spawn_Broodlings)) spell("BROODLING", Orders::CastSpawnBroodlings);
				spell("PARASITE", Orders::CastParasite);
				spell("INFEST", Orders::CastInfestation);
				break;
			case UnitTypes::Hero_Infested_Kerrigan:
				// Infested Kerrigan's kit spans THREE base casters -- Psi Storm
				// (Templar), Consume (Defiler), Ensnare (Queen) -- so card_hero_base_type
				// cannot fold her onto a single base; she keeps her own unit id and is
				// wired here directly. The engine authorises exactly these three
				// (unit_can_use_tech: Psionic_Storm/Consume/Ensnare all include
				// Hero_Infested_Kerrigan; she has NO cloak toggle -- she is passively
				// cloaked, and unit_is_ghost is false for her, which is authentic).
				spell("STORM", Orders::CastPsionicStorm);
				spell("CONSUME", Orders::CastConsume);
				spell("ENSNARE", Orders::CastEnsnare);
				break;
			case UnitTypes::Zerg_Hydralisk:
				if (researched(TechTypes::Lurker_Aspect)) morph("LURKER", UnitTypes::Zerg_Lurker);
				if (researched(TechTypes::Burrowing))
					ability(u_burrowed(u) ? "UNBURROW" : "BURROW", u_burrowed(u) ? 45 : 44);
				break;
			case UnitTypes::Zerg_Mutalisk:
				// Greater Spire gates these; apply_card_availability greys them out
				// (via unit_can_build) until it exists.
				morph("GUARDIAN", UnitTypes::Zerg_Guardian);
				morph("DEVOURER", UnitTypes::Zerg_Devourer);
				break;
			case UnitTypes::Zerg_Zergling:
			case UnitTypes::Zerg_Drone:
			case UnitTypes::Zerg_Infested_Terran:
				if (researched(TechTypes::Burrowing))
					ability(u_burrowed(u) ? "UNBURROW" : "BURROW", u_burrowed(u) ? 45 : 44);
				break;
			case UnitTypes::Zerg_Lurker:
				ability(u_burrowed(u) ? "UNBURROW" : "BURROW", u_burrowed(u) ? 45 : 44);
				break;
			default:
				break;
			}
			// v0.66: MOVE exists as a button now. The original's top row is
			// Move/Stop/Attack, and place_card_buttons() pins it there; on touch
			// it is also a genuine alternative to long-press for players who
			// find the hold gesture awkward.
			if (r.size() < 9) r.push_back({ability_armed_order == (int)Orders::Move ? "MOVE!" : "MOVE",
				cardact_ability_target, (int)Orders::Move});
			if (r.size() < 9) r.push_back({"STOP", cardact_stop, 0});
			if (r.size() < 9) r.push_back({attack_armed ? "ATTACK!" : "ATTACK", cardact_attack, 0});
			if (r.size() < 9) r.push_back({"HOLD", cardact_ability, 43});
			if (r.size() < 9) r.push_back({ability_armed_order == (int)Orders::Patrol ? "PATROL!" : "PATROL",
				cardact_ability_target, (int)Orders::Patrol});
			if (worker) {
				// v0.83: Return Cargo. Opcode 30 was already accepted by
				// submit_ability() and already had its cmdicon (233) mapped --
				// only the button was missing, so there was no way to issue it.
				// The original shows it only while the worker is carrying, which
				// is also the only time the engine does anything with it.
				if (u->carrying_flags & 3) {
					if (r.size() < 9) r.push_back({"RETURN", cardact_ability, 30});
				}
				if (r.size() < 9) r.push_back({"BUILD", cardact_menu_build, 0});
				if (r.size() < 9) r.push_back({"ADVANCED", cardact_menu_adv, 0});
			}
		}
	}

	// Returns the button index at (mx,my), or -1. Only meaningful in game
	// mode with a non-empty card.
	int card_hit_test(int mx, int my) {
		if (!game_input) return -1;
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		if (buttons.empty()) return -1;
		for (size_t i = 0; i != buttons.size(); ++i) {
			if (buttons[i].action == cardact_none) continue; // empty slot: not tappable
			auto r = card_button_rect((int)i);
			if (mx >= r.from.x && mx < r.to.x && my >= r.from.y && my < r.to.y) return (int)i;
		}
		return -1;
	}

	void card_activate(int index) {
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		if (index < 0 || (size_t)index >= buttons.size()) return;
		auto& b = buttons[index];
		if (b.action == cardact_none) return; // empty fixed slot
		// Requirement not met (tech prerequisites): the engine would reject this
		// order, so treat the tap as a no-op instead of, e.g., entering an
		// unbuildable placement mode. Non-gated buttons default to available.
		if (!b.available) return;
		switch (b.action) {
		case cardact_stop:
			if (submit_select_action()) {
				game_action_writer w;
				w.u8(26); // stop
				w.u8(0);  // not queued
				submit_game_action(w);
			}
			attack_armed = false;
			break;
		case cardact_attack:
			attack_armed = !attack_armed;
			break;
		case cardact_menu_build:
			card_menu = 1;
			break;
		case cardact_menu_adv:
			card_menu = 2;
			break;
		case cardact_menu_back:
			card_menu = 0;
			break;
		case cardact_train:
			if (!check_afford_unit(get_unit_type((UnitTypes)b.arg))) break;
			submit_production_routed([&](game_action_writer& w) {
				w.u8(31); // train
				w.u16((uint16_t)b.arg);
			});
			break;
		case cardact_morph:
			if (!check_afford_unit(get_unit_type((UnitTypes)b.arg))) break;
			if (submit_select_action()) {
				game_action_writer w;
				w.u8(35); // unit morph
				w.u16((uint16_t)b.arg);
				submit_game_action(w);
			}
			break;
		case cardact_morph_building:
			if (!check_afford_unit(get_unit_type((UnitTypes)b.arg))) break;
			if (submit_select_action()) {
				game_action_writer w;
				w.u8(53); // building morph
				w.u16((uint16_t)b.arg);
				submit_game_action(w);
			}
			break;
		case cardact_select_larva: {
			// Select ALL larvae attached to this hatchery/lair/hive, so a whole
			// batch can be morphed at once (previously only the first was picked).
			unit_t* hatch = first_own_selected_unit();
			if (hatch) {
				bool cleared = false;
				for (unit_t* n : ptr(st.player_units[game_local_player])) {
					if (unit_is(n, UnitTypes::Zerg_Larva) && n->connected_unit == hatch) {
						if (!cleared) { current_selection_clear(); cleared = true; }
						current_selection_add(n);
					}
				}
			}
			break;
		}
		case cardact_build: {
			// v0.64 item 21: refuse at the button, not after the player has
			// hunted for a spot. Train/morph/research/upgrade all already gate
			// here; build was the odd one out, deferring the check to
			// place_building_at() and then ignoring its result. Tapping an
			// unaffordable building now says so immediately (with the per-race
			// advisor line) instead of entering placement mode and rejecting the
			// tap later.
			if (!check_afford_unit(get_unit_type((UnitTypes)b.arg))) break;
			placing_type = get_unit_type((UnitTypes)b.arg);
			race_t race = st.players[game_local_player].race;
			Orders o;
			if (race == race_t::zerg) o = Orders::DroneStartBuild;
			else if (race == race_t::protoss) o = Orders::PlaceProtossBuilding;
			else o = Orders::PlaceBuilding;
			placing_order = get_order_type(o);
			placing_pos = {-1, -1};
			build_menu_return = card_menu; // task 3: remember the submenu to reopen
			card_menu = 0;
			break;
		}
		case cardact_place_cancel:
			placing_type = nullptr;
			placing_order = nullptr;
			placing_pos = {-1, -1};
			placing_land = false;
			placing_self = false;
			break;
		case cardact_ability:
			submit_ability(b.arg);
			attack_armed = false;
			ability_armed_order = -1;
			break;
		case cardact_ability_target:
			// Toggle armed state, mutually exclusive with the attack cursor.
			attack_armed = false;
			ability_armed_order = (ability_armed_order == b.arg) ? -1 : b.arg;
			break;
		case cardact_liftoff: {
			unit_t* u = first_own_selected_unit();
			if (u && submit_select_action()) {
				game_action_writer w;
				w.u8(47); // liftoff (at the building's own position)
				w.i16((int16_t)u->position.x);
				w.i16((int16_t)u->position.y);
				submit_game_action(w);
			}
			attack_armed = false;
			ability_armed_order = -1;
			break;
		}
		case cardact_land: {
			// Toggle placement mode for landing the selected lifted building.
			unit_t* u = first_own_selected_unit();
			if (placing_land) {
				placing_type = nullptr; placing_order = nullptr; placing_land = false;
				placing_self = false;
				placing_pos = {-1, -1};
			} else if (u) {
				placing_type = u->unit_type;
				placing_order = get_order_type(Orders::BuildingLand);
				placing_pos = {-1, -1};
				placing_land = true;
				card_menu = 0;
			}
			attack_armed = false;
			ability_armed_order = -1;
			break;
		}
		case cardact_nydus_exit: {
			// v0.83.034: enter placement mode for the Nydus Canal's exit. The
			// selected canal is its own builder (placing_self); the placement
			// confirm issues opcode 12 with the BuildNydusExit order, which the
			// engine routes to order_BuildNydusExit.
			if (!check_afford_unit(get_unit_type(UnitTypes::Zerg_Nydus_Canal))) break;
			placing_type = get_unit_type(UnitTypes::Zerg_Nydus_Canal);
			placing_order = get_order_type(Orders::BuildNydusExit);
			placing_self = true;
			placing_pos = {-1, -1};
			card_menu = 0;
			attack_armed = false;
			ability_armed_order = -1;
			break;
		}
		case cardact_research:
			if (!check_afford_tech(get_tech_type((TechTypes)b.arg))) break;
			submit_production_routed([&](game_action_writer& w) {
				w.u8(48); // research
				w.u8((uint8_t)b.arg);
			});
			break;
		case cardact_upgrade:
			if (!check_afford_upgrade(get_upgrade_type((UpgradeTypes)b.arg))) break;
			submit_production_routed([&](game_action_writer& w) {
				w.u8(50); // upgrade
				w.u8((uint8_t)b.arg);
			});
			break;
		case cardact_cancel:
			if (submit_select_action()) {
				game_action_writer w;
				w.u8((uint8_t)b.arg); // cancel opcode
				if (b.arg == 32) w.u16(254); // cancel_build_queue: 254 = last slot
				submit_game_action(w);
			}
			break;
		case cardact_addon: {
			unit_t* u = first_own_selected_unit();
			auto* at = get_unit_type((UnitTypes)b.arg);
			if (u && at && submit_select_action()) {
				// action_build recomputes the builder position from tile_pos by
				// subtracting the addon offset, so pick the tile that maps back
				// to the building's own position (its addon slot).
				int ox = at->addon_position.x / 32 * 32;
				int oy = at->addon_position.y / 32 * 32;
				int tx = (u->position.x - u->unit_type->placement_size.x / 2 + ox) / 32;
				int ty = (u->position.y - u->unit_type->placement_size.y / 2 + oy) / 32;
				game_action_writer w;
				w.u8(12); // build
				w.u8((uint8_t)(int)Orders::PlaceAddon);
				w.u16((uint16_t)tx);
				w.u16((uint16_t)ty);
				w.u16((uint16_t)b.arg);
				submit_game_action(w);
			}
			break;
		}
		default:
			break;
		}
	}

	// Issues a no-target ability action (opcode) for the current selection,
	// appending the payload byte the engine expects for that opcode.
	void submit_ability(int opcode) {
		if (!submit_select_action()) return;
		game_action_writer w;
		w.u8((uint8_t)opcode);
		switch (opcode) {
		case 30: case 37: case 38: case 40: case 43: case 44:
			w.u8(order_queued() ? 1 : 0); // queue byte
			break;
		case 33: case 34: case 45:
			w.u8(0); // dummy byte
			break;
		default:
			break; // 39/42/54 and the cancels take no extra byte
		}
		submit_game_action(w);
		play_unit_yes(first_own_selected_unit()); // ability acknowledgment
	}

	// A gas extractor (Refinery/Extractor/Assimilator) may only sit on a vespene
	// geyser, so its placement is finicky. Snap the tapped position to the
	// nearest geyser (within a generous radius) for quality-of-life placement.
	bool is_gas_building(const unit_type_t* t) {
		return t && (t->id == UnitTypes::Terran_Refinery || t->id == UnitTypes::Zerg_Extractor || t->id == UnitTypes::Protoss_Assimilator);
	}
	xy snap_gas_placement(xy pos) {
		unit_t* best = nullptr; int best_d2 = 0;
		for (unit_t* g : find_units({pos - xy(160, 160), pos + xy(160, 160)})) {
			if (!unit_is(g, UnitTypes::Resource_Vespene_Geyser)) continue;
			xy c = g->sprite->position;
			int dx = c.x - pos.x, dy = c.y - pos.y, d2 = dx * dx + dy * dy;
			if (!best || d2 < best_d2) { best = g; best_d2 = d2; }
		}
		return best ? best->sprite->position : pos;
	}

	// Places the pending building at the tile under (mx,my). Returns true if
	// the build was issued (spot valid); false if blocked, so the caller can
	// keep placement mode active.
	bool place_building_at(int mx, int my) {
		if (!placing_type) return false;
		// Feedback only (does not block): a worker may gather enough by the time it
		// walks to the site, so let the build proceed and let the engine validate.
		if (!placing_land) check_afford_unit(placing_type);
		xy pos = screen_to_world(mx, my);
		if (is_gas_building(placing_type)) pos = snap_gas_placement(pos);
		pos.x = (pos.x - placing_type->placement_size.x / 2) / 32 * 32 + placing_type->placement_size.x / 2;
		pos.y = (pos.y - placing_type->placement_size.y / 2) / 32 * 32 + placing_type->placement_size.y / 2;
		// action_build needs exactly one selected unit and returns true even
		// on a blocked spot, so pick a single builder and pre-check here. When
		// landing, the lifted building is its own builder; otherwise a worker.
		unit_t* builder = nullptr;
		if (placing_land || placing_self) {
			builder = first_own_selected_unit();
		} else {
			// SHIFT builds are ONE worker's sequence: the builder is the
			// first selected worker, and every further building placed with SHIFT on
			// queues behind it (build_queue_ui). v0.126.001 briefly handed each build to
			// a free worker instead -- that is not what the queue is for.
			for (auto uid : current_selection) {
				unit_t* u = get_unit(uid);
				if (!u || u->owner != game_local_player) continue;
				if (unit_is(u, UnitTypes::Terran_SCV) || unit_is(u, UnitTypes::Protoss_Probe) || unit_is(u, UnitTypes::Zerg_Drone)) {
					builder = u;
					break;
				}
			}
		}
		if (!builder || !can_place_building(builder, game_local_player, placing_type, pos, false, false)) return false;
		// #1(a): if queue-mode is on and this worker is already building, queue the
		// new build behind it instead of interrupting -- process_build_queue()
		// issues it once the worker is free. Only for worker builds (not landing /
		// self-builders). Placement mode ends so the next build can be placed.
		if (queue_mode && !placing_land && !placing_self && worker_busy_building(builder)) {
			build_queue_ui.push_back({get_unit_id(builder), (int)placing_type->id, pos, placing_order});
			placing_type = nullptr;
			placing_order = nullptr;
			placing_pos = {-1, -1};
			placing_land = false;
			placing_self = false;
			card_menu = build_menu_return; // task 3: keep the build submenu open for the next order
			return true;
		}
		submit_select_single(builder);
		xy_t<size_t> tile;
		tile.x = (size_t)std::max(0, pos.x - placing_type->placement_size.x / 2) / 32u;
		tile.y = (size_t)std::max(0, pos.y - placing_type->placement_size.y / 2) / 32u;
		game_action_writer w;
		w.u8(12); // build
		w.u8((uint8_t)(int)placing_order->id);
		w.u16((uint16_t)tile.x);
		w.u16((uint16_t)tile.y);
		w.u16((uint16_t)(int)placing_type->id);
		submit_game_action(w);
		bool worker_build = !placing_land && !placing_self;
		placing_type = nullptr;
		placing_order = nullptr;
		placing_pos = {-1, -1};
		placing_land = false;
		placing_self = false;
		// task 3: with QUEUE on, reopen the build submenu (instead of dropping back
		// to the context card) so several buildings can be placed in a row without
		// re-tapping BUILD each time. The first build submits now; subsequent ones
		// queue via build_queue_ui once the worker is busy. QUEUE off = unchanged.
		if (queue_mode && worker_build) card_menu = build_menu_return;
		return true;
	}

	// Handles a game-mode click that is not on the command card. Returns
	// true if the click was consumed (placement targeting / armed attack).
	bool game_click(int mx, int my) {
		auto minimap = get_minimap_area();
		bool on_minimap = mx >= minimap.from.x && mx < minimap.to.x && my >= minimap.from.y && my < minimap.to.y;
		if (placing_type) {
			if (on_minimap) return false;
			// Snap the preview to the tile grid, keeping the tap centered. Gas
			// extractors additionally snap onto the nearest geyser.
			xy pos = screen_to_world(mx, my);
			if (is_gas_building(placing_type)) pos = snap_gas_placement(pos);
			pos.x = (pos.x - placing_type->placement_size.x / 2) / 32 * 32 + placing_type->placement_size.x / 2;
			pos.y = (pos.y - placing_type->placement_size.y / 2) / 32 * 32 + placing_type->placement_size.y / 2;
			placing_pos = pos;
			return true;
		}
		if (attack_armed) {
			attack_armed = false;
			xy pos;
			if (on_minimap) {
				pos = minimap_to_map(mx, my);
			} else {
				pos = screen_to_world(mx, my);
			}
			unit_t* target = on_minimap ? nullptr : select_get_unit_at(pos);
			// #11: attack-move to open ground trails support units behind the
			// fighters (mixed selection only). A targeted attack is left intact.
			if (!target && issue_group_move_split(pos, true, false)) return true;
			if (!submit_select_action()) return true;
			game_action_writer w;
			w.u8(21); // order
			w.i16((int16_t)pos.x);
			w.i16((int16_t)pos.y);
			w.u16(target ? get_unit_id(target).raw_value : 0);
			w.u16((uint16_t)(int)UnitTypes::None);
			w.u8((uint8_t)(int)(target ? Orders::AttackUnit : Orders::AttackMove));
			w.u8(order_queued() ? 1 : 0); // v0.126: SHIFT queues an attack like a move (it was always "not queued")
			submit_game_action(w);
			return true;
		}
		if (ability_armed_order >= 0) {
			int order_id = ability_armed_order;
			ability_armed_order = -1;
			xy pos;
			if (on_minimap) {
				pos = minimap_to_map(mx, my);
			} else {
				pos = screen_to_world(mx, my);
			}
			unit_t* target = on_minimap ? nullptr : select_get_unit_at(pos);
			// SCBW2 smart-cast: route an energy spell to the single best-energy caster.
			if (try_smart_cast(order_id, pos, target)) return true;
			if (!submit_select_action()) return true;
			game_action_writer w;
			w.u8(21); // order
			w.i16((int16_t)pos.x);
			w.i16((int16_t)pos.y);
			w.u16(target ? get_unit_id(target).raw_value : 0);
			w.u16((uint16_t)(int)UnitTypes::None);
			w.u8((uint8_t)order_id);
			w.u8(order_queued() ? 1 : 0);
			submit_game_action(w);
			return true;
		}
		return false;
	}

#include "hud3.h"   // HUD v3, the touch HUD (members of this class; after the card types it uses)

	bool is_moving_minimap = false;
	xy minimap_drag_pos = xy(-1, -1);
	bool is_moving_replay_slider = false;
	bool is_paused = false;
	bool is_drag_selecting = false;
	bool is_dragging_screen = false;
	int drag_select_from_x = 0;
	int drag_select_from_y = 0;
	int drag_select_to_x = 0;
	int drag_select_to_y = 0;
	xy drag_screen_pos;

	void update() {
		auto now = clock.now();

		// Crash-header breadcrumb: record the frame the sim last reached, from a spot
		// that runs every rendered frame in every mode. A plain store; read from the
		// signal handler (never live st there). Cost when idle: one volatile write.
		bwgame::diag::set_frame((uint32_t)st.current_frame);

		if (now - last_fps >= std::chrono::seconds(1)) {
			//ui::log("draw fps: %g\n", fps_counter / std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1, 1>>>(now - last_fps).count());
			fps_display = fps_counter;
			last_fps = now;
			fps_counter = 0;
			// Faze 4.5 FULL HD auto-fallback monitor (runs once per second).
			// Faze 5.2: only count low FPS during actual GAMEPLAY. The briefing,
			// the loading/prewarm screen and a playing cinematic all legitimately
			// render slowly (or block the loop) -- counting those spuriously fired
			// the downgrade and demoted FULL HD users to HD2 on the next launch.
			bool perf_gameplay = game_input && !briefing_active && !loading_mode;
			if (perf_fps_threshold > 0 && !perf_downgrade_fired && perf_gameplay) {
				if (perf_warmup_secs > 0) {
					--perf_warmup_secs;
				} else if (fps_display > 0 && fps_display < perf_fps_threshold) {
					if (++perf_low_secs >= perf_low_secs_needed) {
						perf_downgrade_fired = true;
						if (perf_downgrade_cb) perf_downgrade_cb();
					}
				} else {
					perf_low_secs = 0; // any good second resets the streak
				}
			} else if (perf_fps_threshold > 0 && !perf_downgrade_fired) {
				perf_low_secs = 0; // non-gameplay (briefing/loading): never accumulate
			}
		}
		++fps_counter;

		poll_under_attack_alert();
		poll_research_complete();
		poll_last_error();
		poll_music();
		poll_ready_sounds();
		poll_portrait();
		if (game_input) process_build_queue(); // #1(a): auto-issue queued builds
		if (game_input) prune_control_groups(); // task 8(a): auto-clear emptied groups
		group_menu_tick();

		auto minimap_area = get_minimap_area();
		auto replay_slider_area = get_replay_slider_area();
		// Let a single-finger press on the minimap start a viewport slide at once
		// (finger/S-Pen), instead of tap-only.
		if (wnd && game_input) wnd.set_immediate_drag_rect(minimap_area.from.x, minimap_area.from.y, minimap_area.to.x, minimap_area.to.y);
		else if (wnd) wnd.set_immediate_drag_rect(0, 0, -1, -1);
		hud3_publish_hold_rects();   // HUD v3: SHIFT / camera stick fingers stay out of the gestures
		hud3_tick();

		auto move_minimap = [&](int mouse_x, int mouse_y) {
			// Remember the latest drag point so the per-frame poll can re-apply
			// it without querying the OS cursor (whose synthetic-from-touch
			// position lags on Android, which froze the drag until release).
			minimap_drag_pos = xy(mouse_x, mouse_y);
			if (mouse_x < minimap_area.from.x) mouse_x = minimap_area.from.x;
			else if (mouse_x >= minimap_area.to.x) mouse_x = minimap_area.to.x - 1;
			if (mouse_y < minimap_area.from.y) mouse_y = minimap_area.from.y;
			else if (mouse_y >= minimap_area.to.y) mouse_y = minimap_area.to.y - 1;
			int x = mouse_x - minimap_area.from.x;
			int y = mouse_y - minimap_area.from.y;
			x = x * game_st.map_tile_width / (minimap_area.to.x - minimap_area.from.x);
			y = y * game_st.map_tile_height / (minimap_area.to.y - minimap_area.from.y);
			// Center on the visible playfield (not the full screen) so the tapped
			// point lands above the console, not behind it.
			screen_pos = xy(32 * x - (int)view_width / 2, 32 * y - playfield_view_height() / 2);
		};

		auto check_move_minimap = [&](auto& e) {
			if (e.mouse_x >= minimap_area.from.x && e.mouse_x < minimap_area.to.x) {
				if (e.mouse_y >= minimap_area.from.y && e.mouse_y < minimap_area.to.y) {
					is_moving_minimap = true;
					move_minimap(e.mouse_x, e.mouse_y);
				}
			}
		};

		auto move_replay_slider = [&](int mouse_x, int mouse_y) {
			(void)mouse_y;
			replay_frame = replay_frame_at(mouse_x);   // the seek target; the glue does the seeking
		};

		auto check_move_replay_slider = [&](auto& e) {
			if (game_input) return; // no replay slider in interactive games
			if (e.mouse_x >= replay_slider_area.from.x && e.mouse_x < replay_slider_area.to.x) {
				if (e.mouse_y >= replay_slider_area.from.y && e.mouse_y < replay_slider_area.to.y) {
					// a battle / base mark: jump a moment before it and show the place
					int mark_frame = 0; xy mark_pos;
					if (replay_mark_at(e.mouse_x, e.mouse_y, mark_frame, mark_pos)) {
						replay_frame = mark_frame;
						if (mark_pos.x >= 0) center_camera_on(mark_pos);
						return;
					}
					is_moving_replay_slider = true;
					move_replay_slider(e.mouse_x, e.mouse_y);
				}
			}
		};

		auto end_drag_select = [&](bool double_clicked) {
			bool shift = wnd.get_key_state(225) || wnd.get_key_state(229);
			// HUD v2 (2026-10-02): the on-screen SHIFT toggle (the old
			// QUEUE) also does Shift's selection job -- with it on, a tap on an own
			// unit adds / removes it and a box adds. A tap on the ground or on an
			// enemy stays a (queued) command, so `shift` itself is left alone for
			// the tap-to-command test below. Pure viewer-side selection; the engine
			// sees ordinary select actions.
			const bool sel_shift = shift || (queue_mode && !ui_legacy);
			if (drag_select_from_x > drag_select_to_x) std::swap(drag_select_from_x, drag_select_to_x);
			if (drag_select_from_y > drag_select_to_y) std::swap(drag_select_from_y, drag_select_to_y);
			// A click is a press that did not travel. This used to be `<= 4 ||`, so a
			// long drag that happened to be straight (300 px wide, 3 px tall) was a
			// "click" at its start point -- and with tap-to-command that is an order.
			if (drag_select_to_x - drag_select_from_x <= 4 && drag_select_to_y - drag_select_from_y <= 4) {
				// Faze 4.3: drag_select_* are BUFFER pixels; at world_scale>1 they must
				// be divided by view_scale (screen_to_world does this) or the click
				// hit-tests at ~2x the map distance - the "unit drawn right, selects
				// left" bug. At view_scale==1 screen_to_world is identical to the old
				// screen_pos + xy(...), so HD2/SD are unchanged. (Touch taps arrive
				// as a tiny drag, so this single-click path is what tablet taps hit.)
				unit_t* u = select_get_unit_at(screen_to_world(drag_select_from_x, drag_select_from_y));
				bool ctrl = wnd.get_key_state(224) || wnd.get_key_state(228);
				// Tap-to-command (modern touch-RTS): a plain single tap, with OWN units
				// already selected, on empty ground or a NON-own unit issues the
				// contextual order (move / attack / gather) instead of the 400 ms
				// long-press. Tapping your OWN unit still selects it; shift/ctrl still
				// modify the selection (so the S-Pen barrel = shift still selects); a
				// double-tap still select-all-of-type. Off => classic (tap = select).
				// Determinism-safe: the order goes through the recorded action stream.
				if (game_input && (tap_command || hud_v3) && !shift && !ctrl && !double_clicked
				    && (!u || u->owner != game_local_player)) {
					bool own_sel = false;
					for (auto uid : current_selection) { unit_t* s = get_unit(uid); if (s && s->owner == game_local_player) { own_sel = true; break; } }
					if (own_sel) {
						// v0.126: the drag state must end here too. This early return used to
						// leave is_drag_selecting set, so the NEXT tap on a HUD button skipped
						// its handler and re-issued this order at the old point instead.
						is_drag_selecting = false;
						issue_default_order_at(drag_select_from_x, drag_select_from_y);
						return;
					}
				}
				if (u) {
					if (double_clicked || ctrl) {
						if (!sel_shift) current_selection_clear();
						auto is_tank = [&](unit_t* a) {
							return unit_is(a, UnitTypes::Terran_Siege_Tank_Siege_Mode) || unit_is(a, UnitTypes::Terran_Siege_Tank_Tank_Mode);
						};
						auto is_same_type = [&](unit_t* a, unit_t* b) {
							if (unit_is_mineral_field(a) && unit_is_mineral_field(b)) return true;
							if (is_tank(a) && is_tank(b)) return true;
							return a->unit_type == b->unit_type;
						};
						for (unit_t* u2 : find_units({screen_pos, screen_pos + xy(view_width, view_height)})) {
							if (u2->owner != u->owner) continue;
							if (!is_same_type(u, u2)) continue;
							current_selection_add(u2);
						}
					} else {
						if (sel_shift && (shift || u->owner == game_local_player)) {
							if (current_selection_is_selected(u)) current_selection_remove(u);
							else current_selection_add(u);
						} else {
							current_selection_clear();
							current_selection_add(u);
						}
					}
				}
			} else {
				if (!sel_shift) current_selection_clear();
				// Faze 4.3: convert the drag box's buffer corners to world via
				// screen_to_world (divides by view_scale) so the box matches the
				// world_scale>1 render. view_scale==1 => same as before (+screen_pos).
				auto r = rect{ screen_to_world(drag_select_from_x, drag_select_from_y),
				               screen_to_world(drag_select_to_x, drag_select_to_y) };
				if (r.from.x > r.to.x) std::swap(r.from.x, r.to.x);
				if (r.from.y > r.to.y) std::swap(r.from.y, r.to.y);
				// A box selection never mixes units with buildings. The local
				// player's mobile units take priority; only if there are none do
				// own buildings get picked; only if there are neither do neutral/
				// enemy units (viewing). So dragging over an SCV next to a
				// Command Center selects the SCV, not the CC.
				a_vector<unit_t*> mobile, buildings, others;
				for (unit_t* u : find_units(r)) {
					if (!unit_can_be_selected(u)) continue;
					if (u->owner == game_local_player) {
						if (u->unit_type->flags & unit_type_t::flag_building) buildings.push_back(u);
						else mobile.push_back(u);
					} else {
						others.push_back(u);
					}
				}
				a_vector<unit_t*>* pick = !mobile.empty() ? &mobile
					: (!buildings.empty() ? &buildings : (!others.empty() ? &others : nullptr));
				if (pick) for (unit_t* u : *pick) current_selection_add(u);
			}
			is_drag_selecting = false;
			// "what is it?" on new selection. v0.83.001: own units OR a neutral
			// one. Critters (Rhynadon/Bengalaas/Scantid/Kakaru/Ragnasaur/Ursadon)
			// are the reason -- they carry real `what` sounds in units.dat
			// (Misc\CRITTERS\*.wav, confirmed with `aidump soundicons`) and the
			// engine does create them in melee (bwgame.h UNIT reader lists them
			// as neutral), but first_own_selected_unit() filtered them out, so
			// tapping one was silent. Enemy units are deliberately NOT included:
			// I could not establish retail's behaviour there, and a voice line
			// would leak an enemy's position.
			unit_t* what = first_own_selected_unit();
			if (!what) what = first_neutral_selected_unit();
			play_unit_what(what);
		};

		if (wnd) {
			native_window::event_t e;
			while (wnd.peek_message(e)) {
				// While the chat bar is open, swallow game gestures so typing
				// (soft keyboard) can't move units or the camera.
				if (chat_active) {
					switch (e.type) {
					case native_window::event_t::type_mouse_button_down:
					case native_window::event_t::type_mouse_button_up:
					case native_window::event_t::type_mouse_motion:
					case native_window::event_t::type_long_press:
					case native_window::event_t::type_zoom:
						continue;
					default: break;
					}
				}
				if (hud_v3) {
					if (e.type == native_window::event_t::type_mouse_button_down && e.button == 1) hud_press_pos = xy(e.mouse_x, e.mouse_y);
					if (hud3_input(e)) {
						if (e.type == native_window::event_t::type_mouse_button_up) hud_press_pos = xy(-1, -1);
						continue;
					}
				}
				switch (e.type) {
				case native_window::event_t::type_quit:
					if (exit_on_close) std::exit(0);
					else window_closed = true;
					break;
				case native_window::event_t::type_resize:
					resize(e.width, e.height);
					break;
				case native_window::event_t::type_mouse_button_down:
					if (e.button == 1) hud_press_pos = xy(e.mouse_x, e.mouse_y);   // HUD v2 pressed state
					if (lobby_mode) {
						if (e.button == 1) lobby_tap(e.mouse_x, e.mouse_y);
						break;
					}
					if (briefing_active) {
						if (e.button == 1) briefing_click(e.mouse_x, e.mouse_y);
						break;
					}
					if (e.button == 1) {
						if (pause_screen != 0 || (game_input && menu_button_hit(e.mouse_x, e.mouse_y))) {
							break; // pause menu / MENU button handled on release
						}
						// A2.1: replay transport buttons (|< << pause/play >> >| - + V).
						if (!game_input && replay_st.end_frame != 0) {
							int rb = replay_btn_hit(e.mouse_x, e.mouse_y);
							if (rb >= 0) {
								int seek = 10 * 1000 / 42; // ~10 s of frames
								int end = (int)replay_st.end_frame;
								switch (rb) {
								case rbtn_start: replay_frame = 0; break;
								case rbtn_back: replay_frame = replay_frame > seek ? replay_frame - seek : 0; break;
								case rbtn_pause: is_paused = !is_paused; break;
								case rbtn_fwd: replay_frame += seek; if (replay_frame >= end) replay_frame = end - 1; break;
								case rbtn_end: replay_frame = end - 1; break;
								case rbtn_slower: if (game_speed > 2_fp8) game_speed /= 2; break;
								case rbtn_faster: if (game_speed < fp8::integer(128)) game_speed *= 2; break;
								// EXIT lives in the native bar now (it was a Java overlay button):
								// the same path as hardware Back -- leave to the launcher.
								case rbtn_exit: request_exit = true; break;
								}
								break;
							}
							// The panel's close button / tabs, the phone's PLAYERS button.
							if (replay_overlay_click(e.mouse_x, e.mouse_y)) break;
							// Per-player vision toggle (the eye at the right end of a card):
							// tap -> watch through that player's fog; tap again -> full-map
							// vision. Checked before the card hit because it sits inside the
							// card's rectangle.
							int vp = observer_vision_btn_hit(e.mouse_x, e.mouse_y);
							if (vp >= 0) {
								replay_vision_player = (replay_vision_player == vp) ? -1 : vp;
								break;
							}
							// Tap a player card -> toggle that player's panel (army / upgrades / buildings).
							int op = observer_row_hit(e.mouse_x, e.mouse_y);
							if (op >= 0) {
								observer_flyout_player = (observer_flyout_player == op) ? -1 : op;
								break;
							}
							if (replay_overlay_at(e.mouse_x, e.mouse_y)) break;   // nothing behind a panel reacts
						}
						int zdelta_dn, gnum_dn, util_dn;
						rect pbd = ui_legacy ? rect{} : hud2_ping_button();
						bool ping_dn = !ui_legacy && e.mouse_x >= pbd.from.x && e.mouse_x < pbd.to.x && e.mouse_y >= pbd.from.y && e.mouse_y < pbd.to.y;
						unit_t* cargo_dn = nullptr;
						if (game_input && (ping_dn || auto_worker_button_hit(e.mouse_x, e.mouse_y) || queue_button_hit(e.mouse_x, e.mouse_y) || zoom_button_hit(e.mouse_x, e.mouse_y, zdelta_dn) || util_button_hit(e.mouse_x, e.mouse_y, util_dn) || group_button_hit(e.mouse_x, e.mouse_y, gnum_dn) || group_menu_hit(e.mouse_x, e.mouse_y) != gm_none || sel_page_tab_hit_test(e.mouse_x, e.mouse_y) || cargo_slot_hit(e.mouse_x, e.mouse_y, cargo_dn) || production_slot_hit(e.mouse_x, e.mouse_y) >= 0 || card_hit_test(e.mouse_x, e.mouse_y) >= 0 || equip_hit_test(e.mouse_x, e.mouse_y) >= 0 || placing_type || attack_armed || ability_armed_order >= 0)) {
							// Command card / queue toggle / zoom / group button /
							// targeting click: handled on release (or long-press),
							// must not start a drag-select.
							if (game_input) { card_hover_index = card_hit_test(e.mouse_x, e.mouse_y); equip_hover_upgrade = equip_hit_test(e.mouse_x, e.mouse_y); }
							break;
						}
						check_move_minimap(e);
						check_move_replay_slider(e);
						// Never start a drag-select (which on release becomes a tap-command)
						// on the opaque bottom console -- the minimap/controls there are
						// handled above, and any other spot (portrait, unit display) must be
						// inert, not leak an order to the field hidden behind the HUD.
						if (!is_moving_minimap && !is_moving_replay_slider && (hud_v3 ? !hud3_rect_at(e.mouse_x, e.mouse_y) : e.mouse_y < playfield_bottom_y())) {
							is_drag_selecting = true;
							drag_select_from_x = e.mouse_x;
							drag_select_from_y = e.mouse_y;
							drag_select_to_x = e.mouse_x;
							drag_select_to_y = e.mouse_y;
						}
					} else if (e.button == 3) {
						is_dragging_screen = true;
						drag_screen_pos = screen_pos + xy((fp16::integer(e.mouse_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y) / view_scale).integer_part());
					}
					break;
				case native_window::event_t::type_mouse_motion:
					if (e.button_state & 1) {
						// Once minimap-dragging, keep scrolling even if the
						// finger leaves the minimap (move_minimap clamps), so
						// a drag continuously pans instead of freezing at the
						// edge.
						if (is_moving_minimap) move_minimap(e.mouse_x, e.mouse_y);
						// Once dragging the timeline, keep seeking even if the finger leaves
						// the row (replay_frame_at clamps), like the minimap drag above.
						if (is_moving_replay_slider) move_replay_slider(e.mouse_x, e.mouse_y);
						if (is_drag_selecting) {
							drag_select_to_x = e.mouse_x;
							drag_select_to_y = e.mouse_y;
						}
					} else if (e.button_state & 4) {
						if (is_dragging_screen) {
							screen_pos = drag_screen_pos - xy((fp16::integer(e.mouse_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y) / view_scale).integer_part());
							//screen_pos -= xy((fp16::integer(e.mouse_x - drag_screen_x) / view_scale).integer_part(), (fp16::integer(e.mouse_y - drag_screen_y) / view_scale).integer_part());
						}
					}

					if (is_drag_selecting && ~e.button_state & 1) end_drag_select(false);
					// Track the card button under the pointer (stylus hover or a
					// press-hold) so its cost/requirement tooltip can be drawn.
					if (game_input && !is_drag_selecting) { card_hover_index = card_hit_test(e.mouse_x, e.mouse_y); equip_hover_upgrade = equip_hit_test(e.mouse_x, e.mouse_y); }
					// S-Pen / finger placement preview (T2): while placing a building,
					// let the ghost follow the pointer -- on hover (pen) and on drag --
					// so you see where it lands before the confirming long-press/tap,
					// instead of the ghost only appearing after the first tap. Snap the
					// same way place_building_at / game_click do.
					if (game_input && placing_type && !hud_control_at(e.mouse_x, e.mouse_y)) {
						auto mm = get_minimap_area();
						bool on_mm = e.mouse_x >= mm.from.x && e.mouse_x < mm.to.x && e.mouse_y >= mm.from.y && e.mouse_y < mm.to.y;
						if (!on_mm) {
							xy pos = screen_to_world(e.mouse_x, e.mouse_y);
							if (is_gas_building(placing_type)) pos = snap_gas_placement(pos);
							pos.x = (pos.x - placing_type->placement_size.x / 2) / 32 * 32 + placing_type->placement_size.x / 2;
							pos.y = (pos.y - placing_type->placement_size.y / 2) / 32 * 32 + placing_type->placement_size.y / 2;
							placing_pos = pos;
						}
					}
					break;
				case native_window::event_t::type_mouse_button_up:
					if (e.button == 1) hud_press_pos = xy(-1, -1);
					if (briefing_active) break;
					card_hover_index = -1; // release dismisses the tooltip
					equip_hover_upgrade = -1;
					if (e.button == 1) {
						if (pause_screen != 0) { pause_menu_tap(e.mouse_x, e.mouse_y); break; }
						if (game_input && menu_button_hit(e.mouse_x, e.mouse_y)) { open_pause_menu(); break; }
						if (game_input && !ui_legacy) {
							rect pb = hud2_ping_button();
							if (e.mouse_x >= pb.from.x && e.mouse_x < pb.to.x && e.mouse_y >= pb.from.y && e.mouse_y < pb.to.y) {
								// Ping the current view center. Sent through the action
								// stream (opcode 88) so on_ping_minimap shows it on
								// every allied client; in single player it round-trips
								// locally and sets ping_pos the same way.
								xy pp = screen_pos + xy((int)view_width / 2, playfield_view_height() / 2);
								game_action_writer w;
								w.u8(88);
								w.u16((uint16_t)pp.x);
								w.u16((uint16_t)pp.y);
								submit_game_action(w);
								break;
							}
						}
						if (game_input && !is_drag_selecting && !is_moving_minimap && !is_moving_replay_slider) {
							if (auto_worker_button_hit(e.mouse_x, e.mouse_y)) { // task 2: toggle auto-worker
								auto_worker_active = !auto_worker_active;
								break;
							}
							if (queue_button_hit(e.mouse_x, e.mouse_y)) {
								queue_mode = !queue_mode;
								break;
							}
							int zdelta;
							if (zoom_button_hit(e.mouse_x, e.mouse_y, zdelta)) {
								set_zoom(zoom_level + zdelta);
								break;
							}
							int uwhich;
							if (util_button_hit(e.mouse_x, e.mouse_y, uwhich)) {
								// v0.72 item 9: ARMY cycles army clusters
								// instead of pretending it can select more
								// than BW's 12-unit limit.
								if (uwhich == 0) select_next_idle_worker();
								else select_army_cluster(false);
								break;
							}
							if (sel_page_tab_hit(e.mouse_x, e.mouse_y)) break; // SC2 sub-card tab: switch page
							if (group_menu_n >= 0) {
								// the hold menu is up (nothing was selected: CLEAR waits for a tap)
								const int ga = group_menu_hit(e.mouse_x, e.mouse_y);
								if (ga == gm_replace || ga == gm_clear || (ga == gm_add && group_menu_has_sel)) { group_menu_apply(ga); break; }
								group_menu_close();
							}
							int gnum;
							if (group_button_hit(e.mouse_x, e.mouse_y, gnum)) {
								recall_control_group(gnum); // tap = recall
								break;
							}
							unit_t* cargo = nullptr;
							if (cargo_slot_hit(e.mouse_x, e.mouse_y, cargo)) {
								unload_single(cargo); // tap a passenger to unload it
								break;
							}
							int pslot = production_slot_hit(e.mouse_x, e.mouse_y);
							if (pslot >= 0) {
								// Tap a production thumbnail to cancel that slot.
								if (submit_select_action()) {
									game_action_writer w;
									w.u8(32); // cancel_build_queue
									w.u16((uint16_t)pslot);
									submit_game_action(w);
								}
								break;
							}
							int ci = card_hit_test(e.mouse_x, e.mouse_y);
							if (ci >= 0) {
								card_activate(ci);
								break;
							}
							// v0.83.025 (hardware feedback): a tap on the equipment
							// icons is a HUD tap, not a world order. Without this the
							// release fell through to game_click and the selected unit
							// got a move order to the terrain behind the HUD. (The
							// tooltip itself shows on tap-hold via equip_hover_upgrade.)
							if (equip_hit_test(e.mouse_x, e.mouse_y) >= 0) break;
							// SCBW2 group-focus: a tap on a wireframe grid cell FOCUSES that
							// unit -- its 3x3 card + portrait switch to it so its abilities
							// are usable, while the whole group stays selected (tap a High
							// Templar in a zealot+templar ball, cast Psi Storm, group intact).
							// Checked before game_click so it doesn't fall through to a world
							// order. (Previously this narrowed the selection to the one unit.)
							if (unit_t* gu = selection_grid_unit_at(e.mouse_x, e.mouse_y)) {
								focused_unit_id = get_unit_id(gu);
								card_menu = 0;          // show the focused unit's base card page
								ability_armed_order = -1;
								play_unit_what(gu);
								break;
							}
							if (game_click(e.mouse_x, e.mouse_y)) break;
						}
						if (is_moving_minimap) is_moving_minimap = false;
						if (is_moving_replay_slider) is_moving_replay_slider = false;
						if (is_drag_selecting) {
							end_drag_select(e.clicks >= 2 && e.clicks % 2 == 0);
						}
					} else if (e.button == 3) {
						is_dragging_screen = false;
					}
					break;
				case native_window::event_t::type_touch_cancel:
					// A second finger landed: whatever the first finger had started is
					// dropped without any effect (see sdl2.cpp).
					hud_press_pos = xy(-1, -1);
					card_hover_index = -1;
					equip_hover_upgrade = -1;
					is_drag_selecting = false;
					is_moving_minimap = false;
					is_moving_replay_slider = false;
					if (group_menu_n >= 0 && group_menu_held) group_menu_close();   // a held group menu is dropped too
					break;
				case native_window::event_t::type_long_motion:
					if (group_menu_n >= 0 && group_menu_held) group_menu_pos = xy(e.mouse_x, e.mouse_y);
					break;
				case native_window::event_t::type_long_release:
					group_menu_release(e.mouse_x, e.mouse_y);
					break;
				case native_window::event_t::type_long_press:
					if (game_input && menu_button_hit(e.mouse_x, e.mouse_y)) {
						open_chat(); // Tap-Hold MENU = chat / cheats
						break;
					}
					if (game_input) {
						int gnum_lp;
						if (group_button_hit(e.mouse_x, e.mouse_y, gnum_lp)) {
							// HUD v2: the hold menu (add / replace / clear). Legacy HUD keeps
							// task 8(c): QUEUE on = add, off = overwrite.
							if (!ui_legacy) group_menu_open(gnum_lp);
							else if (queue_mode) append_control_group(gnum_lp);
							else assign_control_group(gnum_lp);
							break;
						}
					}
					if (game_input && placing_type) {
						// Long-press the target tile to place the building
						// (same gesture as issuing a unit order).
						place_building_at(e.mouse_x, e.mouse_y);
						break;
					}
					if (game_input) {
						// Touch has no true hover: press-and-hold on a command-card
						// button reveals its tooltip (without activating). It stays
						// up until the next tap dismisses it (button_up -> -1).
						int ci_lp = card_hit_test(e.mouse_x, e.mouse_y);
						if (ci_lp >= 0) { card_hover_index = ci_lp; break; }
						// Press-hold on an equipment icon reveals its upgrade
						// tooltip (mirror of the card branch above), and -- like the
						// card -- must not leak a world order through the HUD (#10).
						int eu_lp = equip_hit_test(e.mouse_x, e.mouse_y);
						if (eu_lp >= 0) { equip_hover_upgrade = eu_lp; break; }
						// Any other HUD chrome (production strip, cargo passengers,
						// queue/zoom/util/group/ping buttons) swallows the hold too,
						// instead of ordering the selection behind the HUD.
						if (hud_control_at(e.mouse_x, e.mouse_y)) break;
					}
					issue_default_order_at(e.mouse_x, e.mouse_y);
					break;
				case native_window::event_t::type_zoom:
					if (game_input) set_zoom(zoom_level + e.button);
					break;
				case native_window::event_t::type_text_input:
					if (chat_active || save_name_active) {
						size_t cur = chat_input.size();
						for (const char* p = e.text; *p && cur < (save_name_active ? 40u : 120u); ++p, ++cur) chat_input += *p;
					}
					break;
				case native_window::event_t::type_key_down:
					// Chat bar owns the keyboard while open: Enter sends, Esc/Back
					// cancels, Backspace deletes. Everything else is text (handled
					// by type_text_input) — don't leak keys to game hotkeys.
					// D5 save-name prompt owns it the same way: Enter confirms the save.
					if (save_name_active) {
						if (e.sym == '\r' || e.sym == '\n') confirm_save_name_prompt();
						else if (e.sym == 27 || e.scancode == 270) cancel_save_name_prompt();
						else if (e.sym == '\b' && !chat_input.empty()) chat_input.pop_back();
						break;
					}
					if (chat_active) {
						if (e.sym == '\r' || e.sym == '\n') submit_chat();
						else if (e.sym == 27 || e.scancode == 270) close_chat();
						else if (e.sym == '\b' && !chat_input.empty()) chat_input.pop_back();
						break;
					}
					//if (e.sym == 'q') {
					//	use_new_images = !use_new_images;
					//}
#ifndef EMSCRIPTEN
					// The corner button injects 'm'; Android hardware Back is consumed
					// by SDL and arrives as SDL_SCANCODE_AC_BACK (270), not via Java
					// onBackPressed. Both open (or close) the pause menu.
					if ((e.sym == 'm' || e.scancode == 270) && game_input) {
						if (pause_screen == 0) open_pause_menu();
						else close_pause_menu();
					}
					// In a replay there is no pause menu; hardware Back leaves to the
					// launcher (a guaranteed exit even if the native EXIT tap is missed).
					if (e.scancode == 270 && !game_input && replay_st.end_frame != 0) request_exit = true;
					if (e.sym == ' ' || e.sym == 'p') {
						is_paused = !is_paused;
					}
					if (e.sym == 'a' || e.sym == 'u') {
						if (game_speed < fp8::integer(128)) game_speed *= 2;
					}
					if (e.sym == 'z' || e.sym == 'd') {
						if (game_speed > 2_fp8) game_speed /= 2;
					}
					if (e.sym == 'v' && !game_input) {
						cycle_replay_vision(); // watch through a player's fog of war
					}
					if (e.sym == '\b') {
						int t = 5 * 1000 / 42; // rewind 5s: 1 frame == 42ms at fastest (was 5*42/1000 == 0, a no-op)
						if (replay_frame < t) replay_frame = 0;
						else replay_frame -= t;
					}
#endif
					break;
				}
			}
		}

		if (!indexed_surface) {
			// The rgba surface doubles as the window's present source (see
			// get_window_surface); indexed -> rgba blit is the only copy.
			if (wnd) {
				rgba_surface = native_window_drawing::get_window_surface(&wnd, screen_width, screen_height);
			} else {
				rgba_surface = native_window_drawing::create_rgba_surface(screen_width, screen_height);
			}
			indexed_surface = native_window_drawing::convert_to_8_bit_indexed(&*rgba_surface);
			if (!palette) set_image_data();
			indexed_surface->set_palette(palette);

			indexed_surface->set_blend_mode(native_window_drawing::blend_mode::none);
			rgba_surface->set_blend_mode(native_window_drawing::blend_mode::none);
			rgba_surface->set_alpha(0);
		}

		if (wnd) {
			auto input_poll_speed = std::chrono::milliseconds(12);

			auto input_poll_t = now - last_input_poll;
			while (input_poll_t >= input_poll_speed) {
				if (input_poll_t >= input_poll_speed * 20) last_input_poll = now - input_poll_speed;
				else last_input_poll += input_poll_speed;
				std::array<int, 6> scroll_speeds = {2, 2, 4, 6, 6, 8};

				if (!is_drag_selecting) {
					// camera_speed is a 25..250% scale on the classic accel curve
					// (Settings > Sąsaja). Keep a floor of 1 so a low % still moves.
					int scroll_speed = scroll_speeds[scroll_speed_n] * camera_speed / 100;
					if (scroll_speed < 1) scroll_speed = 1;
					auto prev_screen_pos = screen_pos;
					if (wnd.get_key_state(81)) screen_pos.y += scroll_speed;
					else if (wnd.get_key_state(82)) screen_pos.y -= scroll_speed;
					if (wnd.get_key_state(79)) screen_pos.x += scroll_speed;
					else if (wnd.get_key_state(80)) screen_pos.x -= scroll_speed;
					if (screen_pos != prev_screen_pos) {
						if (scroll_speed_n != scroll_speeds.size() - 1) ++scroll_speed_n;
					} else scroll_speed_n = 0;
				}

				input_poll_t = now - last_input_poll;
			}

			if (is_moving_minimap && minimap_drag_pos.x != -1) {
				// Re-apply the last drag point (from the motion events), NOT the
				// OS cursor: on touch the synthetic-mouse cursor lags, which made
				// the minimap freeze mid-drag and snap to the release point.
				move_minimap(minimap_drag_pos.x, minimap_drag_pos.y);
			}
			if (is_moving_replay_slider) {
				int x = -1;
				int y = -1;
				wnd.get_cursor_pos(&x, &y);
				if (x != -1) move_replay_slider(x, y);
			}
		}

		// Vertical clamp uses the PLAYFIELD height (visible area above the
		// console), so the map's bottom rows can rise above the console bar
		// instead of hiding behind it.
		int pf_vh = playfield_view_height();
		if ((int)screen_pos.y + pf_vh > (int)game_st.map_height) screen_pos.y = (int)game_st.map_height - pf_vh;
		if (screen_pos.y < 0) screen_pos.y = 0;
		if (screen_pos.x + view_width > game_st.map_width) screen_pos.x = game_st.map_width - view_width;
		if (screen_pos.x < 0) screen_pos.x = 0;

		uint8_t* data = (uint8_t*)indexed_surface->lock();
		if (lobby_mode) {
			// LAN lobby: don't render the game, just a dark screen (the
			// lobby status text is drawn later over the rgba surface).
			std::memset(data, 0, indexed_surface->pitch * screen_height);
		} else {
			// Decide the terrain tier FIRST. HD terrain overdraws every visible tile
			// in the RGBA pass and the indexed surface is never presented in HD (the
			// present below is a border-only RGBA fill, not the indexed->rgba blit),
			// so the whole indexed pass -- the clear AND draw_tiles -- is wasted work.
			// (lobby_mode still uses the indexed blit, so it keeps the clear.)
			hd_terrain_active = rgba_sprites && (!hd_tileset_dir.empty() || hd_casc);
			if (hd_terrain_active) { load_hd_tiles(); hd_terrain_active = hd_tiles.ok; }
			bool indexed_presented = !(hd_terrain_active && !lobby_mode);
			// Clear to black first (SD/lobby only): draw_tiles paints only the map's
			// tiles, so beyond the map (edge camera / small map) old pixels would
			// smear. In HD the indexed surface is unshown, so this ~screen-size 8bpp
			// memset every frame was pure waste (Faze 7 perf: skip it).
			if (indexed_presented) std::memset(data, 0, indexed_surface->pitch * screen_height);
			prof_t0 = prof_clock::now();
			if (!hd_terrain_active) draw_tiles(data, indexed_surface->pitch);
			prof_add(0);
			draw_sprites(data, indexed_surface->pitch);
			prof_add(1);

			draw_callback(data, indexed_surface->pitch);

			if (draw_ui_elements) {
				// Minimap is now drawn in the RGBA HUD pass (draw_minimap_scaled),
				// scaled into the console slot; no indexed-pass minimap here.
				draw_ui(data, indexed_surface->pitch);
			}
		}
		indexed_surface->unlock();

		// No clear needed: the indexed -> rgba blit is opaque and covers the
		// full surface. With HD terrain the indexed surface is all black
		// (tiles skipped, sprites queued to the RGBA pass, draw_ui/draw_callback
		// are no-ops, replay controls live in the RGBA pass), so the full-screen
		// palette conversion - the single most expensive draw phase on the tablet
		// (measured ~22ms/frame at FULL HD) - collapses to a cheap fill. This now
		// also covers replays: profiling a FULL HD replay showed the indexed
		// sprite/tile phases at ~0us (all black) yet the blit still ran, capping
		// replay playback at ~24fps; the fill lifts it without visual change since
		// HD terrain overdraws every pixel anyway.
		prof_t0 = prof_clock::now();
		if (hd_terrain_active && !lobby_mode) {
			// open1161HD Faze 7 perf: BORDER-ONLY fill. draw_tiles_hd overdraws every
			// visible map tile and the HUD lays an opaque band over the console, so
			// only the margins OUTSIDE the on-screen map rectangle need clearing to
			// black. In the common interior-camera / map-larger-than-view case that is
			// ZERO pixels (was a ~6.5ms full-screen memset every frame). Map spans
			// world [0,map_width]x[0,map_height] px; buffer px = (world-screen_pos)*ws.
			uint32_t* p = (uint32_t*)rgba_surface->lock();
			size_t pitch = rgba_surface->pitch / 4;
			int ws = world_scale < 1 ? 1 : world_scale;
			auto clampx = [&](int v){ return v < 0 ? 0 : (v > (int)screen_width ? (int)screen_width : v); };
			auto clampy = [&](int v){ return v < 0 ? 0 : (v > (int)screen_height ? (int)screen_height : v); };
			int mL = clampx((0 - (int)screen_pos.x) * ws);
			int mT = clampy((0 - (int)screen_pos.y) * ws);
			int mR = clampx(((int)game_st.map_width  - (int)screen_pos.x) * ws);
			int mB = clampy(((int)game_st.map_height - (int)screen_pos.y) * ws);
			if (mT > 0) std::fill_n(p, (size_t)mT * pitch, 0xff000000u);                       // above map
			if (mB < (int)screen_height) std::fill_n(p + (size_t)mB * pitch,                    // below map (HUD overdraws)
			    (size_t)((int)screen_height - mB) * pitch, 0xff000000u);
			for (int y = mT; y < mB; ++y) {                                                     // left/right margins
				uint32_t* row = p + (size_t)y * pitch;
				if (mL > 0) std::fill_n(row, (size_t)mL, 0xff000000u);
				if (mR < (int)screen_width) std::fill_n(row + mR, (size_t)((int)screen_width - mR), 0xff000000u);
			}
			rgba_surface->unlock();
		} else {
			indexed_surface->blit(&*rgba_surface, 0, 0);
		}
		prof_add(2);

		draw_image_queue();

		const bool replaying = !game_input && replay_st.end_frame != 0;   // the replay player draws in this pass too
		if (is_drag_selecting || game_input || !lobby_status.empty() || rgba_sprites || replaying) {
			uint32_t* data = (uint32_t*)rgba_surface->lock();
			// v0.22: true-color sprite pass, composited over the indexed world
			// blit and under fog/HUD.
			prof_t0 = prof_clock::now();
			if (rgba_sprites && !lobby_mode) {
				// v0.23: HD terrain first, then sprites on top.
				if (hd_point_lights) frame_lights.clear(); // SHADOWS Phase 2: rebuilt during the sprite pass
				draw_tiles_hd(data, rgba_surface->pitch / 4);
				process_rgba_sprite_queue(data, rgba_surface->pitch / 4);
				// SHADOWS Phase 2: additive point lights on the world, before fog/HUD.
				if (hd_point_lights && !frame_lights.empty()) draw_point_lights(data, rgba_surface->pitch / 4);
			}
			prof_add(3);
			if (is_drag_selecting) {
				auto r = rect{{drag_select_from_x, drag_select_from_y}, {drag_select_to_x, drag_select_to_y}};
				if (r.from.x > r.to.x) std::swap(r.from.x, r.to.x);
				if (r.from.y > r.to.y) std::swap(r.from.y, r.to.y);

				// v0.125: a faint fill and the shared 2 px stroke (it was a 1 px line)
				blend_rect_rgba(data, rgba_surface->pitch / 4, r, 0x18fc10u, 26);
				mark_rect(data, rgba_surface->pitch / 4, r, 0x18fc10u);
			}
			draw_fog(data, rgba_surface->pitch / 4);
			prof_add(4);
			draw_hud_rgba(data, rgba_surface->pitch / 4);
			draw_replay_hud(data, rgba_surface->pitch / 4); // the replay player: bar, production, panel (no-op unless replaying)
			if (briefing_active) draw_briefing_rgba(data, rgba_surface->pitch / 4);
			loading_cover_first_frame(data, rgba_surface->pitch / 4); // first SP frame only: loading screen, not a map flash
			prof_add(5);
			rgba_surface->unlock();
		}
		prof_frame_done();
		diag_frame_tick(); // DIAG bug-catcher: read-only per-frame detectors

		if (wnd) {
			wnd.set_bloom(hd_bloom); // Faze 7: GPU bloom (0 = off, default)
			wnd.set_post_gl(hd_post_gl, hd_post_effect, hd_post_strength / 100.0f, hd_gl_bloom / 100.0f, hd_grade / 100.0f); // SP: present stage + bloom + grade
			wnd.update_surface();
		}
	}

	std::tuple<int, int, uint32_t*> get_rgba_buffer() {
		void* r = rgba_surface->lock();
		rgba_surface->unlock();
		return std::make_tuple(rgba_surface->pitch / 4, rgba_surface->h, (uint32_t*)r);
	}

	template<typename cb_F>
	void async_read_file(a_string filename, cb_F cb) {
#ifdef EMSCRIPTEN
		auto uptr = std::make_unique<cb_F>(std::move(cb));
		auto f = [](void* ptr, uint8_t* data, size_t size) {
			cb_F* cb_p = (cb_F*)ptr;
			std::unique_ptr<cb_F> uptr(cb_p);
			(*cb_p)(data, size);
		};
		auto* a = filename.c_str();
		auto* b = (void(*)(void*, uint8_t*, size_t))f;
		auto* c = uptr.release();
		EM_ASM_({js_download_file($0, $1, $2);}, a, b, c);
#else
		filename = "data/" + filename;
		FILE* f = fopen(filename.c_str(), "rb");
		if (!f) {
			cb(nullptr, 0);
		} else {
			a_vector<uint8_t> data;
			fseek(f, 0, SEEK_END);
			data.resize(ftell(f));
			fseek(f, 0, SEEK_SET);
			data.resize(fread(data.data(), 1, data.size(), f));
			fclose(f);
			cb(data.data(), data.size());
		}
#endif
	}

	std::array<tileset_image_data, 8> all_tileset_img;

	template<typename load_data_file_F>
	void load_all_image_data(load_data_file_F&& load_data_file) {
		load_image_data(img, load_data_file);
		for (size_t i = 0; i != 8; ++i) {
			load_tileset_image_data(all_tileset_img[i], i, load_data_file);
		}
	}

	void set_image_data() {
		tileset_img = all_tileset_img.at(game_st.tileset_index);

		if (!palette) palette = native_window_drawing::new_palette();

		native_window_drawing::color palette_colors[256];
		if (tileset_img.wpe.size() != 256 * 4) error("wpe size invalid (%d)", tileset_img.wpe.size());
		for (size_t i = 0; i != 256; ++i) {
			palette_colors[i].r = tileset_img.wpe[4 * i + 0];
			palette_colors[i].g = tileset_img.wpe[4 * i + 1];
			palette_colors[i].b = tileset_img.wpe[4 * i + 2];
			palette_colors[i].a = tileset_img.wpe[4 * i + 3];
		}
		palette->set_colors(palette_colors);
		// Keep an RGB copy of the game palette for HUD elements that blit GRP
		// frames into the RGBA layer (unit wireframe, console art).
		for (size_t i = 0; i != 256; ++i) {
			game_palette[i] = ((uint32_t)palette_colors[i].r << 16)
				| ((uint32_t)palette_colors[i].g << 8) | (uint32_t)palette_colors[i].b;
		}
	}

	void reset() {
		apm = {};
		replay_frame = 0;
		auto& game = *st.game;
		st = state();
		game = game_state();
		replay_st = replay_state();
		action_st = action_state();

		st.global = &global_st;
		st.game = &game;
	}
};

}

