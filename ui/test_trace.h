#pragma once
// World Editor "Test in engine" session trace: oetrace/1, one JSON object per line.
// Shared by the Android engine (android_main.cpp) and the desktop host (tools/oetest.cpp).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oetest {

struct sha256_ctx {
	uint32_t h[8];
	uint8_t buf[64];
	uint64_t len = 0;
	size_t fill = 0;
	sha256_ctx() {
		static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		memcpy(h, init, sizeof(h));
	}
	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
	void block(const uint8_t* p) {
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i != 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
		for (int i = 16; i != 64; ++i) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i != 64; ++i) {
			uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	void update(const uint8_t* p, size_t n) {
		len += n;
		while (n) {
			size_t take = 64 - fill < n ? 64 - fill : n;
			memcpy(buf + fill, p, take);
			fill += take; p += take; n -= take;
			if (fill == 64) { block(buf); fill = 0; }
		}
	}
	std::string hex() {
		uint64_t bits = len * 8;
		uint8_t pad = 0x80;
		update(&pad, 1);
		uint8_t zero = 0;
		while (fill != 56) update(&zero, 1);
		uint8_t lb[8];
		for (int i = 0; i != 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
		update(lb, 8);
		static const char* d = "0123456789abcdef";
		std::string r;
		for (uint32_t v : h) for (int s = 28; s >= 0; s -= 4) r += d[(v >> s) & 15];
		return r;
	}
};

// Empty string when the file cannot be read.
inline std::string sha256_file(const std::string& path, uint64_t* size = nullptr) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return std::string();
	sha256_ctx c;
	uint8_t b[16384];
	size_t n;
	uint64_t total = 0;
	while ((n = fread(b, 1, sizeof(b), f)) != 0) { c.update(b, n); total += n; }
	fclose(f);
	if (size) *size = total;
	return c.hex();
}

// Hex digests compared without regard to letter case; an empty side never matches.
inline bool sha256_equal(const std::string& a, const std::string& b) {
	if (a.empty() || a.size() != b.size()) return false;
	for (size_t i = 0; i != a.size(); ++i) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return false;
	}
	return true;
}

inline std::string jstr(const std::string& s) {
	std::string r = "\"";
	for (unsigned char ch : s) {
		if (ch == '"' || ch == '\\') { r += '\\'; r += (char)ch; }
		else if (ch == '\n') r += "\\n";
		else if (ch == '\r') r += "\\r";
		else if (ch == '\t') r += "\\t";
		else if (ch < 0x20 || ch >= 0x80) { char e[8]; snprintf(e, sizeof(e), "\\u%04x", ch); r += e; }
		else r += (char)ch;
	}
	return r + "\"";
}

// Order-sensitive FNV-1a over every live unit (owner, type, position, hp, shields)
// plus the RNG and resources: a cheap fingerprint to compare two runs of the same seed.
// extra: the script runtime hash when the map has a script (0 = none, not mixed).
template<typename state_t, typename ptr_f>
uint64_t state_hash(const state_t& st, ptr_f&& units_of, uint64_t extra = 0) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t v) { for (int i = 0; i != 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; } };
	mix(st.lcg_rand_state);
	mix(st.current_frame);
	for (int p = 0; p != 12; ++p) {
		mix((uint64_t)(uint32_t)st.current_minerals[p] << 32 | (uint32_t)st.current_gas[p]);
		units_of(p, [&](int type, int x, int y, int hp, int sh) {
			mix((uint64_t)p << 48 | (uint64_t)(uint32_t)type);
			mix((uint64_t)(uint32_t)x << 32 | (uint32_t)y);
			mix((uint64_t)(uint32_t)hp << 32 | (uint32_t)sh);
		});
	}
	if (extra) mix(extra);
	return h;
}

struct trace {
	FILE* f = nullptr;
	std::string path;
	bool ended = false;
	bool started = false;
	bool truncated = false;
	bool keep_ai_lines = false;
	size_t bytes = 0;
	size_t max_bytes = 64u << 20;
	int last_fire_frame = -1;

	bool active() const { return f != nullptr; }

	bool open(const std::string& p) {
		if (f) fclose(f);
		*this = trace();
		path = p;
		f = fopen(p.c_str(), "wb");
		if (f) setvbuf(f, nullptr, _IOFBF, 1 << 16);
		return f != nullptr;
	}
	void write(const std::string& obj, bool flush = false) {
		if (!f || ended) return;
		fputs(obj.c_str(), f);
		fputc('\n', f);
		bytes += obj.size() + 1;
		if (flush) fflush(f);
	}
	// Trigger events stop at max_bytes; the session/end records are always written.
	bool room() {
		if (truncated) return false;
		if (bytes < max_bytes) return true;
		truncated = true;
		write("{\"ev\":\"truncated\",\"bytes\":" + std::to_string(bytes) + "}", true);
		return false;
	}

	void session(const std::string& id, const std::string& platform, const std::string& engine, int version_code,
			const std::string& map, const std::string& map_sha, uint64_t map_bytes, int oescript_api = 0) {
		write("{\"ev\":\"session\",\"format\":\"oetrace/1\",\"session\":" + jstr(id) + ",\"platform\":" + jstr(platform)
			+ ",\"engine\":" + jstr(engine) + ",\"versionCode\":" + std::to_string(version_code)
			+ ",\"map\":" + jstr(map) + ",\"mapSha256\":" + jstr(map_sha) + ",\"mapBytes\":" + std::to_string(map_bytes)
			+ ",\"oescriptApi\":" + std::to_string(oescript_api) + ",\"features\":[\"oesc\",\"mbrf-file\"]}", true);
	}
	void loaded(const std::string& mode, uint32_t seed, int local_player, int map_w, int map_h, int triggers) {
		write("{\"ev\":\"loaded\",\"mode\":" + jstr(mode) + ",\"seed\":" + std::to_string(seed)
			+ ",\"localPlayer\":" + std::to_string(local_player) + ",\"mapWidth\":" + std::to_string(map_w)
			+ ",\"mapHeight\":" + std::to_string(map_h) + ",\"triggers\":" + std::to_string(triggers) + "}", true);
	}
	void game_start(int frame) {
		if (started) return;
		started = true;
		write("{\"ev\":\"gameStart\",\"f\":" + std::to_string(frame) + "}", true);
	}
	void script_state(int frame, uint64_t hash, const std::vector<std::pair<std::string, int>>& objectives,
			const std::vector<std::pair<std::string, long long>>& counters, int faults, const std::string& last_fault) {
		static const char* const names[3] = {"active", "done", "failed"};
		char hx[24];
		snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)hash);
		std::string s = "{\"ev\":\"scriptState\",\"sf\":" + std::to_string(frame) + ",\"hash\":\"" + hx + "\",\"objectives\":[";
		for (size_t i = 0; i != objectives.size(); ++i) {
			int k = objectives[i].second;
			s += (i ? ",{\"id\":" : "{\"id\":") + jstr(objectives[i].first) + ",\"state\":\"" + (k >= 0 && k < 3 ? names[k] : "?") + "\"}";
		}
		s += "],\"counters\":{";
		for (size_t i = 0; i != counters.size(); ++i) s += (i ? "," : "") + jstr(counters[i].first) + ":" + std::to_string(counters[i].second);
		s += "},\"faults\":" + std::to_string(faults);
		if (!last_fault.empty()) s += ",\"lastFault\":" + jstr(last_fault);
		write(s + "}", true);
	}
	// G12: Campaign Studio objectives of a classic mission (state 0 active, 1 done, 2 failed).
	void classic_objectives(const std::vector<std::tuple<std::string, std::string, int, bool>>& list) {
		static const char* const names[3] = {"active", "done", "failed"};
		std::string s = "{\"ev\":\"objectives\",\"source\":\"classic\",\"objectives\":[";
		for (size_t i = 0; i != list.size(); ++i) {
			int k = std::get<2>(list[i]);
			s += (i ? ",{\"id\":" : "{\"id\":") + jstr(std::get<0>(list[i])) + ",\"title\":" + jstr(std::get<1>(list[i]))
				+ ",\"state\":\"" + (k >= 0 && k < 3 ? names[k] : "?") + "\",\"optional\":" + (std::get<3>(list[i]) ? "true" : "false") + "}";
		}
		write(s + "]}", true);
	}
	void end(const std::string& result, int frame, uint32_t rand, uint64_t hash, int units,
			const std::string& message = std::string(), const std::string& replay = std::string()) {
		if (!f || ended) return;
		char hx[24];
		snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)hash);
		std::string s = "{\"ev\":\"end\",\"result\":" + jstr(result) + ",\"f\":" + std::to_string(frame)
			+ ",\"rand\":" + std::to_string(rand) + ",\"stateHash\":\"" + hx + "\",\"units\":" + std::to_string(units);
		if (!replay.empty()) s += ",\"replay\":" + jstr(replay);
		if (!message.empty()) s += ",\"message\":" + jstr(message);
		write(s + "}", true);
		ended = true;
		fclose(f);
		f = nullptr;
	}

	// Turns one engine trigger-trace line (bwgame.h trigger_trace_sink) into an event.
	void engine_line(const char* s) {
		if (!f || ended) return;
		int fr, p, a, b, c, d, e, g, h, i, j, k;
		char tok[16];
		if (sscanf(s, "trace f%d p%d TRIGGER FIRES trig#%d", &fr, &p, &a) == 3) {
			if (!room()) return;
			std::string acts = "[";
			const char* l = strstr(s, "actions:");
			if (l) {
				l += 8;
				int t, n;
				bool first = true;
				while (sscanf(l, "%d%n", &t, &n) == 1) { if (!first) acts += ','; acts += std::to_string(t); first = false; l += n; }
			}
			acts += "]";
			write("{\"ev\":\"fire\",\"f\":" + std::to_string(fr) + ",\"p\":" + std::to_string(p) + ",\"trig\":" + std::to_string(a)
				+ ",\"actions\":" + acts + "}", fr != last_fire_frame);
			last_fire_frame = fr;
		} else if (sscanf(s, "trace f%d p%d action type=%d loc=%d unit=%d grp=%d/%d trig#%d act%d", &fr, &p, &a, &b, &c, &d, &e, &g, &h) == 9) {
			if (!room()) return;
			write("{\"ev\":\"action\",\"f\":" + std::to_string(fr) + ",\"p\":" + std::to_string(p) + ",\"trig\":" + std::to_string(g)
				+ ",\"act\":" + std::to_string(h) + ",\"type\":" + std::to_string(a) + ",\"loc\":" + std::to_string(b)
				+ ",\"unit\":" + std::to_string(c) + ",\"group\":" + std::to_string(d) + ",\"group2\":" + std::to_string(e) + "}");
		} else if (sscanf(s, "trace f%d p%d trig#%d cond%d type=%d grp=%d unit=%d cmp=%d count=%d extra=%d loc=%d -> %15s",
				&fr, &p, &a, &b, &c, &d, &e, &g, &h, &i, &j, tok) == 12) {
			if (!room()) return;
			std::string st = !strcmp(tok, "ok") ? "ok" : !strcmp(tok, "disabled") ? "disabled" : "fail";
			std::string have;
			const char* hv = strstr(s, "(have=");
			if (hv && sscanf(hv, "(have=%d", &k) == 1) have = ",\"have\":" + std::to_string(k);
			write("{\"ev\":\"cond\",\"f\":" + std::to_string(fr) + ",\"p\":" + std::to_string(p) + ",\"trig\":" + std::to_string(a)
				+ ",\"cond\":" + std::to_string(b - 1) + ",\"type\":" + std::to_string(c) + ",\"group\":" + std::to_string(d)
				+ ",\"unit\":" + std::to_string(e) + ",\"cmp\":" + std::to_string(g) + ",\"count\":" + std::to_string(h)
				+ ",\"extra\":" + std::to_string(i) + ",\"loc\":" + std::to_string(j) + ",\"state\":\"" + st + "\"" + have + "}");
		} else if (sscanf(s, "trace f%d p%d switch %d = %d", &fr, &p, &a, &b) == 4) {
			if (!room()) return;
			write("{\"ev\":\"switch\",\"f\":" + std::to_string(fr) + ",\"p\":" + std::to_string(p) + ",\"switch\":" + std::to_string(a)
				+ ",\"value\":" + std::to_string(b) + "}");
		} else if (sscanf(s, "trace f%d p%d victory_state %d", &fr, &p, &a) == 3) {
			write("{\"ev\":\"victoryState\",\"f\":" + std::to_string(fr) + ",\"p\":" + std::to_string(p) + ",\"state\":" + std::to_string(a) + "}", true);
		} else if (sscanf(s, "oes f%d s%d @line %d %d", &fr, &a, &b, &c) == 4) {
			if (!room()) return;
			write("{\"ev\":\"scriptLine\",\"f\":" + std::to_string(fr) + ",\"h\":" + std::to_string(b) + ",\"line\":" + std::to_string(c) + "}");
		} else if (sscanf(s, "oes f%d s%d @run %d %n", &fr, &a, &b, &k) == 3) {
			if (!room()) return;
			write("{\"ev\":\"scriptRun\",\"f\":" + std::to_string(fr) + ",\"h\":" + std::to_string(b) + ",\"label\":" + jstr(strip(s + k)) + "}");
		} else if (sscanf(s, "oes f%d s%d %n", &fr, &a, &k) == 2) {
			if (!room()) return;
			write("{\"ev\":\"script\",\"f\":" + std::to_string(fr) + ",\"sf\":" + std::to_string(a) + ",\"text\":" + jstr(strip(s + k)) + "}");
		} else if (sscanf(s, "oes loaded %d handlers %d code", &a, &b) == 2) {
			write("{\"ev\":\"scriptLoaded\",\"handlers\":" + std::to_string(a) + ",\"code\":" + std::to_string(b) + "}", true);
		} else if (!strncmp(s, "oes error ", 10)) {
			write("{\"ev\":\"scriptError\",\"message\":" + jstr(strip(s + 10)) + "}", true);
		} else if (sscanf(s, "trace: TRIG parsed %d", &a) == 1) {
			write("{\"ev\":\"triggers\",\"count\":" + std::to_string(a) + "}");
		} else if (sscanf(s, "trace: player %d controller %d force %d -> %d", &p, &a, &b, &c) == 4) {
			write("{\"ev\":\"owner\",\"p\":" + std::to_string(p) + ",\"controller\":" + std::to_string(a) + ",\"force\":" + std::to_string(b)
				+ ",\"triggers\":" + std::to_string(c) + "}");
		} else if (sscanf(s, "trace: trigger %d enabled for:", &a) == 1) {
			std::string list = "[";
			const char* l = strstr(s, "for:");
			int t, n;
			bool first = true;
			for (l += 4; sscanf(l, "%d%n", &t, &n) == 1; l += n) { if (!first) list += ','; list += std::to_string(t); first = false; }
			write("{\"ev\":\"triggerEnabled\",\"trig\":" + std::to_string(a) + ",\"for\":" + list + "]}");
		} else if (s[0] == 'a' && s[1] == 'i') {
			if (keep_ai_lines && room()) write("{\"ev\":\"ai\",\"text\":" + jstr(strip(s)) + "}");
		} else {
			if (room()) write("{\"ev\":\"log\",\"text\":" + jstr(strip(s)) + "}");
		}
	}
	static std::string strip(const char* s) {
		std::string r = s;
		while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
		return r;
	}
};

inline trace& active_trace() {
	static trace t;
	return t;
}

// Session ids name files and come from another app: keep them to [A-Za-z0-9._-], max 64.
inline bool valid_session_id(const std::string& s) {
	if (s.empty() || s.size() > 64) return false;
	for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
	return true;
}

// A trace path is only ever a *.oetrace.jsonl file, so a caller cannot make the engine
// truncate an arbitrary file it can reach.
inline bool valid_trace_path(const std::string& p) {
	static const char* ext = ".oetrace.jsonl";
	size_t n = strlen(ext);
	if (p.size() <= n || p.compare(p.size() - n, n, ext) != 0) return false;
	if (p.find("..") != std::string::npos) return false;
	return true;
}

}
