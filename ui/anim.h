// SC:R .anim (HD sprite) loader: file parser + BC1/BC3 (DXT1/DXT5) DDS
// decoding. Feeds the v0.22 RGBA sprite pass with true-color HD frames.
//
// Format (verified against HD2\anim\main_239.anim and neivv/animosity):
//   header: magic 'ANIM', u8 scale (1=SD 2=HD2 4=HD), u8 type, u16 unk,
//           u16 layers, u16 entries; 10 x 32B layer name table.
//   sprite: u16 frames, u16 ref_id (0xffff = own data), u16 w, u16 h,
//           u32 frame_arr_offset.  (w/h and all frame fields are in HD
//           units = 4x logical pixels, regardless of file scale.)
//   per-layer tex: u32 offset, u32 size, u16 w, u16 h (texture pixels).
//   frame: u16 tex_x, u16 tex_y, i16 x_off, i16 y_off, u16 w, u16 h, u32 unk.
//   textures are plain DDS blobs (DXT1 or DXT5), mip 0 only used.
//
// The atlas is downscaled to logical (SD) resolution at load time so the
// existing logical-resolution RGBA surface can blit 1:1. A future HD render
// surface can skip the downscale.
#ifndef BWGAME_UI_ANIM_H
#define BWGAME_UI_ANIM_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

namespace bwgame {
namespace anim {

struct frame_t {
	int x, y;              // top-left in the logical-resolution atlas
	int w, h;              // logical px (atlas sample size)
	int x_off_hd, y_off_hd; // offset within the sprite canvas, HD units
	int w_hd, h_hd;        // frame size in HD units
};

struct sprite_t {
	bool ok = false;
	int hd_per_logical = 4; // HD units per logical px (anchor math stays in
	                        // HD units and divides ONCE - two-step truncation
	                        // made animated buildings jitter)
	int canvas_w_hd = 0;
	int canvas_h_hd = 0;
	int canvas_w = 0;  // logical px
	int canvas_h = 0;
	int atlas_w = 0;
	int atlas_h = 0;
	std::vector<frame_t> frames;
	std::vector<uint32_t> diffuse;   // ARGB, atlas_w * atlas_h
	std::vector<uint8_t> teamcolor;  // 0..255 mask, same dims (empty if none)
	// open1161HD Faze 7: SC:R self-glow layer. ARGB, same dims (empty if the anim
	// has no emissive layer). Composited ADDITIVELY over diffuse+teamcolor so weapon
	// charges / psi / warp / shields glow, matching the oracle reference
	// (anim_composite: diffuse -> teamcolor tint -> +emissive).
	std::vector<uint32_t> emissive;
};

namespace detail {

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static inline uint32_t rgb565_to_argb(uint16_t c) {
	uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
	r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

// Decode one BC1 (DXT1) 4x4 block into out[16] ARGB.
static inline void decode_bc1_block(const uint8_t* src, uint32_t* out) {
	uint16_t c0 = rd16(src), c1 = rd16(src + 2);
	uint32_t idx = rd32(src + 4);
	uint32_t col[4];
	col[0] = rgb565_to_argb(c0);
	col[1] = rgb565_to_argb(c1);
	uint32_t r0 = (col[0] >> 16) & 0xff, g0 = (col[0] >> 8) & 0xff, b0 = col[0] & 0xff;
	uint32_t r1 = (col[1] >> 16) & 0xff, g1 = (col[1] >> 8) & 0xff, b1 = col[1] & 0xff;
	if (c0 > c1) {
		col[2] = 0xff000000u | (((2 * r0 + r1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * b0 + b1) / 3);
		col[3] = 0xff000000u | (((r0 + 2 * r1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((b0 + 2 * b1) / 3);
	} else {
		col[2] = 0xff000000u | (((r0 + r1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((b0 + b1) / 2);
		col[3] = 0; // transparent black
	}
	for (int i = 0; i != 16; ++i) out[i] = col[(idx >> (2 * i)) & 3];
}

// Decode one BC3 (DXT5) 4x4 block into out[16] ARGB.
static inline void decode_bc3_block(const uint8_t* src, uint32_t* out) {
	uint8_t a0 = src[0], a1 = src[1];
	uint64_t aidx = 0;
	for (int i = 0; i != 6; ++i) aidx |= (uint64_t)src[2 + i] << (8 * i);
	uint8_t atab[8];
	atab[0] = a0; atab[1] = a1;
	if (a0 > a1) {
		for (int i = 1; i != 7; ++i) atab[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
	} else {
		for (int i = 1; i != 5; ++i) atab[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
		atab[6] = 0; atab[7] = 255;
	}
	decode_bc1_block(src + 8, out); // color part uses 4-color mode semantics
	// force 4-color interpolation even when c0 <= c1
	uint16_t c0 = rd16(src + 8), c1 = rd16(src + 10);
	if (c0 <= c1) {
		uint32_t idx = rd32(src + 12);
		uint32_t col0 = rgb565_to_argb(c0), col1 = rgb565_to_argb(c1);
		uint32_t r0 = (col0 >> 16) & 0xff, g0 = (col0 >> 8) & 0xff, b0 = col0 & 0xff;
		uint32_t r1 = (col1 >> 16) & 0xff, g1 = (col1 >> 8) & 0xff, b1 = col1 & 0xff;
		uint32_t col[4];
		col[0] = col0; col[1] = col1;
		col[2] = 0xff000000u | (((2 * r0 + r1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * b0 + b1) / 3);
		col[3] = 0xff000000u | (((r0 + 2 * r1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((b0 + 2 * b1) / 3);
		for (int i = 0; i != 16; ++i) out[i] = col[(idx >> (2 * i)) & 3];
	}
	for (int i = 0; i != 16; ++i) {
		uint8_t a = atab[(aidx >> (3 * i)) & 7];
		out[i] = (out[i] & 0x00ffffffu) | ((uint32_t)a << 24);
	}
}

// Decode a DDS blob (DXT1/DXT5, mip 0) into ARGB. Returns false on failure.
static inline bool decode_dds(const uint8_t* data, size_t size, std::vector<uint32_t>& out, int& out_w, int& out_h) {
	if (size < 128 || rd32(data) != 0x20534444u) return false; // 'DDS '
	int h = (int)rd32(data + 12);
	int w = (int)rd32(data + 16);
	uint32_t fourcc = rd32(data + 84);
	bool bc3 = fourcc == 0x35545844u; // 'DXT5'
	bool bc1 = fourcc == 0x31545844u; // 'DXT1'
	if (!bc1 && !bc3) return false;
	int bw = (w + 3) / 4, bh = (h + 3) / 4;
	size_t block_size = bc3 ? 16 : 8;
	if (128 + (size_t)bw * bh * block_size > size) return false;
	out.assign((size_t)w * h, 0);
	out_w = w; out_h = h;
	const uint8_t* src = data + 128;
	uint32_t block[16];
	for (int by = 0; by != bh; ++by) {
		for (int bx = 0; bx != bw; ++bx) {
			if (bc3) decode_bc3_block(src, block); else decode_bc1_block(src, block);
			src += block_size;
			for (int py = 0; py != 4; ++py) {
				int y = by * 4 + py;
				if (y >= h) break;
				for (int px = 0; px != 4; ++px) {
					int x = bx * 4 + px;
					if (x >= w) break;
					out[(size_t)y * w + x] = block[py * 4 + px];
				}
			}
		}
	}
	return true;
}

// Box-downscale ARGB by an integer factor (alpha-weighted color average).
static inline void downscale_argb(const std::vector<uint32_t>& src, int sw, int sh, int factor, std::vector<uint32_t>& dst, int& dw, int& dh) {
	if (factor <= 1) { dst = src; dw = sw; dh = sh; return; }
	dw = sw / factor; dh = sh / factor;
	dst.assign((size_t)dw * dh, 0);
	for (int y = 0; y != dh; ++y) {
		for (int x = 0; x != dw; ++x) {
			uint32_t a = 0, r = 0, g = 0, b = 0;
			for (int sy = 0; sy != factor; ++sy) {
				const uint32_t* row = &src[(size_t)(y * factor + sy) * sw + x * factor];
				for (int sx = 0; sx != factor; ++sx) {
					uint32_t c = row[sx];
					uint32_t ca = c >> 24;
					a += ca;
					r += ((c >> 16) & 0xff) * ca;
					g += ((c >> 8) & 0xff) * ca;
					b += (c & 0xff) * ca;
				}
			}
			uint32_t n = factor * factor;
			uint32_t oa = a / n;
			if (a == 0) { dst[(size_t)y * dw + x] = 0; continue; }
			dst[(size_t)y * dw + x] = (oa << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a);
		}
	}
}

} // namespace detail

// Parse a ver-2 per-image .anim buffer and produce a logical-resolution
// atlas (diffuse + teamcolor mask). Returns sprite with ok=false on any
// structural mismatch.
static inline sprite_t parse(const uint8_t* data, size_t size, int render_scale = 1) {
	using namespace detail;
	sprite_t out;
	if (size < 12 + 10 * 32 + 12 || rd32(data) != 0x4d494e41u) return out; // 'ANIM'
	int scale = data[4];
	// open1161HD Faze 4.3: keep the atlas at render_scale x logical (ds = how much
	// we still downscale) so FULL HD sprites carry real detail instead of being
	// crushed to logical and upscaled. render_scale=1 -> ds=scale -> logical
	// atlas, byte-identical to before (HD2/SD path).
	if (render_scale < 1) render_scale = 1;
	int ds = scale / render_scale;
	if (ds < 1) ds = 1;
	int layer_count = rd16(data + 8);
	int entries = rd16(data + 10);
	if (entries != 1 || layer_count < 1 || layer_count > 10) return out;
	if (scale != 1 && scale != 2 && scale != 4) return out;

	int layer_diffuse = -1, layer_teamcolor = -1, layer_emissive = -1;
	for (int i = 0; i != layer_count; ++i) {
		const char* name = (const char*)data + 12 + i * 32;
		if (!strncmp(name, "diffuse", 32)) layer_diffuse = i;
		else if (!strncmp(name, "teamcolor", 32)) layer_teamcolor = i;
		else if (!strncmp(name, "emissive", 32)) layer_emissive = i;
	}
	if (layer_diffuse < 0) return out;

	size_t sp = 12 + 10 * 32;
	int frame_count = rd16(data + sp);
	int ref_id = rd16(data + sp + 2);
	int canvas_w = rd16(data + sp + 4);
	int canvas_h = rd16(data + sp + 6);
	uint32_t frame_arr = rd32(data + sp + 8);
	if (ref_id != 0xffff) return out; // reference sprites not used by main_*.anim
	if (frame_count == 0 || frame_arr + (size_t)frame_count * 16 > size) return out;

	struct tex_t { uint32_t offset, size; int w, h; };
	tex_t texs[10];
	size_t tp = sp + 12;
	if (tp + (size_t)layer_count * 12 > size) return out;
	for (int i = 0; i != layer_count; ++i) {
		const uint8_t* p = data + tp + i * 12;
		texs[i] = { rd32(p), rd32(p + 4), (int)rd16(p + 8), (int)rd16(p + 10) };
		if (texs[i].size && ((size_t)texs[i].offset + texs[i].size > size)) return out;
	}

	auto& dt = texs[layer_diffuse];
	if (!dt.size) return out;
	std::vector<uint32_t> diffuse_full;
	int tw = 0, th = 0;
	if (!decode_dds(data + dt.offset, dt.size, diffuse_full, tw, th)) return out;

	// Frame fields are in HD units (4x logical) for every scale seen so far;
	// auto-detect against the texture dimensions to stay safe.
	int max_ext_x = 0, max_ext_y = 0;
	for (int i = 0; i != frame_count; ++i) {
		const uint8_t* p = data + frame_arr + (size_t)i * 16;
		int ex = rd16(p) + rd16(p + 8), ey = rd16(p + 2) + rd16(p + 10);
		if (ex > max_ext_x) max_ext_x = ex;
		if (ey > max_ext_y) max_ext_y = ey;
	}
	int coord_div = 1; // frame units -> texture px
	while (coord_div < 8 && (max_ext_x / coord_div > tw || max_ext_y / coord_div > th)) coord_div *= 2;
	int units_per_logical = coord_div * scale; // frame units -> logical px (anchor)
	int units_per_atlas = coord_div * ds;      // frame units -> atlas px (render_scale x logical)
	if (units_per_logical < 1 || units_per_atlas < 1) return out;

	// Downscale atlas to render_scale x logical resolution (ds; == logical when
	// render_scale==1).
	detail::downscale_argb(diffuse_full, tw, th, ds, out.diffuse, out.atlas_w, out.atlas_h);
	diffuse_full.clear(); diffuse_full.shrink_to_fit();

	if (layer_teamcolor >= 0 && texs[layer_teamcolor].size) {
		auto& tt = texs[layer_teamcolor];
		std::vector<uint32_t> tc_full;
		int tcw = 0, tch = 0;
		if (decode_dds(data + tt.offset, tt.size, tc_full, tcw, tch) && tcw == tw && tch == th) {
			std::vector<uint32_t> tc_small;
			int sw = 0, sh = 0;
			detail::downscale_argb(tc_full, tcw, tch, ds, tc_small, sw, sh);
			if (sw == out.atlas_w && sh == out.atlas_h) {
				out.teamcolor.resize(tc_small.size());
				for (size_t i = 0; i != tc_small.size(); ++i) {
					// mask strength = luma * alpha
					uint32_t c = tc_small[i];
					uint32_t a = c >> 24;
					uint32_t l = (((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 + (c & 0xff) * 29) >> 8;
					out.teamcolor[i] = (uint8_t)(l * a / 255);
				}
			}
		}
	}

	// open1161HD Faze 7: emissive self-glow layer (same dims as diffuse), decoded
	// and downscaled like teamcolor; composited additively at draw time.
	if (layer_emissive >= 0 && texs[layer_emissive].size) {
		auto& et = texs[layer_emissive];
		std::vector<uint32_t> em_full;
		int ew = 0, eh = 0;
		if (decode_dds(data + et.offset, et.size, em_full, ew, eh) && ew == tw && eh == th) {
			std::vector<uint32_t> em_small;
			int sw = 0, sh = 0;
			detail::downscale_argb(em_full, ew, eh, ds, em_small, sw, sh);
			if (sw == out.atlas_w && sh == out.atlas_h) out.emissive = std::move(em_small);
		}
	}

	out.hd_per_logical = units_per_logical;
	out.canvas_w_hd = canvas_w;
	out.canvas_h_hd = canvas_h;
	out.canvas_w = canvas_w / units_per_logical;
	out.canvas_h = canvas_h / units_per_logical;
	out.frames.resize(frame_count);
	for (int i = 0; i != frame_count; ++i) {
		const uint8_t* p = data + frame_arr + (size_t)i * 16;
		frame_t& f = out.frames[i];
		int u = units_per_atlas; // f.x/y/w/h index the render_scale x logical atlas
		f.x = rd16(p) / u;
		f.y = rd16(p + 2) / u;
		f.x_off_hd = (int16_t)rd16(p + 4);
		f.y_off_hd = (int16_t)rd16(p + 6);
		f.w_hd = rd16(p + 8);
		f.h_hd = rd16(p + 10);
		f.w = (f.w_hd + u - 1) / u;
		f.h = (f.h_hd + u - 1) / u;
		if (f.x + f.w > out.atlas_w) f.w = out.atlas_w - f.x;
		if (f.y + f.h > out.atlas_h) f.h = out.atlas_h - f.y;
		if (f.w < 0) f.w = 0;
		if (f.h < 0) f.h = 0;
	}
	out.ok = true;
	return out;
}

// Load and parse an .anim file from disk.
static inline sprite_t load_file(const std::string& path, int render_scale = 1) {
	sprite_t out;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) { fclose(f); return out; }
	std::vector<uint8_t> buf((size_t)sz);
	bool read_ok = fread(buf.data(), 1, (size_t)sz, f) == (size_t)sz;
	fclose(f);
	if (!read_ok) return out;
	return parse(buf.data(), buf.size(), render_scale);
}

} // namespace anim
} // namespace bwgame

#endif
