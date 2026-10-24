// casc.h -- read-only StarCraft: Remastered (NGDP local CASC) reader.
//
// Faze 2 of open1161HD: lets the engine read SC:R native storage directly
// instead of only MPQ. Ported layer-for-layer from the grp-toolkit oracle
// (tools/casc.py), which is validated byte-exact against retail SC:R data.
//
// Resolution chain (ASSET-GUIDE §1.3):
//   name -> root -> CKey -> encoding -> EKey -> .idx -> data.### -> BLTE
//
// Scope / limits (by design, guardrail #3):
//   * Read-only. No writing, no CDN fetch -- a fully installed local storage.
//   * BLTE modes N (raw), Z (zlib), F (recursive frame). Mode E (encrypted,
//     TACT key) is NOT handled -- SC:R graphics/data never need it (only some
//     non-enus locales), so read_ekey() reports failure for an E chunk.
//   * zlib inflate is INJECTED (inflate_fn) -- this header stays free of any
//     compression dependency. The engine's MPQ path uses PKWARE, not zlib, so
//     a zlib provider is supplied by the caller (host harness: libz; engine
//     integration sub-step: bundled inflate).
//
// Endianness: the .idx packs file/offset big-endian and size little-endian;
// BLTE is big-endian; encoding pages are big-endian. All reads are explicit
// byte math, so the code is host-endianness-independent.
#ifndef BWGAME_UI_CASC_H
#define BWGAME_UI_CASC_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <dirent.h>   // POSIX dir scan -- present on MSYS2, Linux and Android NDK

namespace bwgame {
namespace casc {

// Decompress a zlib stream [src, src+srclen) into `out`. `hint` is the exact
// decompressed size when known (BLTE chunk header), else 0. Return false on
// error. Supplied by the caller so this header needs no zlib link.
using inflate_fn = std::function<bool(const uint8_t* src, size_t srclen,
                                      size_t hint, std::vector<uint8_t>& out)>;

namespace detail {

static inline uint32_t be32(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t be_n(const uint8_t* p, int n) { // 1..8 bytes big-endian
	uint64_t v = 0;
	for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
	return v;
}
static inline uint64_t le_n(const uint8_t* p, int n) { // 1..8 bytes little-endian
	uint64_t v = 0;
	for (int i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline std::string hex(const uint8_t* p, size_t n) {
	static const char* d = "0123456789abcdef";
	std::string s;
	s.resize(n * 2);
	for (size_t i = 0; i < n; ++i) { s[2*i] = d[p[i] >> 4]; s[2*i+1] = d[p[i] & 15]; }
	return s;
}

// Parse the first 9 bytes of a hex string (>=18 chars) into a raw 9-byte key
// (tolerant of upper/lowercase). Empty on malformed input. Used to key the
// CASC index by raw bytes (SSO, no heap alloc) instead of 18-char hex.
static inline std::string hex9_raw(const std::string& h) {
	if (h.size() < 18) return std::string();
	auto nib = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	std::string r(9, '\0');
	for (int i = 0; i < 9; ++i) {
		int hi = nib(h[2*i]), lo = nib(h[2*i+1]);
		if (hi < 0 || lo < 0) return std::string();
		r[i] = (char)((hi << 4) | lo);
	}
	return r;
}

static inline bool read_whole(const std::string& path, std::vector<uint8_t>& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	bool ok = false;
	if (n >= 0) {
		out.resize((size_t)n);
		ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	}
	fclose(f);
	return ok;
}

} // namespace detail

struct index_entry {
	int file_index = 0;
	uint64_t offset = 0;
	uint32_t size = 0;
};

class storage {
public:
	// data_dir = ".../Data/data" (holds *.idx and data.###).
	// install_root = the install folder that also contains data/config/**
	// (build config); if empty, taken as two levels above data_dir.
	// Only the low-level index is needed for read_ekey(); read_name() also
	// needs encoding+root, loaded here when the build config is found.
	bool open(const std::string& data_dir, std::string install_root, inflate_fn inflate) {
		dir_ = data_dir;
		inflate_ = std::move(inflate);
		if (!build_index()) return false;
		indexed_ = true;
		if (install_root.empty())
			install_root = parent(parent(abspath(data_dir)));
		load_names(install_root); // best-effort; read_ekey works without it
		return true;
	}

	bool indexed() const { return indexed_; }
	size_t ekey_count() const { return index_.size(); }
	size_t name_count() const { return root_.size(); }
	const std::unordered_map<std::string, std::string>& names() const { return root_; }

	// Read + BLTE-decode the blob for a (full or 9-byte) EKey hex string.
	bool read_ekey(const std::string& ekey_hex, std::vector<uint8_t>& out) {
		if (ekey_hex.size() < 18) return false;
		std::string key = detail::hex9_raw(ekey_hex); // 18 hex chars -> raw 9-byte key
		if (key.empty()) return false;
		auto it = index_.find(key);
		if (it == index_.end()) return false;
		const index_entry& e = it->second;
		std::vector<uint8_t> raw;
		if (!read_blob(e, raw)) return false;
		if (raw.size() < 30) return false;
		return blte_decode(raw.data() + 30, raw.size() - 30, out); // skip blob header
	}

	// Resolve a logical name (case-insensitive, / or \\) to its bytes.
	bool read_name(const std::string& name, std::vector<uint8_t>& out) {
		auto rit = root_.find(norm(name));
		if (rit == root_.end()) return false;
		auto eit = enc_.find(rit->second);
		if (eit == enc_.end()) return false;
		return read_ekey(eit->second, out);
	}

	bool has_name(const std::string& name) const { return root_.count(norm(name)) != 0; }

private:
	std::string dir_;
	inflate_fn inflate_;
	bool indexed_ = false;
	std::unordered_map<std::string, index_entry> index_; // raw 9-byte ekey -> entry (B10: SSO key, no hex)
	std::unordered_map<std::string, std::string> enc_;   // CKey hex -> EKey hex
	std::unordered_map<std::string, std::string> root_;  // norm(name) -> CKey hex

	// ---- path helpers (portable, no <filesystem>) --------------------------
	static std::string norm(const std::string& s) {
		std::string o = s;
		for (char& c : o) { if (c == '\\') c = '/'; else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
		return o;
	}
	static std::string abspath(const std::string& p) {
		// Good enough for our use: callers pass absolute paths already.
		return p;
	}
	static std::string parent(const std::string& p) {
		size_t i = p.find_last_of("/\\");
		return i == std::string::npos ? p : p.substr(0, i);
	}
	std::string join(const std::string& a, const std::string& b) const { return a + "/" + b; }

	// ---- local .idx (v7) ---------------------------------------------------
	bool parse_idx(const std::string& path) {
		std::vector<uint8_t> d;
		if (!detail::read_whole(path, d) || d.size() < 0x28) return false;
		int span_size_b = d[12], span_off_b = d[13], key_b = d[14], seg_bits = d[15];
		// SC:R v7: span_size_b=4, span_off_b=5 (40-bit file<<seg_bits | offset,
		// seg_bits=30), key_b=9. Allow the plausible ranges these can take.
		if (key_b < 9 || span_off_b < 1 || span_off_b > 6 || span_size_b < 1 ||
		    span_size_b > 4 || seg_bits < 1 || seg_bits > 40)
			return false;
		uint32_t entries_size = (uint32_t)detail::le_n(&d[0x20], 4);
		size_t off = 0x28;
		int elen = key_b + span_off_b + span_size_b;
		uint64_t seg_mask = (seg_bits >= 64) ? ~0ull : (((uint64_t)1 << seg_bits) - 1);
		for (uint32_t i = 0; i + elen <= entries_size && off + elen <= d.size(); ++i, off += elen) {
			const uint8_t* p = &d[off];
			std::string ekey(reinterpret_cast<const char*>(p), (size_t)9); // raw 9-byte EKey (SSO: no alloc, no hex)
			uint64_t packed = detail::be_n(p + key_b, span_off_b);
			index_entry e;
			e.file_index = (int)(packed >> seg_bits);
			e.offset = packed & seg_mask;
			e.size = (uint32_t)detail::le_n(p + key_b + span_off_b, span_size_b);
			index_[ekey] = e; // last .idx wins (newest)
		}
		return true;
	}

	bool build_index() {
		// Enumerate *.idx via a portable dir scan.
		std::vector<std::string> idx = list_idx(dir_);
		if (idx.empty()) return false;
		for (const std::string& p : idx) parse_idx(p);
		return !index_.empty();
	}

	// ---- data.### blob -----------------------------------------------------
	bool read_blob(const index_entry& e, std::vector<uint8_t>& out) {
		char name[32];
		snprintf(name, sizeof name, "data.%03d", e.file_index);
		FILE* f = fopen(join(dir_, name).c_str(), "rb");
		if (!f) return false;
		bool ok = false;
		if (fseek(f, (long)e.offset, SEEK_SET) == 0) {
			out.resize(e.size);
			ok = e.size == 0 || fread(out.data(), 1, e.size, f) == e.size;
		}
		fclose(f);
		return ok;
	}

	// ---- BLTE --------------------------------------------------------------
	bool blte_decode(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
		out.clear();
		if (len < 8 || memcmp(data, "BLTE", 4) != 0) return false;
		uint32_t hsize = detail::be32(data + 4);
		struct chunk { uint32_t cs, ds; };
		std::vector<chunk> chunks;
		size_t start;
		if (hsize > 0) {
			if (len < 12) return false;
			uint32_t cnt = detail::be_n(data + 9, 3); // 3-byte count at [9:12]
			size_t p = 12;
			for (uint32_t i = 0; i < cnt; ++i) {
				if (p + 8 + 16 > len) return false;
				chunk c;
				c.cs = detail::be32(data + p);
				c.ds = detail::be32(data + p + 4);
				p += 8 + 16; // skip 16-byte checksum
				chunks.push_back(c);
			}
			start = hsize;
		} else {
			chunk c{ (uint32_t)(len - 8), 0 };
			chunks.push_back(c);
			start = 8;
		}
		size_t p = start;
		for (const chunk& c : chunks) {
			if (p + c.cs > len || c.cs < 1) return false;
			uint8_t mode = data[p];
			const uint8_t* body = data + p + 1;
			size_t bodylen = c.cs - 1;
			p += c.cs;
			switch (mode) {
			case 0x4E: // 'N' raw
				out.insert(out.end(), body, body + bodylen);
				break;
			case 0x5A: { // 'Z' zlib
				if (!inflate_) return false;
				std::vector<uint8_t> dec;
				if (!inflate_(body, bodylen, c.ds, dec)) return false;
				out.insert(out.end(), dec.begin(), dec.end());
				break;
			}
			case 0x46: { // 'F' recursive BLTE frame
				std::vector<uint8_t> dec;
				if (!blte_decode(body, bodylen, dec)) return false;
				out.insert(out.end(), dec.begin(), dec.end());
				break;
			}
			case 0x45: // 'E' encrypted -- out of scope
			default:
				return false;
			}
		}
		return true;
	}

	// ---- encoding + root (name resolution) ---------------------------------
	bool parse_encoding(const std::vector<uint8_t>& buf) {
		if (buf.size() < 22) return false;
		uint32_t ckey_page_kb = detail::be16(&buf[5]);
		uint32_t ckey_page_count = detail::be32(&buf[9]);
		uint32_t espec = detail::be32(&buf[18]);
		size_t page = (size_t)ckey_page_kb * 1024;
		size_t pages_off = 22 + (size_t)espec + (size_t)ckey_page_count * 32;
		for (uint32_t pg = 0; pg < ckey_page_count; ++pg) {
			size_t p = pages_off + (size_t)pg * page, end = p + page;
			if (end > buf.size()) break;
			while (p + 38 <= end) {
				uint8_t kc = buf[p];
				if (kc == 0) break;
				std::string ck = detail::hex(&buf[p + 6], 16);
				std::string ek = detail::hex(&buf[p + 22], 16);
				enc_[ck] = ek;
				p += 22 + (size_t)kc * 16;
			}
		}
		return !enc_.empty();
	}

	// SC:R root is a text table: "name|ckeyhex" per line.
	bool parse_root(const std::vector<uint8_t>& buf) {
		size_t i = 0, n = buf.size();
		while (i < n) {
			size_t j = i;
			while (j < n && buf[j] != '\n') ++j;
			size_t e = j;
			if (e > i && buf[e - 1] == '\r') --e;
			// find last '|'
			size_t bar = std::string::npos;
			for (size_t k = e; k > i; --k) if (buf[k - 1] == '|') { bar = k - 1; break; }
			if (bar != std::string::npos) {
				std::string name((const char*)&buf[i], bar - i);
				std::string ck((const char*)&buf[bar + 1], e - (bar + 1));
				// trim ck whitespace
				while (!ck.empty() && (ck.back() == ' ' || ck.back() == '\t')) ck.pop_back();
				root_[norm(name)] = ck;
			}
			i = j + 1;
		}
		return !root_.empty();
	}

	void load_names(const std::string& install_root) {
		std::string enc_ek, root_ck;
		if (!find_build_config(install_root, enc_ek, root_ck)) return;
		std::vector<uint8_t> encbuf;
		if (!read_ekey(enc_ek, encbuf)) return;
		if (!parse_encoding(encbuf)) return;
		auto it = enc_.find(root_ck);
		if (it == enc_.end()) return;
		std::vector<uint8_t> rootbuf;
		if (!read_ekey(it->second, rootbuf)) return;
		parse_root(rootbuf);
	}

	// ---- platform bits (POSIX dirent; works on MSYS2 host and Android) ------
	// Absolute paths of *.idx in `dir`, sorted ascending (newest .idx last so
	// last-write-wins in parse_idx picks the freshest entry).
	std::vector<std::string> list_idx(const std::string& dir) {
		std::vector<std::string> out;
		DIR* dp = opendir(dir.c_str());
		if (!dp) return out;
		while (struct dirent* de = readdir(dp)) {
			const char* nm = de->d_name;
			size_t l = strlen(nm);
			if (l > 4 && strcmp(nm + l - 4, ".idx") == 0)
				out.push_back(join(dir, nm));
		}
		closedir(dp);
		std::sort(out.begin(), out.end());
		return out;
	}

	// Walk install_root for a 32-hex-named file < 8 KB that is a build config
	// (contains "root =" and "encoding ="); pull the encoding EKey (2nd hash
	// on the encoding line) and the root CKey. Bounded recursion.
	bool find_build_config(const std::string& root, std::string& enc_ek,
	                       std::string& root_ck, int depth = 0) {
		if (depth > 8) return false;
		DIR* dp = opendir(root.c_str());
		if (!dp) return false;
		std::vector<std::string> subdirs;
		bool found = false;
		while (struct dirent* de = readdir(dp)) {
			const char* nm = de->d_name;
			if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
			std::string full = join(root, nm);
			if (is_dir(full)) { subdirs.push_back(full); continue; }
			if (is_hex32(nm) && try_build_config(full, enc_ek, root_ck)) { found = true; break; }
		}
		closedir(dp);
		if (found) return true;
		for (const std::string& s : subdirs)
			if (find_build_config(s, enc_ek, root_ck, depth + 1)) return true;
		return false;
	}

	static bool is_dir(const std::string& p) {
		DIR* d = opendir(p.c_str());
		if (d) { closedir(d); return true; }
		return false;
	}
	static bool is_hex32(const char* s) {
		int n = 0;
		for (; s[n]; ++n) {
			char c = s[n];
			bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
			if (!hex) return false;
		}
		return n == 32;
	}
	static bool try_build_config(const std::string& path, std::string& enc_ek,
	                             std::string& root_ck) {
		std::vector<uint8_t> b;
		if (!detail::read_whole(path, b) || b.empty() || b.size() >= 8192) return false;
		std::string t((const char*)b.data(), b.size());
		if (t.find("root =") == std::string::npos ||
		    t.find("encoding =") == std::string::npos)
			return false;
		// encoding = <ckey32> <ekey32>   -> take the 2nd 32-hex token
		if (!field_hash(t, "encoding =", 1, enc_ek)) return false;
		if (!field_hash(t, "root =", 0, root_ck)) return false;
		return true;
	}
	// Return the `which`-th 32-hex token that follows `key` on its line.
	static bool field_hash(const std::string& t, const std::string& key,
	                       int which, std::string& out) {
		size_t k = t.find(key);
		if (k == std::string::npos) return false;
		size_t p = k + key.size();
		size_t eol = t.find('\n', p);
		if (eol == std::string::npos) eol = t.size();
		int seen = 0;
		while (p < eol) {
			// scan a run of hex
			size_t s = p;
			while (s < eol) {
				char c = t[s];
				if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) ++s; else break;
			}
			if (s - p == 32) {
				if (seen == which) { out = t.substr(p, 32); return true; }
				++seen;
				p = s;
			} else {
				p = (s > p) ? s : p + 1;
			}
		}
		return false;
	}
};

} // namespace casc
} // namespace bwgame

#endif
