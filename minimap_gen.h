#ifndef OPENBW_MINIMAP_GEN_H
#define OPENBW_MINIMAP_GEN_H

// v0.53: standalone minimap PNG generator for the launcher's map preview.
// Renders a map's terrain minimap to an RGB8 PNG using only the header-only
// MPQ/data-loading layer (no engine state, no SDL). The per-tile colour is
// the same lookup ui.h draw_minimap_into() uses: mega tile -> vx4 image ->
// vr4 pixel 55 -> tileset wpe palette. Used by both tools/aidump (mode
// "minimap") and the app's libopenbw_minimap JNI wrapper.

#include "containers.h"
#include "strf.h"
#include "data_loading.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace bwgame {
namespace minimap_gen {

// Core: given raw CHK bytes and a data loader that can resolve Tileset/* files,
// render the minimap PNG. Returns true on success; err holds a reason on fail.
inline bool render_chk_to_png(data_loading::data_files_loader<>& load_data_file,
                              const a_vector<uint8_t>& chk, const char* outpng,
                              a_string& err) {
	if (chk.size() < 8) { err = "CHK too small"; return false; }

	auto rd16 = [](const uint8_t* p) -> uint16_t { return (uint16_t)(p[0] | (p[1] << 8)); };
	auto rd32 = [](const uint8_t* p) -> uint32_t { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };

	// Walk CHK chunks for DIM (dimensions), ERA (tileset), MTXM (tiles).
	int mw = 0, mh = 0, tileset = 0;
	a_vector<uint16_t> tiles;
	{
		size_t pos = 0;
		while (pos + 8 <= chk.size()) {
			const uint8_t* tag = chk.data() + pos;
			uint32_t len = rd32(chk.data() + pos + 4);
			pos += 8;
			if (len > chk.size() - pos) len = (uint32_t)(chk.size() - pos);
			const uint8_t* body = chk.data() + pos;
			if (!memcmp(tag, "DIM ", 4) && len >= 4) { mw = rd16(body); mh = rd16(body + 2); }
			else if (!memcmp(tag, "ERA ", 4) && len >= 2) { tileset = rd16(body) % 8; }
			else if (!memcmp(tag, "MTXM", 4)) {
				size_t n = len / 2;
				tiles.resize(n);
				for (size_t i = 0; i != n; ++i) tiles[i] = rd16(body + i * 2);
			}
			pos += len;
		}
	}
	if (mw <= 0 || mh <= 0) { err = "no DIM chunk"; return false; }
	if ((int)tiles.size() < mw * mh) { err = "MTXM too small"; return false; }

	// Load the tileset graphics for the colour lookup.
	static const char* tileset_names[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};
	a_vector<uint8_t> cv5, vx4, vr4, wpe;
	try {
		load_data_file(cv5, format("Tileset/%s.cv5", tileset_names[tileset]));
		load_data_file(vx4, format("Tileset/%s.vx4", tileset_names[tileset]));
		load_data_file(vr4, format("Tileset/%s.vr4", tileset_names[tileset]));
		load_data_file(wpe, format("Tileset/%s.wpe", tileset_names[tileset]));
	} catch (std::exception& e) {
		err = a_string("tileset load failed: ") + e.what();
		return false;
	}
	if (wpe.size() < 256 * 4) { err = "bad wpe"; return false; }
	size_t cv5_n = cv5.size() / 52;
	size_t vx4_n = vx4.size() / 32;
	size_t vr4_n = vr4.size() / 64;

	// Build the RGB pixel grid (top-down).
	std::vector<uint8_t> rgb((size_t)mw * mh * 3, 0);
	for (int y = 0; y != mh; ++y) {
		for (int x = 0; x != mw; ++x) {
			uint16_t t = tiles[(size_t)y * mw + x];
			size_t g = (t >> 4) & 0x7ff;
			size_t s = t & 0xf;
			size_t mti = 0;
			if (g < cv5_n) mti = rd16(cv5.data() + g * 52 + 20 + s * 2);
			uint8_t val = 0;
			if (mti < vx4_n) {
				size_t vr4i = rd16(vx4.data() + mti * 32) >> 1;
				if (vr4i < vr4_n) val = vr4[vr4i * 64 + 55];
			}
			uint8_t* px = rgb.data() + ((size_t)y * mw + x) * 3;
			px[0] = wpe[val * 4 + 0];
			px[1] = wpe[val * 4 + 1];
			px[2] = wpe[val * 4 + 2];
		}
	}

	// Encode a PNG (RGB8) using stored-deflate - no zlib dependency.
	auto crc32_of = [](const uint8_t* p, size_t n, uint32_t crc) -> uint32_t {
		crc = ~crc;
		for (size_t i = 0; i != n; ++i) {
			crc ^= p[i];
			for (int k = 0; k != 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
		}
		return ~crc;
	};
	std::vector<uint8_t> raw; // filtered scanlines: 1 filter byte + RGB row
	raw.reserve((size_t)mh * (1 + mw * 3));
	for (int y = 0; y != mh; ++y) {
		raw.push_back(0);
		const uint8_t* row = rgb.data() + (size_t)y * mw * 3;
		raw.insert(raw.end(), row, row + mw * 3);
	}
	std::vector<uint8_t> zlib;
	zlib.push_back(0x78); zlib.push_back(0x01);
	{
		size_t off = 0;
		while (off < raw.size()) {
			size_t block = std::min<size_t>(65535, raw.size() - off);
			bool last = off + block >= raw.size();
			zlib.push_back(last ? 1 : 0);
			zlib.push_back((uint8_t)(block & 0xff));
			zlib.push_back((uint8_t)((block >> 8) & 0xff));
			zlib.push_back((uint8_t)(~block & 0xff));
			zlib.push_back((uint8_t)((~block >> 8) & 0xff));
			zlib.insert(zlib.end(), raw.begin() + off, raw.begin() + off + block);
			off += block;
		}
		uint32_t a = 1, b = 0;
		for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
		uint32_t adler = (b << 16) | a;
		zlib.push_back((uint8_t)(adler >> 24)); zlib.push_back((uint8_t)(adler >> 16));
		zlib.push_back((uint8_t)(adler >> 8)); zlib.push_back((uint8_t)adler);
	}
	FILE* out = fopen(outpng, "wb");
	if (!out) { err = "cannot write output"; return false; }
	auto put_chunk = [&](const char* type, const std::vector<uint8_t>& body) {
		uint8_t len[4] = {(uint8_t)(body.size() >> 24), (uint8_t)(body.size() >> 16), (uint8_t)(body.size() >> 8), (uint8_t)body.size()};
		fwrite(len, 1, 4, out);
		fwrite(type, 1, 4, out);
		if (!body.empty()) fwrite(body.data(), 1, body.size(), out);
		std::vector<uint8_t> crcbuf(type, type + 4);
		crcbuf.insert(crcbuf.end(), body.begin(), body.end());
		uint32_t crc = crc32_of(crcbuf.data(), crcbuf.size(), 0);
		uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
		fwrite(cb, 1, 4, out);
	};
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	fwrite(sig, 1, 8, out);
	std::vector<uint8_t> ihdr = {
		(uint8_t)(mw >> 24), (uint8_t)(mw >> 16), (uint8_t)(mw >> 8), (uint8_t)mw,
		(uint8_t)(mh >> 24), (uint8_t)(mh >> 16), (uint8_t)(mh >> 8), (uint8_t)mh,
		8, 2, 0, 0, 0};
	put_chunk("IHDR", ihdr);
	put_chunk("IDAT", zlib);
	put_chunk("IEND", {});
	fclose(out);
	return true;
}

// Attach the optional CD archives (campaign / bundled maps live here) to a
// loader if present in gamedir. Harmless for tileset lookups.
inline void attach_cd_archives(data_loading::data_files_loader<>& loader, const a_string& gamedir) {
	a_string dir = gamedir;
	if (!dir.empty() && dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\') dir += '/';
	for (const char* fn : {"StarCraft.mpq", "BroodWar.mpq"}) {
		a_string p = dir + fn;
		FILE* f = fopen(p.c_str(), "rb");
		if (!f) continue;
		fclose(f);
		try { loader.add_mpq_file(p); } catch (...) {}
	}
}

// Front-end A: minimap of a map FILE on disk (.scm/.scx MPQ or raw scenario.chk).
inline bool generate_minimap_png(const char* gamedir, const char* mapfile,
                                 const char* outpng, a_string& err) {
	a_vector<uint8_t> chk;
	FILE* mf = fopen(mapfile, "rb");
	if (!mf) { err = "cannot open map file"; return false; }
	uint8_t magic[4] = {0, 0, 0, 0};
	fread(magic, 1, 4, mf);
	bool is_mpq = magic[0] == 'M' && magic[1] == 'P' && magic[2] == 'Q' && magic[3] == 0x1a;
	if (is_mpq) {
		fclose(mf);
		try {
			data_loading::mpq_file<> mm(mapfile);
			mm(chk, "staredit\\scenario.chk");
		} catch (std::exception& e) {
			err = a_string("scenario.chk extract failed: ") + e.what();
			return false;
		}
	} else {
		fseek(mf, 0, SEEK_END);
		long sz = ftell(mf);
		fseek(mf, 0, SEEK_SET);
		chk.resize(sz > 0 ? (size_t)sz : 0);
		if (!chk.empty()) fread(chk.data(), 1, chk.size(), mf);
		fclose(mf);
	}
	auto loader = data_loading::data_files_directory(a_string(gamedir) + "/");
	return render_chk_to_png(loader, chk, outpng, err);
}

// Front-end B: minimap of a map stored INSIDE the game MPQs (bundled / campaign),
// addressed by its archive-internal path. Two forms are supported: a direct CHK
// (campaign/.../staredit/scenario.chk) and a nested .scm/.scx map archive
// (multimaps/(2)Foo.scm) - the latter is itself an MPQ, extracted via a temp
// file next to outpng. scratchdir may be null to reuse the outpng directory.
inline bool generate_minimap_png_internal(const char* gamedir, const char* mappath,
                                          const char* outpng, a_string& err) {
	auto loader = data_loading::data_files_directory(a_string(gamedir) + "/");
	attach_cd_archives(loader, gamedir);
	a_vector<uint8_t> bytes;
	try {
		loader(bytes, mappath);
	} catch (std::exception& e) {
		err = a_string("map load failed: ") + e.what();
		return false;
	}
	a_vector<uint8_t> chk;
	bool nested_mpq = bytes.size() >= 4 && bytes[0] == 'M' && bytes[1] == 'P' && bytes[2] == 'Q' && bytes[3] == 0x1a;
	if (nested_mpq) {
		// Materialise the inner archive and extract its scenario.chk.
		a_string tmp = a_string(outpng) + ".scm.tmp";
		FILE* tf = fopen(tmp.c_str(), "wb");
		if (!tf) { err = "cannot write temp archive"; return false; }
		if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), tf);
		fclose(tf);
		try {
			data_loading::mpq_file<> mm(tmp);
			mm(chk, "staredit\\scenario.chk");
		} catch (std::exception& e) {
			remove(tmp.c_str());
			err = a_string("nested scenario.chk extract failed: ") + e.what();
			return false;
		}
		remove(tmp.c_str());
	} else {
		chk = std::move(bytes);
	}
	return render_chk_to_png(loader, chk, outpng, err);
}

// --- map info (scenario name + description) ------------------------------
// v0.56: extract the CHK's SPRP scenario name + description (via the STR
// string table) for the campaign mission briefing preview. Mirrors bwgame.h
// load_map_data's SPRP/STR handling. Returns "" for missing fields.
inline void parse_map_info(const a_vector<uint8_t>& chk, a_string& name, a_string& desc) {
	name.clear();
	desc.clear();
	auto rd16 = [](const uint8_t* p) -> uint16_t { return (uint16_t)(p[0] | (p[1] << 8)); };
	auto rd32 = [](const uint8_t* p) -> uint32_t { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };
	const uint8_t* str_body = nullptr; size_t str_len = 0;
	int name_idx = 0, desc_idx = 0;
	size_t pos = 0;
	while (pos + 8 <= chk.size()) {
		const uint8_t* tag = chk.data() + pos;
		uint32_t len = rd32(chk.data() + pos + 4);
		pos += 8;
		if (len > chk.size() - pos) len = (uint32_t)(chk.size() - pos);
		const uint8_t* body = chk.data() + pos;
		if (!memcmp(tag, "STR ", 4)) { str_body = body; str_len = len; }        // last STR wins
		else if (!memcmp(tag, "SPRP", 4) && len >= 4) { name_idx = rd16(body); desc_idx = rd16(body + 2); }
		pos += len;
	}
	// 1-based index into the STR table (0 = none).
	auto get_str = [&](int idx) -> a_string {
		if (idx <= 0 || !str_body || str_len < 2) return "";
		size_t num = rd16(str_body);
		if ((size_t)idx > num) return "";
		size_t off_pos = 2 + (size_t)(idx - 1) * 2;
		if (off_pos + 2 > str_len) return "";
		size_t offset = rd16(str_body + off_pos);
		if (offset >= str_len) return "";
		a_string s;
		for (size_t i = offset; i < str_len && str_body[i]; ++i) s += (char)str_body[i];
		return s;
	};
	name = get_str(name_idx);
	desc = get_str(desc_idx);
}

// Front-end: map info from a file on disk (.scm/.scx MPQ or raw .chk).
inline bool get_map_info(const char* mapfile, a_string& name, a_string& desc, a_string& err) {
	a_vector<uint8_t> chk;
	FILE* mf = fopen(mapfile, "rb");
	if (!mf) { err = "cannot open map file"; return false; }
	uint8_t magic[4] = {0, 0, 0, 0};
	fread(magic, 1, 4, mf);
	bool is_mpq = magic[0] == 'M' && magic[1] == 'P' && magic[2] == 'Q' && magic[3] == 0x1a;
	if (is_mpq) {
		fclose(mf);
		try { data_loading::mpq_file<> mm(mapfile); mm(chk, "staredit\\scenario.chk"); }
		catch (std::exception& e) { err = a_string("scenario.chk extract failed: ") + e.what(); return false; }
	} else {
		fseek(mf, 0, SEEK_END); long sz = ftell(mf); fseek(mf, 0, SEEK_SET);
		chk.resize(sz > 0 ? (size_t)sz : 0);
		if (!chk.empty()) fread(chk.data(), 1, chk.size(), mf);
		fclose(mf);
	}
	parse_map_info(chk, name, desc);
	return true;
}

// Front-end: map info from a game-MPQ-internal path (bundled/campaign). Handles
// both a direct scenario.chk and a nested .scm/.scx (extracted via scratchpath).
inline bool get_map_info_internal(const char* gamedir, const char* mappath,
                                  const char* scratchpath, a_string& name, a_string& desc,
                                  a_string& err) {
	auto loader = data_loading::data_files_directory(a_string(gamedir) + "/");
	attach_cd_archives(loader, gamedir);
	a_vector<uint8_t> bytes;
	try { loader(bytes, mappath); }
	catch (std::exception& e) { err = a_string("map load failed: ") + e.what(); return false; }
	a_vector<uint8_t> chk;
	bool nested = bytes.size() >= 4 && bytes[0] == 'M' && bytes[1] == 'P' && bytes[2] == 'Q' && bytes[3] == 0x1a;
	if (nested) {
		a_string tmp = scratchpath && scratchpath[0] ? a_string(scratchpath) : (a_string(mappath) + ".tmp");
		FILE* tf = fopen(tmp.c_str(), "wb");
		if (!tf) { err = "cannot write temp archive"; return false; }
		if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), tf);
		fclose(tf);
		try { data_loading::mpq_file<> mm(tmp); mm(chk, "staredit\\scenario.chk"); }
		catch (std::exception& e) { remove(tmp.c_str()); err = a_string("nested extract failed: ") + e.what(); return false; }
		remove(tmp.c_str());
	} else {
		chk = std::move(bytes);
	}
	parse_map_info(chk, name, desc);
	return true;
}

} // namespace minimap_gen
} // namespace bwgame

#endif
