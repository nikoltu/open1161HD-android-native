#pragma once
// G13(2) / GE1: the type registry of Ganymede Extended mods (data/oe/extensions.json,
// written by the editor's ModExtensions.export). Definitions are checked against the
// editor's field table and given stable runtime indices in their own space (custom
// unit / building types from 256 up, sorted by namespaced id: 228-232 are None and the
// trigger unit groups). to_engine() turns them into the engine's ext_unit_def list.
#include "mini_json.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace oeext {

constexpr int format = 1;
constexpr int base_unit_count = 228;
constexpr int first_custom_type = 256; // bwgame::ext_unit_type_first
constexpr int max_custom_types = 128;  // bwgame::ext_unit_type_max
enum kind_t { k_unit, k_building, k_hero, k_ability, k_count };
inline const char* kind_name(int k) {
	static const char* const n[k_count] = {"unit", "building", "hero", "ability"};
	return k >= 0 && k < k_count ? n[k] : "?";
}
inline int kind_of(const std::string& s) {
	for (int k = 0; k != k_count; ++k) if (s == kind_name(k)) return k;
	return -1;
}

enum ftype { f_int, f_text, f_bool, f_choice, f_base_unit, f_weapon, f_ref, f_refs, f_ints, f_asset };
struct field {
	const char* key;
	ftype type;
	long long min, max;
	unsigned refs; // REF / REFS: bit per kind a value may name
	bool base;     // REF / REFS: "base:N" (a game unit) allowed
};

// ModExtensions.FIELDS (editor), per kind.
inline const std::vector<field>& fields(int kind) {
	static std::vector<field> t[k_count];
	if (t[0].empty()) {
		const unsigned U = 1u << k_unit, B = 1u << k_building, A = 1u << k_ability;
		for (int k : {k_unit, k_building}) {
			bool u = k == k_unit;
			auto& f = t[k];
			f.push_back({"name", f_text, 0, 64, 0, false});
			f.push_back({"parent", f_base_unit, 0, base_unit_count - 1, 0, true});
			f.push_back({"race", f_choice, 0, 3, 0, false});
			f.push_back({"description", f_text, 0, 400, 0, false});
			f.push_back({"hp", f_int, 1, 99999, 0, false});
			f.push_back({"shields", f_int, 0, 99999, 0, false});
			f.push_back({"armor", f_int, 0, 255, 0, false});
			f.push_back({"energy", f_int, 0, 1000, 0, false});
			if (u) f.push_back({"speed", f_int, 0, 100, 0, false});
			f.push_back({"sight", f_int, 1, 11, 0, false});
			f.push_back({"supply", f_int, 0, 200, 0, false});
			f.push_back({"weapon", f_weapon, -1, 129, 0, false});
			f.push_back({"minerals", f_int, 0, 9999, 0, false});
			f.push_back({"gas", f_int, 0, 9999, 0, false});
			f.push_back({"time", f_int, 1, 9999, 0, false});
			if (u) f.push_back({"producer", f_ref, 0, 0, B, true});
			else {
				f.push_back({"builder", f_ref, 0, 0, U, true});
				f.push_back({"produces", f_refs, 0, 0, U, true});
				f.push_back({"width", f_int, 1, 8, 0, false});
				f.push_back({"height", f_int, 1, 8, 0, false});
			}
			f.push_back({"requires", f_refs, 0, 0, U | B, true});
			f.push_back({"abilities", f_refs, 0, 0, A, false});
			f.push_back({"hotkey", f_text, 0, 1, 0, false});
			f.push_back({"icon", f_asset, 0, 0, 0, false});
			f.push_back({"portrait", f_asset, 0, 0, 0, false});
			for (const char* v : {"voice.ready", "voice.what", "voice.yes", "voice.pissed", "voice.attack", "voice.death"})
				f.push_back({v, f_asset, 0, 0, 0, false});
		}
		auto& h = t[k_hero];
		h.push_back({"name", f_text, 0, 64, 0, false});
		h.push_back({"unit", f_ref, 0, 0, U, true});
		h.push_back({"description", f_text, 0, 400, 0, false});
		h.push_back({"maxLevel", f_int, 1, 100, 0, false});
		h.push_back({"xp", f_ints, 1, 10000000, 0, false});
		h.push_back({"skillPoints", f_int, 0, 10, 0, false});
		h.push_back({"xpKill", f_int, 0, 10000, 0, false});
		h.push_back({"xpRadius", f_int, 0, 64, 0, false});
		h.push_back({"xpShared", f_bool, 0, 1, 0, false});
		h.push_back({"strength", f_int, 0, 1000, 0, false});
		h.push_back({"agility", f_int, 0, 1000, 0, false});
		h.push_back({"intelligence", f_int, 0, 1000, 0, false});
		h.push_back({"hpPerLevel", f_int, 0, 10000, 0, false});
		h.push_back({"armorPerLevel", f_int, 0, 50, 0, false});
		h.push_back({"damagePerLevel", f_int, 0, 1000, 0, false});
		h.push_back({"energyPerLevel", f_int, 0, 1000, 0, false});
		h.push_back({"abilities", f_refs, 0, 0, A, false});
		h.push_back({"respawn", f_bool, 0, 1, 0, false});
		h.push_back({"respawnTime", f_int, 0, 3600, 0, false});
		h.push_back({"keepXp", f_bool, 0, 1, 0, false});
		h.push_back({"unique", f_bool, 0, 1, 0, false});
		h.push_back({"portrait", f_asset, 0, 0, 0, false});
		auto& a = t[k_ability];
		a.push_back({"name", f_text, 0, 64, 0, false});
		a.push_back({"tooltip", f_text, 0, 400, 0, false});
		a.push_back({"target", f_choice, 0, 4, 0, false});
		a.push_back({"effect", f_choice, 0, 5, 0, false});
		a.push_back({"ranks", f_int, 1, 10, 0, false});
		a.push_back({"heroLevel", f_int, 1, 100, 0, false});
		a.push_back({"levelStep", f_int, 0, 20, 0, false});
		a.push_back({"cost", f_int, 0, 1000, 0, false});
		a.push_back({"costPerRank", f_int, -1000, 1000, 0, false});
		a.push_back({"cooldown", f_int, 0, 3600, 0, false});
		a.push_back({"cooldownPerRank", f_int, -3600, 3600, 0, false});
		a.push_back({"amount", f_int, 0, 100000, 0, false});
		a.push_back({"amountPerRank", f_int, 0, 100000, 0, false});
		a.push_back({"range", f_int, 0, 32, 0, false});
		a.push_back({"radius", f_int, 0, 16, 0, false});
		a.push_back({"duration", f_int, 0, 3600, 0, false});
		a.push_back({"castTime", f_int, 0, 10000, 0, false});
		a.push_back({"summon", f_ref, 0, 0, U, true});
		a.push_back({"script", f_text, 0, 64, 0, false});
		a.push_back({"slot", f_int, 1, 9, 0, false});
		a.push_back({"hotkey", f_text, 0, 1, 0, false});
		a.push_back({"icon", f_asset, 0, 0, 0, false});
	}
	return t[kind >= 0 && kind < k_count ? kind : 0];
}

struct def {
	std::string id;    // "<mod>:<kind>:<local id>"
	std::string mod;
	int kind = -1;
	int index = -1;    // unit / building: custom type id (>= 256); hero / ability: 0-based in its kind
	mini_json::value v; // the definition object as written
	long long num(const char* key, long long d = 0) const { return v.int_at(key, d); }
	std::string str(const char* key) const { return v.str_at(key); }
};

struct registry {
	std::vector<def> defs;          // sorted by id once finished
	std::vector<std::string> errors; // non-empty: the set cannot be used
	std::map<std::string, size_t> by_id;
	int count(int kind) const {
		int n = 0;
		for (auto& d : defs) n += d.kind == kind;
		return n;
	}
	const def* find(const std::string& id) const {
		auto i = by_id.find(id);
		return i == by_id.end() ? nullptr : &defs[i->second];
	}
	// "base:N" -> N; a custom unit / building id -> its index (>= 256); else -1.
	int unit_type(const std::string& ref) const {
		if (ref.compare(0, 5, "base:") == 0) {
			char* end = nullptr;
			long n = strtol(ref.c_str() + 5, &end, 10);
			return end && !*end && ref.size() > 5 && n >= 0 && n < base_unit_count ? (int)n : -1;
		}
		const def* d = find(ref);
		return d && (d->kind == k_unit || d->kind == k_building) ? d->index : -1;
	}
};

inline bool safe_local_id(const std::string& s) {
	if (s.empty() || s.size() > 48 || s[0] < 'a' || s[0] > 'z') return false;
	for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
	return true;
}

// One mod's extensions.json; problems go to r.errors (prefixed with the mod id).
inline void add_file(registry& r, const std::string& mod, const char* data, size_t size) {
	auto fail = [&](const std::string& s) { r.errors.push_back(mod + ": " + s); };
	mini_json::value root;
	std::string err;
	if (!mini_json::parse(data, size, root, &err)) { fail("extensions.json: " + err); return; }
	if (root.kind != mini_json::value::object_v) { fail("extensions.json is not an object"); return; }
	auto* fmt = root.get("format");
	if (!fmt || !fmt->is_int() || fmt->as_int() != format) { fail("extensions.json format is not " + std::to_string(format)); return; }
	if (root.str_at("mod") != mod) { fail("extensions.json belongs to mod '" + root.str_at("mod") + "'"); return; }
	auto* list = root.get("definitions");
	if (!list || list->kind != mini_json::value::array_v) { fail("extensions.json has no definitions"); return; }
	for (auto& o : list->arr) {
		if (o.kind != mini_json::value::object_v) { fail("a definition is not an object"); continue; }
		def d;
		d.id = o.str_at("id");
		d.mod = mod;
		d.kind = kind_of(o.str_at("kind"));
		if (d.kind < 0) { fail(d.id + ": unknown kind '" + o.str_at("kind") + "'"); continue; }
		std::string pre = mod + ":" + kind_name(d.kind) + ":";
		if (d.id.compare(0, pre.size(), pre) != 0 || !safe_local_id(d.id.substr(pre.size()))) { fail(d.id + ": the id is not " + pre + "<a-z0-9_>"); continue; }
		if (r.by_id.count(d.id)) { fail(d.id + ": defined twice"); continue; }
		d.v = o;
		r.by_id[d.id] = r.defs.size();
		r.defs.push_back(std::move(d));
	}
}

// Field checks that need every mod loaded (references), then the stable indices.
inline void finish(registry& r) {
	std::sort(r.defs.begin(), r.defs.end(), [](const def& a, const def& b) { return a.id < b.id; });
	r.by_id.clear();
	for (size_t i = 0; i != r.defs.size(); ++i) r.by_id[r.defs[i].id] = i;
	auto ref_ok = [&](const def& d, const field& f, const mini_json::value& v) -> std::string {
		if (v.kind != mini_json::value::string_v) return std::string(f.key) + ": not a reference";
		const std::string& s = v.str;
		if (s.empty()) return std::string();
		if (s.compare(0, 5, "base:") == 0) {
			if (!f.base) return std::string(f.key) + ": a game unit cannot be used here";
			return r.unit_type(s) < 0 ? std::string(f.key) + ": " + s + " is not a game unit" : std::string();
		}
		const def* o = r.find(s);
		if (!o) return std::string(f.key) + ": " + s + " does not exist";
		if (!(f.refs & (1u << o->kind))) return std::string(f.key) + ": " + s + " is a " + kind_name(o->kind);
		if (o == &d && std::string(f.key) == "requires") return "requires itself";
		return std::string();
	};
	int next_type = first_custom_type, next_hero = 0, next_ability = 0;
	for (auto& d : r.defs) {
		for (const field& f : fields(d.kind)) {
			const mini_json::value* v = d.v.get(f.key);
			if (!v) continue; // the editor writes every field; an older file may lack new ones
			std::string bad;
			switch (f.type) {
			case f_int: case f_choice: case f_base_unit: case f_weapon:
				if (!v->is_int()) bad = std::string(f.key) + ": not a whole number";
				else if (v->as_int() < f.min || v->as_int() > f.max) bad = std::string(f.key) + ": " + std::to_string(v->as_int()) + " is out of range";
				break;
			case f_text:
				if (v->kind != mini_json::value::string_v) bad = std::string(f.key) + ": not text";
				break;
			case f_bool:
				if (v->kind != mini_json::value::bool_v) bad = std::string(f.key) + ": not true / false";
				break;
			case f_asset:
				if (v->kind != mini_json::value::string_v) bad = std::string(f.key) + ": not a file path";
				else if (v->str.find("..") != std::string::npos || (!v->str.empty() && (v->str[0] == '/' || v->str[0] == '\\')))
					bad = std::string(f.key) + ": unsafe path " + v->str;
				break;
			case f_ints:
				if (v->kind != mini_json::value::array_v) bad = std::string(f.key) + ": not a list of numbers";
				else for (auto& x : v->arr) if (!x.is_int() || x.as_int() < f.min || x.as_int() > f.max) { bad = std::string(f.key) + ": a value is out of range"; break; }
				break;
			case f_ref:
				bad = ref_ok(d, f, *v);
				break;
			case f_refs:
				if (v->kind != mini_json::value::array_v) bad = std::string(f.key) + ": not a list";
				else for (auto& x : v->arr) if (!(bad = ref_ok(d, f, x)).empty()) break;
				break;
			}
			if (!bad.empty()) r.errors.push_back(d.id + ": " + bad);
		}
		if (d.kind == k_unit || d.kind == k_building) d.index = next_type++;
		else d.index = d.kind == k_hero ? next_hero++ : next_ability++;
	}
	if (next_type - first_custom_type > max_custom_types)
		r.errors.push_back("too many new units and buildings (" + std::to_string(next_type - first_custom_type) + ", at most " + std::to_string(max_custom_types) + ")");
}

// A definition's "abilities" as ability indices.
inline std::vector<int> ability_refs(const registry& r, const def& d) {
	std::vector<int> v;
	if (auto* a = d.v.get("abilities")) if (a->kind == mini_json::value::array_v)
		for (auto& x : a->arr) if (x.kind == mini_json::value::string_v) {
			const def* ab = r.find(x.str);
			if (ab && ab->kind == k_ability) v.push_back(ab->index);
		}
	return v;
}

// The units / buildings of a finished, error-free registry as engine definitions
// (Def = bwgame::ext_unit_def), in type id order: out[k] is type first_custom_type + k.
template<typename Def, typename Vec>
void to_engine(const registry& r, Vec& out) {
	out.clear();
	if (!r.errors.empty()) return;
	auto refs = [&](const def& d, const char* key) {
		std::vector<int> v;
		if (auto* a = d.v.get(key)) if (a->kind == mini_json::value::array_v)
			for (auto& x : a->arr) if (x.kind == mini_json::value::string_v) { int t = r.unit_type(x.str); if (t >= 0) v.push_back(t); }
		return v;
	};
	auto ref = [&](const def& d, const char* key) { std::string s = d.str(key); return s.empty() ? -1 : r.unit_type(s); };
	for (auto& d : r.defs) {
		if (d.kind != k_unit && d.kind != k_building) continue;
		Def e;
		e.id = d.id.c_str();
		e.name = d.str("name").c_str();
		e.building = d.kind == k_building;
		e.parent = (int)d.num("parent", 0);
		e.hp = (int)d.num("hp", 1);
		e.shields = (int)d.num("shields", 0);
		e.armor = (int)d.num("armor", 0);
		e.energy = (int)d.num("energy", 0);
		e.speed = e.building ? -1 : (int)d.num("speed", -1);
		e.sight = (int)d.num("sight", 7);
		e.supply = (int)d.num("supply", 0);
		e.weapon = (int)d.num("weapon", -1);
		e.minerals = (int)d.num("minerals", 0);
		e.gas = (int)d.num("gas", 0);
		e.time = (int)d.num("time", 1);
		e.width = (int)d.num("width", 1);
		e.height = (int)d.num("height", 1);
		if (e.building) {
			e.builder = ref(d, "builder");
			for (int t : refs(d, "produces")) e.produces.push_back(t);
		} else e.producer = ref(d, "producer");
		for (int t : refs(d, "requires")) e.requires.push_back(t);
		for (int a : ability_refs(r, d)) e.abilities.push_back(a);
		e.hotkey = d.str("hotkey").c_str();
		out.push_back(std::move(e));
	}
}

// The heroes of a finished, error-free registry (Def = bwgame::ext_hero_def), in hero index order.
template<typename Def, typename Vec>
void to_engine_heroes(const registry& r, Vec& out) {
	out.clear();
	if (!r.errors.empty()) return;
	for (auto& d : r.defs) {
		if (d.kind != k_hero) continue;
		Def e;
		e.id = d.id.c_str();
		e.name = d.str("name").c_str();
		std::string unit = d.str("unit");
		e.unit = unit.empty() ? -1 : r.unit_type(unit);
		e.max_level = (int)d.num("maxLevel", 1);
		if (auto* a = d.v.get("xp")) if (a->kind == mini_json::value::array_v)
			for (auto& x : a->arr) if (x.is_int()) e.xp.push_back((int)x.as_int());
		e.skill_points = (int)d.num("skillPoints", 0);
		e.xp_kill = (int)d.num("xpKill", 0);
		e.xp_radius = (int)d.num("xpRadius", 0);
		e.xp_shared = d.v.bool_at("xpShared", false);
		e.strength = (int)d.num("strength", 0);
		e.agility = (int)d.num("agility", 0);
		e.intelligence = (int)d.num("intelligence", 0);
		e.hp_per_level = (int)d.num("hpPerLevel", 0);
		e.armor_per_level = (int)d.num("armorPerLevel", 0);
		e.damage_per_level = (int)d.num("damagePerLevel", 0);
		e.energy_per_level = (int)d.num("energyPerLevel", 0);
		e.respawn = d.v.bool_at("respawn", false);
		e.respawn_time = (int)d.num("respawnTime", 0);
		e.keep_xp = d.v.bool_at("keepXp", false);
		e.unique = d.v.bool_at("unique", false);
		for (int a : ability_refs(r, d)) e.abilities.push_back(a);
		out.push_back(std::move(e));
	}
}

// The abilities of a finished, error-free registry (Def = bwgame::ext_ability_def), in ability index order.
template<typename Def, typename Vec>
void to_engine_abilities(const registry& r, Vec& out) {
	out.clear();
	if (!r.errors.empty()) return;
	for (auto& d : r.defs) {
		if (d.kind != k_ability) continue;
		Def e;
		e.id = d.id.c_str();
		e.name = d.str("name").c_str();
		e.tooltip = d.str("tooltip").c_str();
		e.target = (int)d.num("target", 0);
		e.effect = (int)d.num("effect", 0);
		e.ranks = (int)d.num("ranks", 1);
		e.hero_level = (int)d.num("heroLevel", 1);
		e.level_step = (int)d.num("levelStep", 0);
		e.cost = (int)d.num("cost", 0);
		e.cost_per_rank = (int)d.num("costPerRank", 0);
		e.cooldown = (int)d.num("cooldown", 0);
		e.cooldown_per_rank = (int)d.num("cooldownPerRank", 0);
		e.amount = (int)d.num("amount", 0);
		e.amount_per_rank = (int)d.num("amountPerRank", 0);
		e.range = (int)d.num("range", 0);
		e.radius = (int)d.num("radius", 0);
		e.duration = (int)d.num("duration", 0);
		e.cast_time = (int)d.num("castTime", 0);
		std::string summon = d.str("summon");
		e.summon = summon.empty() ? -1 : r.unit_type(summon);
		e.slot = (int)d.num("slot", 1);
		e.hotkey = d.str("hotkey").c_str();
		e.event = d.str("script").c_str();
		out.push_back(std::move(e));
	}
}

} // namespace oeext
