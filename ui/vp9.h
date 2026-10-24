// vp9.h -- VP9 header layer (Faze 3.2a/b of open1161HD).
//
// Ported from the grp-toolkit oracle (tools/vp9.py), written from the VP9
// Bitstream & Decoding Process Specification v0.6. Two pieces, both validated
// without the big constant tables:
//   * VP9HeaderParser -- the UNCOMPRESSED frame header (both profiles; key /
//     intra-only / inter), stateful across a sequence (an inter frame inherits
//     size and colour config from reference slots).
//   * BoolDecoder + parse_compressed_header -- the arithmetic decoder and the
//     whole probability-update structure of the COMPRESSED header. Its
//     conformance oracle is the exit padding: after the header the remaining
//     bits must be zero. The bit count each diff_update_prob consumes does not
//     depend on the current probability value, so this is exact without the
//     default prob tables (those belong to 3.2c pixel/entropy decode).
//
// Pure presentation; no determinism impact. Container/superframe split lives in
// ui/webm.h (webm::vp9_superframe_split).
#ifndef BWGAME_UI_VP9_H
#define BWGAME_UI_VP9_H

#include <cstdint>
#include <cstddef>
#include <array>

namespace bwgame {
namespace vp9 {

enum { KEY_FRAME = 0, CS_RGB = 7, LAST_FRAME = 1,
       MAX_SEGMENTS = 8, SEG_LVL_MAX = 4,
       MIN_TILE_WIDTH_B64 = 4, MAX_TILE_WIDTH_B64 = 64,
       REFS_PER_FRAME = 3, NUM_REF_FRAMES = 8 };

// read_interpolation_filter literal_to_type: SMOOTH, EIGHTTAP, SHARP, BILINEAR
static const int INTERP_LITERAL_TO_TYPE[4] = { 1, 0, 2, 3 };
static const int SEG_FEATURE_BITS[4] = { 8, 6, 2, 0 };
static const int SEG_FEATURE_SIGNED[4] = { 1, 1, 0, 0 };

enum { INTERP_SWITCHABLE = 99 }; // sentinel; else 0..3 = literal_to_type value

struct header_t {
	bool ok = false;
	int profile = 0;
	int show_existing = -1;         // -1 = not a show_existing_frame
	bool keyframe = false;
	int show_frame = 0;
	int error_resilient = 0;
	bool intra_only = false;
	bool frame_is_intra = true;
	int width = 0, height = 0;
	int sb64_cols = 0, sb64_rows = 0;
	int color_space = 1;            // CS_BT_601
	int ss_x = 1, ss_y = 1;         // subsampling
	int bit_depth = 8;
	int color_range = 0;
	int base_q_idx = 0;
	int delta_q_y_dc = 0, delta_q_uv_dc = 0, delta_q_uv_ac = 0;
	bool lossless = true;
	int loop_filter_level = 0, loop_filter_sharpness = 0;
	int loop_filter_delta_enabled = 0;
	int loop_filter_ref_deltas[4] = { 1, 0, -1, -1 }; // setup_past_independence
	int loop_filter_mode_deltas[2] = { 0, 0 };
	int refresh_frame_flags = 0;
	int frame_context_idx = 0;
	int refresh_frame_context = 0, frame_parallel_decoding_mode = 1;
	int tile_cols_log2 = 0, tile_rows_log2 = 0;
	int allow_high_precision_mv = 0;
	int interp_filter = 0;
	bool has_interp = false;
	int ref_frame_idx[3] = { 0, 0, 0 };   // inter: LAST/GOLDEN/ALTREF ref slots
	int ref_frame_sign_bias[4] = { 0, 0, 0, 0 };
	int header_size_in_bytes = 0;
	int uncompressed_bytes = 0;
};

// MSB-first bit reader for the uncompressed header (f(n)/s(n)).
struct bits_t {
	const uint8_t* d;
	size_t len;
	size_t pos = 0;
	bits_t(const uint8_t* data, size_t n) : d(data), len(n) {}
	int f(int n) {
		int v = 0;
		for (int i = 0; i < n; ++i) {
			int byte = (pos >> 3) < len ? d[pos >> 3] : 0;
			v = (v << 1) | ((byte >> (7 - (pos & 7))) & 1);
			++pos;
		}
		return v;
	}
	int s(int n) { int v = f(n); return f(1) ? -v : v; }
	int byte_pos() const { return (int)((pos + 7) >> 3); }
};

class header_parser {
public:
	header_parser() { for (int i = 0; i < NUM_REF_FRAMES; ++i) ref_w[i] = ref_h[i] = 0; }

	header_t parse(const uint8_t* buf, size_t n) {
		header_t h;
		bits_t b(buf, n);
		if (b.f(2) != 2) return h;               // frame_marker
		int prof = b.f(1) | (b.f(1) << 1);
		if (prof == 3) b.f(1);                   // reserved_zero
		h.profile = prof;
		if (b.f(1)) {                            // show_existing_frame
			h.show_existing = b.f(3);
			h.header_size_in_bytes = 0;
			h.ok = true;
			return h;
		}
		int ft = b.f(1);
		h.keyframe = (ft == KEY_FRAME);
		h.show_frame = b.f(1);
		h.error_resilient = b.f(1);
		h.intra_only = (ft == KEY_FRAME);
		int refresh = 0;

		if (ft == KEY_FRAME) {
			if (!sync(b)) return h;
			color_config(b, prof, h);
			frame_size(b, h);
			render_size(b, h);
			refresh = 0xFF;
			h.frame_is_intra = true;
		} else {
			int intra_only = (h.show_frame == 0) ? b.f(1) : 0;
			h.intra_only = intra_only != 0;
			h.frame_is_intra = intra_only != 0;
			if (h.error_resilient == 0) b.f(2);  // reset_frame_context
			if (intra_only) {
				if (!sync(b)) return h;
				if (prof > 0) {
					color_config(b, prof, h);
				} else {
					h.color_space = 1; h.ss_x = 1; h.ss_y = 1; h.bit_depth = 8;
				}
				refresh = b.f(8);
				frame_size(b, h);
				render_size(b, h);
			} else {
				refresh = b.f(8);
				int idx[REFS_PER_FRAME];
				for (int i = 0; i < REFS_PER_FRAME; ++i) {
					idx[i] = b.f(3);
					h.ref_frame_idx[i] = idx[i];
					h.ref_frame_sign_bias[LAST_FRAME + i] = b.f(1);
				}
				frame_size_with_refs(b, h, idx);
				h.allow_high_precision_mv = b.f(1);
				interp(b, h);
			}
		}

		if (h.error_resilient == 0) {
			h.refresh_frame_context = b.f(1);
			h.frame_parallel_decoding_mode = b.f(1);
		} else {
			h.refresh_frame_context = 0;
			h.frame_parallel_decoding_mode = 1;
		}
		h.frame_context_idx = b.f(2);
		h.refresh_frame_flags = refresh;
		loop_filter(b, h);
		quant(b, h);
		segmentation(b, h);
		tile_info(b, h);
		h.header_size_in_bytes = b.f(16);

		for (int i = 0; i < NUM_REF_FRAMES; ++i)
			if (refresh & (1 << i)) { ref_w[i] = h.width; ref_h[i] = h.height; }

		// persist / inherit colour config across the sequence
		if (h.frame_is_intra && (ft == KEY_FRAME || h.intra_only)) {
			// color_config was read this frame (or defaulted for prof0 intra)
			ss_x = h.ss_x; ss_y = h.ss_y; bit_depth = h.bit_depth; color_space = h.color_space;
		} else {
			h.ss_x = ss_x; h.ss_y = ss_y; h.bit_depth = bit_depth; h.color_space = color_space;
		}
		h.uncompressed_bytes = b.byte_pos();
		h.ok = true;
		return h;
	}

private:
	int ref_w[NUM_REF_FRAMES], ref_h[NUM_REF_FRAMES];
	int ss_x = 1, ss_y = 1, bit_depth = 8, color_space = 1; // sequence colour state

	bool sync(bits_t& b) { return b.f(8) == 0x49 && b.f(8) == 0x83 && b.f(8) == 0x42; }

	void color_config(bits_t& b, int prof, header_t& h) {
		h.bit_depth = (prof >= 2) ? (b.f(1) ? 12 : 10) : 8;
		int cs = b.f(3);
		h.color_space = cs;
		if (cs != CS_RGB) {
			h.color_range = b.f(1);
			if (prof == 1 || prof == 3) { h.ss_x = b.f(1); h.ss_y = b.f(1); b.f(1); }
			else { h.ss_x = 1; h.ss_y = 1; }
		} else {
			h.color_range = 1;
			if (prof == 1 || prof == 3) { h.ss_x = 0; h.ss_y = 0; b.f(1); }
			else { h.ss_x = 0; h.ss_y = 0; }
		}
	}
	void image_size(header_t& h) {
		int mi_cols = (h.width + 7) >> 3, mi_rows = (h.height + 7) >> 3;
		h.sb64_cols = (mi_cols + 7) >> 3;
		h.sb64_rows = (mi_rows + 7) >> 3;
	}
	void frame_size(bits_t& b, header_t& h) {
		h.width = b.f(16) + 1; h.height = b.f(16) + 1; image_size(h);
	}
	void render_size(bits_t& b, header_t& h) {
		if (b.f(1)) { b.f(16); b.f(16); }
	}
	void frame_size_with_refs(bits_t& b, header_t& h, const int* idx) {
		int found = 0;
		for (int i = 0; i < REFS_PER_FRAME; ++i) {
			found = b.f(1);
			if (found) { h.width = ref_w[idx[i]]; h.height = ref_h[idx[i]]; break; }
		}
		if (!found) { h.width = b.f(16) + 1; h.height = b.f(16) + 1; }
		image_size(h);
		render_size(b, h);
	}
	void interp(bits_t& b, header_t& h) {
		h.has_interp = true;
		if (b.f(1)) h.interp_filter = INTERP_SWITCHABLE;
		else h.interp_filter = INTERP_LITERAL_TO_TYPE[b.f(2)];
	}
	void loop_filter(bits_t& b, header_t& h) {
		h.loop_filter_level = b.f(6);
		h.loop_filter_sharpness = b.f(3);
		int enabled = b.f(1);        // delta_enabled
		h.loop_filter_delta_enabled = enabled;
		if (enabled) {
			if (b.f(1)) {            // delta_update
				for (int i = 0; i < 4; ++i) if (b.f(1)) h.loop_filter_ref_deltas[i] = b.s(6);
				for (int i = 0; i < 2; ++i) if (b.f(1)) h.loop_filter_mode_deltas[i] = b.s(6);
			}
		}
	}
	int read_delta_q(bits_t& b) { return b.f(1) ? b.s(4) : 0; }
	void quant(bits_t& b, header_t& h) {
		h.base_q_idx = b.f(8);
		h.delta_q_y_dc = read_delta_q(b);
		h.delta_q_uv_dc = read_delta_q(b);
		h.delta_q_uv_ac = read_delta_q(b);
		h.lossless = (h.base_q_idx == 0 && h.delta_q_y_dc == 0 &&
		              h.delta_q_uv_dc == 0 && h.delta_q_uv_ac == 0);
	}
	int read_prob(bits_t& b) { return b.f(1) ? b.f(8) : 255; }
	void segmentation(bits_t& b, header_t& h) {
		int enabled = b.f(1);
		if (!enabled) return;
		if (b.f(1)) {                            // update_map
			for (int i = 0; i < 7; ++i) read_prob(b);
			int temporal = b.f(1);
			for (int i = 0; i < 3; ++i) if (temporal) read_prob(b);
		}
		if (b.f(1)) {                            // update_data
			b.f(1);                              // abs_or_delta_update
			for (int s = 0; s < MAX_SEGMENTS; ++s)
				for (int j = 0; j < SEG_LVL_MAX; ++j)
					if (b.f(1)) {
						int bits = SEG_FEATURE_BITS[j];
						if (bits) b.f(bits);
						if (SEG_FEATURE_SIGNED[j]) b.f(1);
					}
		}
	}
	void tile_info(bits_t& b, header_t& h) {
		int sb64_cols = h.sb64_cols;
		int min_log2 = 0;
		while ((MAX_TILE_WIDTH_B64 << min_log2) < sb64_cols) ++min_log2;
		int max_log2 = 1;
		while ((sb64_cols >> max_log2) >= MIN_TILE_WIDTH_B64) ++max_log2;
		--max_log2;
		int cols_log2 = min_log2;
		while (cols_log2 < max_log2) { if (b.f(1)) ++cols_log2; else break; }
		int rows_log2 = b.f(1);
		if (rows_log2 == 1) rows_log2 += b.f(1);
		h.tile_cols_log2 = cols_log2;
		h.tile_rows_log2 = rows_log2;
	}
};

// ---- compressed header: arithmetic decoder + probability-update structure ---
enum { TX_SIZE_CONTEXTS = 2, TX_SIZES = 4, TX_MODE_SELECT = 4,
       SKIP_CONTEXTS = 3, INTER_MODE_CONTEXTS = 7, INTER_MODES = 4,
       INTERP_FILTER_CONTEXTS = 4, SWITCHABLE_FILTERS = 3, IS_INTER_CONTEXTS = 4,
       COMP_MODE_CONTEXTS = 5, REF_CONTEXTS = 5, BLOCK_SIZE_GROUPS = 4,
       INTRA_MODES = 10, PARTITION_CONTEXTS = 16, PARTITION_TYPES = 4,
       MV_JOINTS = 4, MV_CLASSES = 11, CLASS0_SIZE = 2, MV_OFFSET_BITS = 10,
       MV_FR_SIZE = 4, SINGLE_REFERENCE = 0, COMPOUND_REFERENCE = 1,
       REFERENCE_MODE_SELECT = 2 };
static const int TX_MODE_TO_BIGGEST[5] = { 0, 1, 2, 3, 3 };

struct bool_decoder {
	const uint8_t* d;
	size_t len;
	size_t pos, end;
	int value, rng;
	long bits_left;

	bool_decoder(const uint8_t* data, size_t datalen, int start, int sz)
		: d(data), len(datalen) {
		pos = (size_t)start * 8;
		end = (size_t)(start + sz) * 8;
		value = f(8);
		rng = 255;
		bits_left = 8L * sz - 8;
		read_bool(128); // init_bool marker bool (must be 0)
	}
	int f(int n) {
		int v = 0;
		for (int i = 0; i < n; ++i) {
			int bit = 0;
			if (pos < end && (pos >> 3) < len)
				bit = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
			v = (v << 1) | bit;
			++pos;
		}
		return v;
	}
	int read_bool(int p) {
		int split = 1 + (((rng - 1) * p) >> 8);
		int bit;
		if (value < split) { rng = split; bit = 0; }
		else { rng -= split; value -= split; bit = 1; }
		while (rng < 128) {
			int nb = 0;
			if (bits_left > 0) { nb = f(1); --bits_left; }
			rng <<= 1;
			value = (value << 1) + nb;
		}
		return bit;
	}
	int read_literal(int n) {
		int x = 0;
		for (int i = 0; i < n; ++i) x = (x << 1) | read_bool(128);
		return x;
	}
	int exit_padding_nonzero() {
		int bad = 0;
		while (bits_left > 0) { if (f(1)) ++bad; --bits_left; }
		return bad;
	}
};

static inline void decode_term_subexp(bool_decoder& bd) {
	if (bd.read_literal(1) == 0) { bd.read_literal(4); return; }
	if (bd.read_literal(1) == 0) { bd.read_literal(4); return; }
	if (bd.read_literal(1) == 0) { bd.read_literal(5); return; }
	int v = bd.read_literal(7);
	if (v < 65) return;
	bd.read_literal(1);
}
static inline void diff_update_prob(bool_decoder& bd) {
	if (bd.read_bool(252)) decode_term_subexp(bd);
}
static inline void update_mv_prob(bool_decoder& bd) {
	if (bd.read_bool(252)) bd.read_literal(7);
}

// Parse the compressed header; return the number of non-zero padding bits after
// it (0 == clean, in-sync parse), or -1 on over-read. Mirrors vp9.py.
static inline int parse_compressed_header(const uint8_t* data, size_t datalen,
                                          const header_t& h) {
	bool_decoder bd(data, datalen, h.uncompressed_bytes, h.header_size_in_bytes);

	int tx_mode;
	if (h.lossless) tx_mode = 0;
	else { tx_mode = bd.read_literal(2); if (tx_mode == 3) tx_mode += bd.read_literal(1); }
	if (tx_mode == TX_MODE_SELECT) {
		for (int c = 0; c < TX_SIZE_CONTEXTS; ++c) for (int i = 0; i < TX_SIZES - 3; ++i) diff_update_prob(bd);
		for (int c = 0; c < TX_SIZE_CONTEXTS; ++c) for (int i = 0; i < TX_SIZES - 2; ++i) diff_update_prob(bd);
		for (int c = 0; c < TX_SIZE_CONTEXTS; ++c) for (int i = 0; i < TX_SIZES - 1; ++i) diff_update_prob(bd);
	}

	int max_tx = TX_MODE_TO_BIGGEST[tx_mode];
	for (int t = 0; t <= max_tx; ++t) {
		if (bd.read_literal(1)) {
			for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j)
				for (int k = 0; k < 6; ++k) {
					int maxl = (k == 0) ? 3 : 6;
					for (int l = 0; l < maxl; ++l) for (int m = 0; m < 3; ++m) diff_update_prob(bd);
				}
		}
	}

	for (int i = 0; i < SKIP_CONTEXTS; ++i) diff_update_prob(bd);

	if (!h.frame_is_intra) {
		for (int c = 0; c < INTER_MODE_CONTEXTS; ++c) for (int i = 0; i < INTER_MODES - 1; ++i) diff_update_prob(bd);
		if (h.interp_filter == INTERP_SWITCHABLE)
			for (int c = 0; c < INTERP_FILTER_CONTEXTS; ++c) for (int i = 0; i < SWITCHABLE_FILTERS - 1; ++i) diff_update_prob(bd);
		for (int i = 0; i < IS_INTER_CONTEXTS; ++i) diff_update_prob(bd);

		bool compound_allowed = false;
		for (int i = 1; i < REFS_PER_FRAME; ++i)
			if (h.ref_frame_sign_bias[i + 1] != h.ref_frame_sign_bias[1]) compound_allowed = true;
		int ref_mode;
		if (compound_allowed) {
			if (bd.read_literal(1) == 0) ref_mode = SINGLE_REFERENCE;
			else ref_mode = (bd.read_literal(1) == 0) ? COMPOUND_REFERENCE : REFERENCE_MODE_SELECT;
		} else ref_mode = SINGLE_REFERENCE;
		if (ref_mode == REFERENCE_MODE_SELECT)
			for (int i = 0; i < COMP_MODE_CONTEXTS; ++i) diff_update_prob(bd);
		if (ref_mode != COMPOUND_REFERENCE)
			for (int i = 0; i < REF_CONTEXTS; ++i) { diff_update_prob(bd); diff_update_prob(bd); }
		if (ref_mode != SINGLE_REFERENCE)
			for (int i = 0; i < REF_CONTEXTS; ++i) diff_update_prob(bd);

		for (int g = 0; g < BLOCK_SIZE_GROUPS; ++g) for (int i = 0; i < INTRA_MODES - 1; ++i) diff_update_prob(bd);
		for (int c = 0; c < PARTITION_CONTEXTS; ++c) for (int i = 0; i < PARTITION_TYPES - 1; ++i) diff_update_prob(bd);

		for (int i = 0; i < MV_JOINTS - 1; ++i) update_mv_prob(bd);
		for (int i = 0; i < 2; ++i) {
			update_mv_prob(bd);                                  // sign
			for (int j = 0; j < MV_CLASSES - 1; ++j) update_mv_prob(bd); // class
			update_mv_prob(bd);                                  // class0_bit
			for (int j = 0; j < MV_OFFSET_BITS; ++j) update_mv_prob(bd); // bits
		}
		for (int i = 0; i < 2; ++i) {
			for (int j = 0; j < CLASS0_SIZE; ++j) for (int k = 0; k < MV_FR_SIZE - 1; ++k) update_mv_prob(bd);
			for (int k = 0; k < MV_FR_SIZE - 1; ++k) update_mv_prob(bd);
		}
		if (h.allow_high_precision_mv)
			for (int i = 0; i < 2; ++i) { update_mv_prob(bd); update_mv_prob(bd); }
	}

	if (bd.bits_left < 0) return -1;
	return bd.exit_padding_nonzero();
}

} // namespace vp9
} // namespace bwgame

#endif
