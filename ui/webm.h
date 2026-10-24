// webm.h -- read-only WebM / Matroska (EBML) demuxer for SC:R video.
//
// Faze 3.1 of open1161HD: pulls the raw VP9/VP8 compressed frames (and their
// timecodes / keyframe flags / track metadata) out of the .webm containers
// SC:R ships portraits and cinematics in. Ported layer-for-layer from the
// grp-toolkit oracle (tools/webm.py), which is validated across 2483 files.
//
// Container only -- NOT a codec. It hands each block's byte range to the VP9/
// VP8 decoders (Faze 3.2+). Pure presentation; no determinism impact.
//
// Endianness-safe: every multi-byte field is read with explicit byte math.
#ifndef BWGAME_UI_WEBM_H
#define BWGAME_UI_WEBM_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

namespace bwgame {
namespace webm {

struct track_t {
	uint64_t num = 0;
	uint64_t type = 0;          // 1 = video
	std::string codec;          // e.g. "V_VP9", "V_VP8"
	bool has_w = false, has_h = false;
	int w = 0, h = 0;
	uint64_t default_dur = 0;
};

// One SimpleBlock/Block: a compressed frame. offset/size point into the file.
struct block_t {
	uint64_t track = 0;
	int64_t timecode = 0;       // cluster_tc + relative
	bool keyframe = false;
	size_t offset = 0;
	size_t size = 0;
};

struct file_t {
	bool ok = false;
	std::string codec;          // the (first) video track's codec
	bool has_w = false, has_h = false;
	int width = 0, height = 0;
	uint64_t timecode_scale = 1000000;
	bool has_duration = false;
	double duration_raw = 0.0;
	std::string muxer;
	std::vector<track_t> tracks;
	std::vector<block_t> frames;
};

namespace detail {

// Matroska master elements we descend into (mirrors webm.py MASTER). 0xA0 =
// BlockGroup: without it, files that wrap a Block(0xA1) in a BlockGroup instead
// of a bare SimpleBlock parse to ZERO frames.
static inline bool is_master(uint64_t id) {
	switch (id) {
	case 0x1A45DFA3: case 0x18538067: case 0x1549A966: case 0x1654AE6B:
	case 0xAE: case 0xE0: case 0x1F43B675: case 0x1C53BB6B: case 0xBB:
	case 0xB7: case 0xA0:
		return true;
	default:
		return false;
	}
}

// EBML variable-length integer. IDs keep their length marker, sizes drop it.
// Returns false on EOF / invalid. `unknown` set when an all-ones size means
// "unknown length".
static inline bool vint(const uint8_t* d, size_t len, size_t& p,
                        bool keep_marker, uint64_t& out, bool& unknown) {
	unknown = false;
	if (p >= len) return false;
	uint8_t b = d[p];
	if (b == 0) return false;
	int n = 1;
	uint8_t mask = 0x80;
	while (!(b & mask)) { mask >>= 1; ++n; }
	if (p + n > len) return false;
	uint64_t v = keep_marker ? b : (uint64_t)(b & (mask - 1));
	for (int i = 1; i < n; ++i) v = (v << 8) | d[p + i];
	if (!keep_marker && n <= 8 && v == (((uint64_t)1 << (7 * n)) - 1)) unknown = true;
	p += n;
	out = v;
	return true;
}

struct parser {
	const uint8_t* d;
	size_t len;
	file_t* f;
	track_t* cur = nullptr;     // current TrackEntry being filled
	int64_t cluster_tc = 0;

	uint64_t uint_at(size_t p, size_t size) {
		uint64_t v = 0;
		for (size_t i = 0; i < size; ++i) v = (v << 8) | d[p + i];
		return v;
	}
	double flt_at(size_t p, size_t size) {
		if (size == 4) {
			uint32_t u = (uint32_t)uint_at(p, 4);
			float fv;
			memcpy(&fv, &u, 4);
			return fv;
		}
		if (size == 8) {
			uint64_t u = uint_at(p, 8);
			double dv;
			memcpy(&dv, &u, 8);
			return dv;
		}
		return 0.0;
	}

	void leaf(uint64_t eid, size_t p, size_t size) {
		switch (eid) {
		case 0x2AD7B1: f->timecode_scale = uint_at(p, size); return;         // TimecodeScale
		case 0x4489: f->duration_raw = flt_at(p, size); f->has_duration = true; return; // Duration
		case 0x4D80: if (f->muxer.empty()) f->muxer.assign((const char*)d + p, size); return; // MuxingApp
		case 0xE7: cluster_tc = (int64_t)uint_at(p, size); return;           // Cluster Timecode
		default: break;
		}
		if (cur) {
			switch (eid) {
			case 0xD7: cur->num = uint_at(p, size); return;                  // TrackNumber
			case 0x83: cur->type = uint_at(p, size); return;                 // TrackType
			case 0x86: cur->codec.assign((const char*)d + p, size); return;  // CodecID
			case 0xB0: cur->w = (int)uint_at(p, size); cur->has_w = true; return; // PixelWidth
			case 0xBA: cur->h = (int)uint_at(p, size); cur->has_h = true; return; // PixelHeight
			case 0x23E383: cur->default_dur = uint_at(p, size); return;      // DefaultDuration
			default: return;
			}
		}
		if (eid == 0xA3 || eid == 0xA1) { // SimpleBlock / Block
			size_t q = p;
			uint64_t tn; bool unk;
			if (!vint(d, p + size, q, false, tn, unk)) return;
			if (q + 3 > p + size) return;
			int16_t rel = (int16_t)((d[q] << 8) | d[q + 1]);
			uint8_t flags = d[q + 2];
			block_t blk;
			blk.track = tn;
			blk.timecode = cluster_tc + rel;
			blk.keyframe = (flags & 0x80) != 0;
			blk.offset = q + 3;
			blk.size = size - (q + 3 - p);
			f->frames.push_back(blk);
		}
	}

	void walk(size_t p, size_t end) {
		while (p < end) {
			uint64_t eid, size; bool unk_id, unk_sz;
			size_t p2 = p;
			if (!vint(d, end, p2, true, eid, unk_id)) return;
			size_t p3 = p2;
			if (!vint(d, end, p3, false, size, unk_sz)) return;
			if (unk_sz) { walk(p3, end); return; } // unknown-size master
			size_t body_end = p3 + size;
			if (body_end > end) body_end = end;
			if (is_master(eid)) {
				if (eid == 0xAE) {                 // TrackEntry
					f->tracks.push_back(track_t());
					cur = &f->tracks.back();
					walk(p3, body_end);
					cur = nullptr;
				} else {
					walk(p3, body_end);
				}
			} else {
				leaf(eid, p3, size);
			}
			p = body_end;
		}
	}
};

} // namespace detail

// Parse a whole .webm image. Returns file_t{ok=false} if not EBML.
static inline file_t parse(const uint8_t* data, size_t size) {
	file_t f;
	if (size < 4 || data[0] != 0x1a || data[1] != 0x45 || data[2] != 0xdf || data[3] != 0xa3)
		return f;
	detail::parser p{ data, size, &f };
	p.walk(0, size);
	// first video track drives codec/dims (webm.py: type==1)
	for (const track_t& t : f.tracks) {
		if (t.type == 1) {
			f.codec = t.codec;
			f.has_w = t.has_w; f.width = t.w;
			f.has_h = t.has_h; f.height = t.h;
			break;
		}
	}
	f.ok = true;
	return f;
}

// A VP9 "superframe" packs several frames in one block. Returns the sub-frame
// byte ranges (just the whole block when there is no index). Mirrors
// webm.py:vp9_superframe_split.
static inline std::vector<std::pair<size_t, size_t>> vp9_superframe_split(
		const uint8_t* buf, size_t n) {
	std::vector<std::pair<size_t, size_t>> out;
	if (n == 0) return out;
	uint8_t m = buf[n - 1];
	if ((m & 0xE0) != 0xC0) { out.push_back({ 0, n }); return out; }
	int frames = (m & 0x7) + 1;
	int mag = ((m >> 3) & 3) + 1;
	size_t idx_sz = 2 + (size_t)mag * frames;
	if (n < idx_sz || buf[n - idx_sz] != m) { out.push_back({ 0, n }); return out; }
	size_t p = n - idx_sz + 1;
	size_t off = 0;
	for (int i = 0; i < frames; ++i) {
		size_t sz = 0;
		for (int k = 0; k < mag; ++k) sz |= (size_t)buf[p + k] << (8 * k);
		p += mag;
		out.push_back({ off, off + sz });
		off += sz;
	}
	return out;
}

} // namespace webm
} // namespace bwgame

#endif
