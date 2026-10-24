// vp8.h -- VP8 boolean entropy decoder + keyframe header (Faze 3.x-a of
// open1161HD). Ported from the grp-toolkit oracle tools/vp8.py (RFC 6386
// "VP8 Data Format and Decoding Guide"). SC:R carbot portraits are VP8; the
// full pixel decoder is ui/vp8_decode.h. Pure presentation; no determinism.
#ifndef BWGAME_UI_VP8_H
#define BWGAME_UI_VP8_H

#include <cstdint>
#include <cstddef>

namespace bwgame {
namespace vp8 {

// RFC 6386 §7 range coder. range stays in 128..255; value holds the position
// inside the interval; bit_count tracks shifts since the last byte refill.
struct bool_decoder {
	const uint8_t* d;
	size_t len, p;
	uint32_t value;
	int range, bit_count;

	bool_decoder(const uint8_t* data, size_t datalen, size_t start = 0)
		: d(data), len(datalen), p(start), value(0), range(255), bit_count(0) {
		for (int i = 0; i < 2; ++i) value = (value << 8) | next();
	}
	uint32_t next() {
		if (p < len) return d[p++];
		++p; return 0; // past the end reads as zero
	}
	int get_bool(int prob) {
		int split = 1 + (((range - 1) * prob) >> 8);
		uint32_t SPLIT = (uint32_t)split << 8;
		int ret;
		if (value >= SPLIT) { ret = 1; range -= split; value -= SPLIT; }
		else { ret = 0; range = split; }
		while (range < 128) {
			value <<= 1; range <<= 1;
			if (++bit_count == 8) { bit_count = 0; value |= next(); }
		}
		return ret;
	}
	int literal(int n) { int v = 0; for (int i = 0; i < n; ++i) v = (v << 1) | get_bool(128); return v; }
	int signed_lit(int n) { int v = literal(n); return get_bool(128) ? -v : v; }
	int flagged(int n) { return get_bool(128) ? signed_lit(n) : 0; }
	size_t bytes_used() const { return p; }
};

struct frame_tag {
	bool ok = false;
	bool keyframe = false;
	int version = 0;
	bool show_frame = false;
	int first_part_size = 0;
};

static inline frame_tag parse_frame_tag(const uint8_t* d, size_t n) {
	frame_tag t;
	if (n < 3) return t;
	uint32_t v = d[0] | (d[1] << 8) | (d[2] << 16);
	t.ok = true;
	t.keyframe = (v & 1) == 0;
	t.version = (v >> 1) & 7;
	t.show_frame = ((v >> 4) & 1) != 0;
	t.first_part_size = (v >> 5) & 0x7FFFF;
	return t;
}

struct kf_header {
	bool ok = false;
	bool keyframe = false;
	int version = 0;
	bool show_frame = false;
	int first_part_size = 0;
	int width = 0, horiz_scale = 0, height = 0, vert_scale = 0;
	int color_space = 0, clamping_type = 0;
	int segmentation_enabled = 0, update_mb_seg_map = 0;
	int segment_tree_probs[3] = { 255, 255, 255 };
	int filter_type = 0, loop_filter_level = 0, sharpness_level = 0;
	int lf_adj_enable = 0;
	int ref_lf_deltas[4] = { 0, 0, 0, 0 };
	int mode_lf_deltas[4] = { 0, 0, 0, 0 };
	int nbr_partitions = 1;
	int y_ac_qi = 0, y_dc_delta = 0, y2_dc_delta = 0, y2_ac_delta = 0, uv_dc_delta = 0, uv_ac_delta = 0;
	int refresh_entropy_probs = 0;
	int header_bytes_used = 0;
};

// Uncompressed chunk + bool-coded header up to refresh_entropy_probs (stage a/b).
static inline kf_header parse_keyframe(const uint8_t* data, size_t n) {
	kf_header h;
	frame_tag tag = parse_frame_tag(data, n);
	if (!tag.ok || !tag.keyframe) return h;
	if (n < 10 || data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a) return h;
	h.keyframe = true; h.version = tag.version; h.show_frame = tag.show_frame;
	h.first_part_size = tag.first_part_size;
	int w_raw = data[6] | (data[7] << 8);
	int h_raw = data[8] | (data[9] << 8);
	h.width = w_raw & 0x3FFF; h.horiz_scale = w_raw >> 14;
	h.height = h_raw & 0x3FFF; h.vert_scale = h_raw >> 14;

	bool_decoder b(data, n, 10);
	h.color_space = b.literal(1);
	h.clamping_type = b.literal(1);
	h.segmentation_enabled = b.literal(1);
	if (h.segmentation_enabled) {
		int update_map = h.update_mb_seg_map = b.literal(1);
		int update_data = b.literal(1);
		if (update_data) {
			b.literal(1);                               // segment_feature_mode
			for (int i = 0; i < 4; ++i) b.flagged(7);   // segment_quant
			for (int i = 0; i < 4; ++i) b.flagged(6);   // segment_lf
		}
		if (update_map)
			for (int i = 0; i < 3; ++i) h.segment_tree_probs[i] = b.get_bool(128) ? b.literal(8) : 255;
	}
	h.filter_type = b.literal(1);
	h.loop_filter_level = b.literal(6);
	h.sharpness_level = b.literal(3);
	h.lf_adj_enable = b.literal(1);
	if (h.lf_adj_enable) {
		if (b.literal(1)) {                             // mode_ref_lf_delta_update
			for (int i = 0; i < 4; ++i) h.ref_lf_deltas[i] = b.flagged(6);
			for (int i = 0; i < 4; ++i) h.mode_lf_deltas[i] = b.flagged(6);
		}
	}
	h.nbr_partitions = 1 << b.literal(2);
	h.y_ac_qi = b.literal(7);
	h.y_dc_delta = b.flagged(4);
	h.y2_dc_delta = b.flagged(4);
	h.y2_ac_delta = b.flagged(4);
	h.uv_dc_delta = b.flagged(4);
	h.uv_ac_delta = b.flagged(4);
	h.refresh_entropy_probs = b.literal(1);
	h.header_bytes_used = (int)b.bytes_used();
	h.ok = true;
	return h;
}

} // namespace vp8
} // namespace bwgame

#endif
