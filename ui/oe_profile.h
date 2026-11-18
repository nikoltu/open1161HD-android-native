#pragma once
// Ganymede "Extended" map profile guard (G13 step 1). A map MPQ may carry
// oe\manifest.json; this engine plays it only when it understands every
// declared runtime feature, otherwise the map is refused (never a silent
// classic load of a map that expects more). Pure file inspection, no sim.
#include "data_loading.h"
#include "mini_json.h"

#include <string>
#include <vector>

namespace oeprofile {

// Highest oe/manifest.json "runtimeVersion" this engine plays.
constexpr int runtime_version = 0;
inline bool known_feature(const std::string&) { return false; }

struct manifest {
	bool present = false; // the map has oe\manifest.json
	bool playable = true;
	int runtime = 0;
	std::vector<std::string> features;
	std::string reason;   // why it is refused (English, for logs and the test trace)
	int needs_runtime = 0;
	// for the launcher's own wording: "damaged" (unreadable / malformed manifest), "newer"
	// (a later manifest format or another profile), "runtime" (arg = the runtime it needs)
	// or "feature" (arg = the feature name)
	std::string kind, arg;
};

inline manifest check_json(const char* data, size_t size) {
	manifest m;
	m.present = true;
	mini_json::value v;
	std::string err;
	auto refuse = [&](const std::string& r) { m.playable = false; m.reason = r; if (m.kind.empty()) m.kind = "damaged"; return m; };
	if (!mini_json::parse(data, size, v, &err)) return refuse("oe/manifest.json is not valid JSON (" + err + ")");
	if (v.kind != mini_json::value::object_v) return refuse("oe/manifest.json is not a JSON object");
	const mini_json::value* fmt = v.get("format");
	if (fmt && fmt->is_int() && fmt->as_int() > 1) m.kind = "newer";
	if (!fmt || !fmt->is_int() || fmt->as_int() != 1) return refuse("oe/manifest.json format is not 1");
	if (v.str_at("profile") != "open1161hd-extended") {
		m.kind = "newer";
		return refuse("unknown map profile \"" + v.str_at("profile") + "\"");
	}
	const mini_json::value* rv = v.get("runtimeVersion");
	if (rv && !rv->is_int()) return refuse("runtimeVersion is not an integer");
	m.runtime = rv ? (int)rv->as_int() : 0;
	if (m.runtime < 0) return refuse("runtimeVersion is negative");
	if (const mini_json::value* ff = v.get("featureFlags")) {
		if (ff->kind != mini_json::value::array_v) return refuse("featureFlags is not an array");
		for (auto& f : ff->arr) {
			if (f.kind != mini_json::value::string_v) return refuse("featureFlags holds a non-string");
			m.features.push_back(f.str);
		}
	}
	if (m.runtime > runtime_version) {
		m.needs_runtime = m.runtime;
		m.kind = "runtime";
		m.arg = std::to_string(m.runtime);
		return refuse("the map needs Extended runtime " + std::to_string(m.runtime) + ", this game plays up to " + std::to_string(runtime_version));
	}
	for (auto& f : m.features) {
		if (known_feature(f)) continue;
		m.kind = "feature";
		m.arg = f;
		return refuse("the map needs Extended feature \"" + f + "\"");
	}
	return m;
}

// A plain .chk, an unreadable file or an MPQ without oe\manifest.json: classic (present = false).
inline manifest check_map_file(const std::string& path) {
	manifest none;
	const bwgame::a_string name = "oe\\manifest.json";
	bool present = false;
	try {
		bwgame::data_loading::mpq_file<> f(bwgame::a_string(path.c_str()));
		if (!f.mpq.file_exists(name)) return none;
		present = true;
		bwgame::a_vector<uint8_t> buf;
		f(buf, name);
		return check_json((const char*)buf.data(), buf.size());
	} catch (const std::exception&) {
		if (!present) return none;
		manifest m;
		m.present = true;
		m.playable = false;
		m.reason = "oe/manifest.json cannot be read";
		m.kind = "damaged";
		return m;
	}
}

} // namespace oeprofile
