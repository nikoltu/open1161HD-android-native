#ifndef BWGAME_OESCRIPT_H
#define BWGAME_OESCRIPT_H

// OEScript runtime: reads the World Editor's .obc bytecode (OeVm.Compiled.write) and runs it
// with the editor VM's semantics. The program is static map data (game_state); everything the
// script changes lives in `runtime` (state), which savegame.h serializes and sync.h hashes.

#include "util.h"

#include <cstdint>
#include <cstring>
#include <deque>

namespace bwgame {
namespace oescript {

enum { bytecode_version = 1, api_version = 3, step_budget = 100000, frame_budget = 2000000, max_text = 65536, max_queue = 100000,
	max_raised = 10000 };

enum op_t {
	PUSH_INT = 1, PUSH_STR, PUSH_UNIT, PUSH_NULL, PUSH_BOOL, LOAD, STORE, CALL, POP, ADD, SUB, MUL, DIV, MOD,
	EQ, NE, LT, LE, GT, GE, NOT, NEG, CONCAT, JMP, JF, END
};

enum fn_t {
	fn_location, fn_objective_set, fn_objective_complete, fn_objective_fail, fn_timer_start, fn_timer_stop, fn_timer_left,
	fn_counter_get, fn_counter_set, fn_counter_add, fn_switch_get, fn_switch_set, fn_units_spawn, fn_units_count,
	fn_units_kill, fn_units_give, fn_resources_add, fn_resources_minerals, fn_ai_attack, fn_mission_victory,
	fn_mission_defeat, fn_text_show, fn_sound_play, fn_camera_center, fn_random, fn_game_frame,
	ev_game_started, ev_frame_every, ev_timer_expired, ev_unit_entered, ev_unit_killed, ev_counter_changed, ev_switch_changed,
	// API 2: appended in OeApi.BY_ID order, never renumbered.
	fn_units_remove, fn_units_order, fn_units_set_hp, fn_units_set_shields, fn_units_set_energy, fn_units_set_invincible,
	fn_resources_gas, fn_resources_set, fn_deaths_get, fn_deaths_set, fn_score_get, fn_score_set, fn_player_allied,
	fn_player_alliance, fn_mission_draw, fn_location_move, fn_minimap_ping, fn_timer_show, fn_objective_state, fn_ai_run,
	ev_unit_left, ev_unit_trained, ev_building_finished, ev_player_left,
	// API 3: heroes, abilities, signals, event context.
	fn_hero_add_xp, fn_hero_set_level, fn_hero_level, fn_hero_xp, fn_hero_skill_points, fn_hero_add_skill_points, fn_hero_learn,
	fn_hero_unlearn, fn_hero_attribute, fn_hero_set_attribute, fn_ability_rank, fn_ability_cooldown, fn_ability_ready,
	fn_ability_set_cooldown, fn_ability_cast, fn_ability_cast_at, fn_ability_cast_on, fn_signal_send, fn_context_owner,
	fn_context_type, fn_context_x, fn_context_y, fn_context_value, fn_location_set_center,
	ev_signal_received, ev_hero_leveled,
	fn_count
};

// Parameter count and whether a value is returned, by OeApi id.
static const int fn_params[fn_count] = {1, 1, 1, 1, 2, 1, 1, 1, 2, 2, 1, 2, 4, 3, 4, 5, 3, 1, 2, 1, 1, 2, 2, 2, 1, 0, 0, 1, 1, 3, 2, 1, 1,
	4, 5, 5, 5, 5, 4, 1, 3, 2, 3, 2, 2, 2, 3, 0, 2, 2, 1, 1, 3, 3, 2, 2, 1,
	4, 4, 2, 2, 2, 4, 5, 4, 3, 5, 3, 3, 3, 5, 4, 5, 7, 2, 0, 0, 0, 0, 0, 3, 2, 2};
static const bool fn_returns[fn_count] = {true, false, false, false, false, false, true, true, false, false, true, false, false, true,
	false, false, false, true, false, false, false, false, false, false, true, true, false, false, false, false, false, false, false,
	true, false, false, false, false, false, true, false, true, false, true, false, true, false, false, false, false, false, true, false,
	false, false, false, false,
	false, false, true, true, true, false, true, true, true, false, true, true, true, false, true, true, true, false, true, true,
	true, true, true, false, false, false};

inline bool is_event(int id) {
	return (id >= ev_game_started && id <= ev_switch_changed) || (id >= ev_unit_left && id <= ev_player_left) || id >= ev_signal_received;
}
// The API version that introduced an id; the .obc header's api is the highest one a script uses.
inline int since(int id) { return id < fn_units_remove ? 1 : id < fn_hero_add_xp ? 2 : 3; }

inline const char* unit_name(int id) {
	static const char* const names[228] = {
		"Terran_Marine", "Terran_Ghost", "Terran_Vulture", "Terran_Goliath", "Terran_Goliath_Turret", "Terran_Siege_Tank_Tank_Mode",
		"Terran_Siege_Tank_Tank_Mode_Turret", "Terran_SCV", "Terran_Wraith", "Terran_Science_Vessel", "Hero_Gui_Montag", "Terran_Dropship",
		"Terran_Battlecruiser", "Terran_Vulture_Spider_Mine", "Terran_Nuclear_Missile", "Terran_Civilian", "Hero_Sarah_Kerrigan", "Hero_Alan_Schezar",
		"Hero_Alan_Schezar_Turret", "Hero_Jim_Raynor_Vulture", "Hero_Jim_Raynor_Marine", "Hero_Tom_Kazansky", "Hero_Magellan",
		"Hero_Edmund_Duke_Tank_Mode", "Hero_Edmund_Duke_Tank_Mode_Turret", "Hero_Edmund_Duke_Siege_Mode", "Hero_Edmund_Duke_Siege_Mode_Turret",
		"Hero_Arcturus_Mengsk", "Hero_Hyperion", "Hero_Norad_II", "Terran_Siege_Tank_Siege_Mode", "Terran_Siege_Tank_Siege_Mode_Turret",
		"Terran_Firebat", "Spell_Scanner_Sweep", "Terran_Medic", "Zerg_Larva", "Zerg_Egg", "Zerg_Zergling", "Zerg_Hydralisk", "Zerg_Ultralisk",
		"Zerg_Broodling", "Zerg_Drone", "Zerg_Overlord", "Zerg_Mutalisk", "Zerg_Guardian", "Zerg_Queen", "Zerg_Defiler", "Zerg_Scourge",
		"Hero_Torrasque", "Hero_Matriarch", "Zerg_Infested_Terran", "Hero_Infested_Kerrigan", "Hero_Unclean_One", "Hero_Hunter_Killer",
		"Hero_Devouring_One", "Hero_Kukulza_Mutalisk", "Hero_Kukulza_Guardian", "Hero_Yggdrasill", "Terran_Valkyrie", "Zerg_Cocoon",
		"Protoss_Corsair", "Protoss_Dark_Templar", "Zerg_Devourer", "Protoss_Dark_Archon", "Protoss_Probe", "Protoss_Zealot", "Protoss_Dragoon",
		"Protoss_High_Templar", "Protoss_Archon", "Protoss_Shuttle", "Protoss_Scout", "Protoss_Arbiter", "Protoss_Carrier", "Protoss_Interceptor",
		"Hero_Dark_Templar", "Hero_Zeratul", "Hero_Tassadar_Zeratul_Archon", "Hero_Fenix_Zealot", "Hero_Fenix_Dragoon", "Hero_Tassadar", "Hero_Mojo",
		"Hero_Warbringer", "Hero_Gantrithor", "Protoss_Reaver", "Protoss_Observer", "Protoss_Scarab", "Hero_Danimoth", "Hero_Aldaris", "Hero_Artanis",
		"Critter_Rhynadon", "Critter_Bengalaas", "Special_Cargo_Ship", "Special_Mercenary_Gunship", "Critter_Scantid", "Critter_Kakaru",
		"Critter_Ragnasaur", "Critter_Ursadon", "Zerg_Lurker_Egg", "Hero_Raszagal", "Hero_Samir_Duran", "Hero_Alexei_Stukov", "Special_Map_Revealer",
		"Hero_Gerard_DuGalle", "Zerg_Lurker", "Hero_Infested_Duran", "Spell_Disruption_Web", "Terran_Command_Center", "Terran_Comsat_Station",
		"Terran_Nuclear_Silo", "Terran_Supply_Depot", "Terran_Refinery", "Terran_Barracks", "Terran_Academy", "Terran_Factory", "Terran_Starport",
		"Terran_Control_Tower", "Terran_Science_Facility", "Terran_Covert_Ops", "Terran_Physics_Lab", "Unused_Terran1", "Terran_Machine_Shop",
		"Unused_Terran2", "Terran_Engineering_Bay", "Terran_Armory", "Terran_Missile_Turret", "Terran_Bunker", "Special_Crashed_Norad_II",
		"Special_Ion_Cannon", "Powerup_Uraj_Crystal", "Powerup_Khalis_Crystal", "Zerg_Infested_Command_Center", "Zerg_Hatchery", "Zerg_Lair",
		"Zerg_Hive", "Zerg_Nydus_Canal", "Zerg_Hydralisk_Den", "Zerg_Defiler_Mound", "Zerg_Greater_Spire", "Zerg_Queens_Nest",
		"Zerg_Evolution_Chamber", "Zerg_Ultralisk_Cavern", "Zerg_Spire", "Zerg_Spawning_Pool", "Zerg_Creep_Colony", "Zerg_Spore_Colony",
		"Unused_Zerg1", "Zerg_Sunken_Colony", "Special_Overmind_With_Shell", "Special_Overmind", "Zerg_Extractor", "Special_Mature_Chrysalis",
		"Special_Cerebrate", "Special_Cerebrate_Daggoth", "Unused_Zerg2", "Protoss_Nexus", "Protoss_Robotics_Facility", "Protoss_Pylon",
		"Protoss_Assimilator", "Unused_Protoss1", "Protoss_Observatory", "Protoss_Gateway", "Unused_Protoss2", "Protoss_Photon_Cannon",
		"Protoss_Citadel_of_Adun", "Protoss_Cybernetics_Core", "Protoss_Templar_Archives", "Protoss_Forge", "Protoss_Stargate",
		"Special_Stasis_Cell_Prison", "Protoss_Fleet_Beacon", "Protoss_Arbiter_Tribunal", "Protoss_Robotics_Support_Bay", "Protoss_Shield_Battery",
		"Special_Khaydarin_Crystal_Form", "Special_Protoss_Temple", "Special_XelNaga_Temple", "Resource_Mineral_Field",
		"Resource_Mineral_Field_Type_2", "Resource_Mineral_Field_Type_3", "Unused_Cave", "Unused_Cave_In", "Unused_Cantina", "Unused_Mining_Platform",
		"Unused_Independant_Command_Center", "Special_Independant_Starport", "Unused_Independant_Jump_Gate", "Unused_Ruins",
		"Unused_Khaydarin_Crystal_Formation", "Resource_Vespene_Geyser", "Special_Warp_Gate", "Special_Psi_Disrupter", "Unused_Zerg_Marker",
		"Unused_Terran_Marker", "Unused_Protoss_Marker", "Special_Zerg_Beacon", "Special_Terran_Beacon", "Special_Protoss_Beacon",
		"Special_Zerg_Flag_Beacon", "Special_Terran_Flag_Beacon", "Special_Protoss_Flag_Beacon", "Special_Power_Generator", "Special_Overmind_Cocoon",
		"Spell_Dark_Swarm", "Special_Floor_Missile_Trap", "Special_Floor_Hatch", "Special_Upper_Level_Door", "Special_Right_Upper_Level_Door",
		"Special_Pit_Door", "Special_Right_Pit_Door", "Special_Floor_Gun_Trap", "Special_Wall_Missile_Trap", "Special_Wall_Flame_Trap",
		"Special_Right_Wall_Missile_Trap", "Special_Right_Wall_Flame_Trap", "Special_Start_Location", "Powerup_Flag", "Powerup_Young_Chrysalis",
		"Powerup_Psi_Emitter", "Powerup_Data_Disk", "Powerup_Khaydarin_Crystal", "Powerup_Mineral_Cluster_Type_1", "Powerup_Mineral_Cluster_Type_2",
		"Powerup_Protoss_Gas_Orb_Type_1", "Powerup_Protoss_Gas_Orb_Type_2", "Powerup_Zerg_Gas_Sac_Type_1", "Powerup_Zerg_Gas_Sac_Type_2",
		"Powerup_Terran_Gas_Tank_Type_1", "Powerup_Terran_Gas_Tank_Type_2",
	};
	return id >= 0 && id < 228 ? names[id] : nullptr;
}

inline a_string num(int64_t v) {
	char b[24];
	snprintf(b, sizeof(b), "%lld", (long long)v);
	return b;
}

// t: 0 null, 1 int, 2 string, 3 unit, 4 location (by name), 5 bool -- the .obc value tags.
struct value {
	int8_t t = 0;
	int64_t i = 0;
	a_string s;
	static value of_int(int64_t v) { value r; r.t = 1; r.i = v; return r; }
	static value of_str(a_string v) { value r; r.t = 2; r.s = std::move(v); return r; }
	static value of_unit(int64_t v) { value r; r.t = 3; r.i = v; return r; }
	static value of_loc(a_string v) { value r; r.t = 4; r.s = std::move(v); return r; }
	static value of_bool(bool v) { value r; r.t = 5; r.i = v ? 1 : 0; return r; }
	bool operator==(const value& o) const {
		if (t != o.t) return false;
		if (t == 2 || t == 4) return s == o.s;
		return i == o.i;
	}
	a_string text() const {
		switch (t) {
		case 1: return num(i);
		case 2: return s;
		case 3: { const char* n = unit_name((int)i); return n ? a_string(n) : "#" + num(i); }
		case 4: return "location(\"" + s + "\")";
		case 5: return i ? "true" : "false";
		default: return "null";
		}
	}
};

struct handler {
	int event = 0;
	uint32_t start = 0;
	a_vector<value> filter;
	a_string label;
	size_t slots = 0;
};

struct program {
	bool loaded = false;
	int api = 1;                   // the .obc header's API version
	bool wants_custom = false;     // signal.received
	bool wants_levels = false;     // hero.leveled
	bool wants_kills = false;
	bool wants_left = false;
	bool wants_production = false; // unit.trained / building.finished
	bool wants_players = false;    // player.left
	a_string error;
	a_vector<a_string> strings;
	a_vector<handler> handlers;
	a_vector<int32_t> code;
	a_vector<int32_t> lines;
	a_vector<int> entered_locations; // distinct unit.entered / unit.left location strings, in file order

	bool has_event(int ev) const {
		for (auto& h : handlers) if (h.event == ev) return true;
		return false;
	}

	// The .obc layout written by OeVm.Compiled.write (big-endian, Java DataOutputStream).
	bool read(const uint8_t* data, size_t size) {
		*this = program();
		const uint8_t* p = data;
		const uint8_t* e = data + size;
		bool bad = false;
		auto need = [&](size_t n) { if ((size_t)(e - p) < n) { bad = true; return false; } return true; };
		auto u8 = [&]() -> uint32_t { if (!need(1)) return 0; return *p++; };
		auto u16 = [&]() -> uint32_t { if (!need(2)) return 0; uint32_t v = (uint32_t)p[0] << 8 | p[1]; p += 2; return v; };
		auto u32 = [&]() -> uint32_t { if (!need(4)) return 0; uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; p += 4; return v; };
		auto str = [&](uint32_t len) -> a_string {
			if (len > (1u << 20) || !need(len)) { bad = true; return a_string(); }
			a_string s((const char*)p, len);
			p += len;
			return s;
		};
		auto fail = [&](const char* m) { *this = program(); error = m; return false; };
		if (!need(4) || memcmp(p, "OEBC", 4)) return fail("not OEScript bytecode");
		p += 4;
		uint32_t bv = u16(), av = u16();
		if (bad) return fail("truncated bytecode");
		if (bv != bytecode_version) return fail("unsupported bytecode version");
		if (av > api_version) return fail("the script needs a newer engine (OEScript API version)");
		api = (int)av;
		uint32_t ns = u32();
		if (bad || ns > 100000) return fail("bad string table");
		for (uint32_t i = 0; i != ns && !bad; ++i) strings.push_back(str(u32()));
		uint32_t nh = u32();
		if (bad || nh > 10000) return fail("bad handler table");
		for (uint32_t i = 0; i != nh && !bad; ++i) {
			handler h;
			h.event = (int)u16();
			h.start = u32();
			uint32_t nf = u16();
			for (uint32_t k = 0; k != nf && !bad; ++k) {
				value v;
				switch (u8()) {
				case 0: break;
				case 1: { uint64_t hi = u32(); uint64_t lo = u32(); v.t = 1; v.i = (int64_t)(hi << 32 | lo); break; }
				case 2: v.t = 2; v.s = str(u32()); break;
				case 3: v.t = 3; v.i = u16(); break;
				case 4: v.t = 4; v.s = str(u32()); break;
				case 5: v.t = 5; v.i = u8() ? 1 : 0; break;
				default: bad = true;
				}
				h.filter.push_back(std::move(v));
			}
			h.label = str(u32());
			h.slots = u16();
			for (size_t k = 0; k != h.slots && !bad; ++k) str(u16());
			if (bad) break;
			if (!is_event(h.event)) return fail("unknown event in bytecode");
			if (h.filter.size() != (size_t)fn_params[h.event]) return fail("bad event values in bytecode");
			handlers.push_back(std::move(h));
		}
		uint32_t n = u32();
		if (bad || n > 10000000 || (size_t)(e - p) < (size_t)n * 8) return fail("truncated bytecode");
		code.resize(n);
		lines.resize(n);
		for (uint32_t i = 0; i != n; ++i) {
			code[i] = (int32_t)u32();
			lines[i] = (int32_t)u32();
		}
		for (auto& h : handlers) if (h.start >= n) return fail("handler outside the code");
		for (auto& h : handlers) {
			if ((h.event != ev_unit_entered && h.event != ev_unit_left) || h.filter[0].t != 4) continue;
			int idx = -1;
			for (size_t k = 0; k != entered_locations.size(); ++k) if (handlers_loc(entered_locations[k]) == h.filter[0].s) idx = (int)k;
			if (idx < 0) entered_locations.push_back((int)(&h - handlers.data()));
		}
		wants_kills = has_event(ev_unit_killed);
		wants_left = has_event(ev_unit_left);
		wants_production = has_event(ev_unit_trained) || has_event(ev_building_finished);
		wants_players = has_event(ev_player_left);
		wants_custom = has_event(ev_signal_received);
		wants_levels = has_event(ev_hero_leveled);
		loaded = true;
		return true;
	}
	const a_string& handlers_loc(int handler_index) const { return handlers[handler_index].filter[0].s; }
	int line(size_t pc) const { return pc < lines.size() ? lines[pc] : 0; }
};

// The event a handler runs for (context.*): -1 / no type when the event has none.
struct event_ctx {
	int32_t owner = -1, type = -1, x = -1, y = -1, value = -1;
};
// A raised signal, or a hero level-up when the name is empty; delivered in raise order.
struct raised_event {
	a_string name;
	event_ctx ctx;
};

struct kill_event {
	int owner;
	int type;
};
struct entered_unit {
	uint16_t id;
	int8_t owner;
	int16_t type;
	bool operator<(const entered_unit& o) const { return id < o.id; }
};
// A unit under construction / in training: raw unit id and the type it becomes when done.
struct pending_unit {
	uint16_t id;
	int16_t type;
	bool operator<(const pending_unit& o) const { return id < o.id; }
};

// Script state between frames. The run queue is always empty between frames.
struct runtime {
	bool active = false;
	bool started = false;
	bool ended = false;
	int32_t frame = 0;
	a_map<a_string, int64_t> timers;
	a_map<a_string, int64_t> counters;
	a_vector<std::pair<a_string, int>> objectives; // 0 active, 1 done, 2 failed
	std::array<bool, 256> switches_seen{};
	a_vector<a_vector<uint16_t>> entered; // per program::entered_locations, sorted unit ids inside
	a_vector<kill_event> kills;
	int32_t outcome_player = -1;
	int32_t outcome_victory = 0;
	int32_t faults = 0;
	int32_t fault_frame = -1;
	a_string last_fault;
	uint32_t objectives_version = 0;
	// API 2 production / player events; only these programs serialize as version 2.
	bool ext = false;
	a_vector<pending_unit> incomplete;
	a_vector<uint16_t> born; // the second unit of a two-in-one egg, completed in the same frame it was created
	uint32_t humans = 0;     // players 0-7 still in the game as people
	// API 3: signals and level-ups waiting for the script; only API 3 programs serialize as version 3.
	bool v3 = false;
	a_vector<raised_event> raised;
	size_t raised_early = 0; // queued before this frame's simulation ran (set every frame, not saved)

	int objective(const a_string& id) const {
		for (auto& o : objectives) if (o.first == id) return o.second;
		return -1;
	}

	a_vector<uint8_t> serialize() const {
		a_vector<uint8_t> o;
		auto u32 = [&](uint32_t v) { for (int i = 0; i != 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); };
		auto i64 = [&](int64_t v) { u32((uint32_t)(uint64_t)v); u32((uint32_t)((uint64_t)v >> 32)); };
		auto str = [&](const a_string& s) { u32((uint32_t)s.size()); o.insert(o.end(), s.begin(), s.end()); };
		u32(v3 ? 3 : ext ? 2 : 1);
		u32((active ? 1 : 0) | (started ? 2 : 0) | (ended ? 4 : 0) | (v3 && ext ? 8 : 0));
		u32((uint32_t)frame);
		u32((uint32_t)timers.size());
		for (auto& v : timers) { str(v.first); i64(v.second); }
		u32((uint32_t)counters.size());
		for (auto& v : counters) { str(v.first); i64(v.second); }
		u32((uint32_t)objectives.size());
		for (auto& v : objectives) { str(v.first); u32((uint32_t)v.second); }
		for (size_t i = 0; i != 256; i += 32) {
			uint32_t b = 0;
			for (size_t k = 0; k != 32; ++k) if (switches_seen[i + k]) b |= 1u << k;
			u32(b);
		}
		u32((uint32_t)entered.size());
		for (auto& v : entered) { u32((uint32_t)v.size()); for (auto id : v) u32(id); }
		u32((uint32_t)kills.size());
		for (auto& k : kills) { u32((uint32_t)k.owner); u32((uint32_t)k.type); }
		u32((uint32_t)outcome_player);
		u32((uint32_t)outcome_victory);
		u32((uint32_t)faults);
		u32((uint32_t)fault_frame);
		str(last_fault);
		if (ext || v3) {
			u32((uint32_t)incomplete.size());
			for (auto& v : incomplete) { u32(v.id); u32((uint32_t)(int32_t)v.type); }
			u32((uint32_t)born.size());
			for (auto id : born) u32(id);
			u32(humans);
		}
		if (v3) {
			u32((uint32_t)raised.size());
			for (auto& e : raised) {
				str(e.name);
				u32((uint32_t)e.ctx.owner); u32((uint32_t)e.ctx.type); u32((uint32_t)e.ctx.x); u32((uint32_t)e.ctx.y); u32((uint32_t)e.ctx.value);
			}
		}
		return o;
	}
	bool deserialize(const uint8_t* p, size_t size) {
		const uint8_t* e = p + size;
		bool bad = false;
		auto u32 = [&]() -> uint32_t {
			if (e - p < 4) { bad = true; return 0; }
			uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
			p += 4;
			return v;
		};
		auto i64 = [&]() -> int64_t { uint64_t lo = u32(); uint64_t hi = u32(); return (int64_t)(hi << 32 | lo); };
		auto str = [&]() -> a_string {
			uint32_t n = u32();
			if (bad || (size_t)(e - p) < n) { bad = true; return a_string(); }
			a_string s((const char*)p, n);
			p += n;
			return s;
		};
		auto count = [&]() -> uint32_t { uint32_t n = u32(); if (n > (uint32_t)(e - p)) { bad = true; return 0; } return n; };
		runtime r;
		uint32_t version = u32();
		if (version < 1 || version > 3) return false;
		uint32_t f = u32();
		r.v3 = version == 3;
		r.ext = version == 2 || (r.v3 && (f & 8));
		r.active = f & 1; r.started = (f & 2) != 0; r.ended = (f & 4) != 0;
		r.frame = (int32_t)u32();
		for (uint32_t n = count(), i = 0; i != n && !bad; ++i) { a_string k = str(); r.timers[k] = i64(); }
		for (uint32_t n = count(), i = 0; i != n && !bad; ++i) { a_string k = str(); r.counters[k] = i64(); }
		for (uint32_t n = count(), i = 0; i != n && !bad; ++i) { a_string k = str(); r.objectives.emplace_back(k, (int)u32()); }
		for (size_t i = 0; i != 256; i += 32) {
			uint32_t b = u32();
			for (size_t k = 0; k != 32; ++k) r.switches_seen[i + k] = (b >> k) & 1;
		}
		for (uint32_t n = count(), i = 0; i != n && !bad; ++i) {
			r.entered.emplace_back();
			for (uint32_t m = count(), k = 0; k != m && !bad; ++k) r.entered.back().push_back((uint16_t)u32());
		}
		for (uint32_t n = count(), i = 0; i != n && !bad; ++i) { kill_event k; k.owner = (int)u32(); k.type = (int)u32(); r.kills.push_back(k); }
		r.outcome_player = (int32_t)u32();
		r.outcome_victory = (int32_t)u32();
		r.faults = (int32_t)u32();
		r.fault_frame = (int32_t)u32();
		r.last_fault = str();
		if (version >= 2) {
			for (uint32_t n = count(), i = 0; i != n && !bad; ++i) {
				pending_unit v;
				v.id = (uint16_t)u32();
				v.type = (int16_t)(int32_t)u32();
				r.incomplete.push_back(v);
			}
			for (uint32_t n = count(), i = 0; i != n && !bad; ++i) r.born.push_back((uint16_t)u32());
			r.humans = u32();
		}
		if (r.v3) {
			for (uint32_t n = count(), i = 0; i != n && !bad; ++i) {
				raised_event e;
				e.name = str();
				e.ctx.owner = (int32_t)u32(); e.ctx.type = (int32_t)u32(); e.ctx.x = (int32_t)u32(); e.ctx.y = (int32_t)u32(); e.ctx.value = (int32_t)u32();
				r.raised.push_back(std::move(e));
			}
		}
		if (bad) return false;
		r.objectives_version = objectives_version + 1;
		*this = std::move(r);
		return true;
	}
	uint64_t hash() const {
		uint64_t h = 1469598103934665603ull;
		for (uint8_t b : serialize()) { h ^= b; h *= 1099511628211ull; }
		return h;
	}
};

struct fault_error {
	a_string message;
};

// Runs a program against a host (state_functions in the game, a model in the tests).
template<typename host_T>
struct machine {
	const program& prog;
	runtime& rt;
	host_T& host;
	machine(const program& prog, runtime& rt, host_T& host) : prog(prog), rt(rt), host(host) {}

	struct run_t {
		size_t h;
		a_vector<value> event;
		a_vector<value> locals;
		a_vector<value> stack;
		size_t pc;
		int steps = 0;
		event_ctx ctx;
	};
	std::deque<run_t> queue;
	int64_t frame_steps = 0;
	event_ctx ctx;          // the running handler's event
	int raised_frame = 0;   // signals / level-ups delivered this frame

	void log(const a_string& s) {
		if (host.oes_tracing()) host.oes_log(rt.frame, s);
	}
	static a_string who(int p) { return "P" + num(p + 1); }
	[[noreturn]] static void fault(a_string m) { throw fault_error{std::move(m)}; }

	void fire(int ev, a_vector<value> args, const event_ctx& c = event_ctx()) {
		if (rt.ended) return;
		if (ev == ev_game_started) rt.started = true;
		for (size_t i = 0; i != prog.handlers.size(); ++i) {
			auto& h = prog.handlers[i];
			if (h.event != ev) continue;
			bool match = true;
			for (size_t k = 0; k != h.filter.size(); ++k) {
				const value& f = h.filter[k];
				if (f.t == 0) continue;
				if (k >= args.size()) { match = false; break; }
				if (f.t == 2 && args[k].t == 3) {
					// a mod unit type in a unit filter, by its "mod:unit:id"
					int t = host.oes_unit_ref(f.s);
					if (t < 0 || t != args[k].i) { match = false; break; }
					continue;
				}
				if (!(f == args[k])) { match = false; break; }
			}
			if (match) enqueue(i, args, c);
		}
	}
	void enqueue(size_t h, const a_vector<value>& args, const event_ctx& c = event_ctx()) {
		if (queue.size() >= max_queue) {
			record_fault(prog.handlers[h].label + ": too many events queued: stopped");
			return;
		}
		run_t r;
		r.h = h;
		r.event = args;
		r.locals.resize(prog.handlers[h].slots);
		r.pc = prog.handlers[h].start;
		r.ctx = c;
		queue.push_back(std::move(r));
	}
	void record_fault(const a_string& m) {
		++rt.faults;
		rt.fault_frame = rt.frame;
		rt.last_fault = m;
		log(m);
	}

	void run() {
		while (!queue.empty() && !rt.ended) {
			run_t r = std::move(queue.front());
			queue.pop_front();
			ctx = r.ctx;
			int before = 0;
			const bool lines = host.oes_tracing();
			if (lines) log("@run " + num((int64_t)r.h) + " " + prog.handlers[r.h].label);
			try {
				while (true) {
					int at = prog.line(r.pc);
					if (lines && at != before && at > 0) log("@line " + num((int64_t)r.h) + " " + num(at));
					before = at;
					if (one(r)) break;
				}
			} catch (fault_error& f) {
				record_fault(prog.handlers[r.h].label + ", line " + num(before) + ": " + f.message);
				if (frame_steps > frame_budget) queue.clear();
			}
		}
		queue.clear();
	}

	value pop(run_t& r) {
		if (r.stack.empty()) fault("bad bytecode (stack)");
		value v = std::move(r.stack.back());
		r.stack.pop_back();
		return v;
	}
	int64_t pop_int(run_t& r) { value v = pop(r); if (v.t != 1) fault("bad bytecode (int)"); return v.i; }
	bool pop_bool(run_t& r) { value v = pop(r); if (v.t != 5) fault("bad bytecode (bool)"); return v.i != 0; }
	int32_t arg(run_t& r) {
		if (r.pc >= prog.code.size()) fault("bad bytecode (pc)");
		return prog.code[r.pc++];
	}

	bool one(run_t& r) {
		if (++r.steps > step_budget) fault("more than " + num(step_budget) + " steps in one event: stopped");
		if (++frame_steps > frame_budget) fault("more than " + num(frame_budget) + " steps in one frame: stopped");
		int op = arg(r);
		auto& s = r.stack;
		switch (op) {
		case PUSH_INT: s.push_back(value::of_int(arg(r))); return false;
		case PUSH_STR: {
			int32_t i = arg(r);
			if (i < 0 || (size_t)i >= prog.strings.size()) fault("bad bytecode (string)");
			s.push_back(value::of_str(prog.strings[i]));
			return false;
		}
		case PUSH_UNIT: s.push_back(value::of_unit(arg(r))); return false;
		case PUSH_BOOL: s.push_back(value::of_bool(arg(r) != 0)); return false;
		case PUSH_NULL: s.push_back(value()); return false;
		case LOAD: {
			int32_t i = arg(r);
			if (i < 0 || (size_t)i >= r.locals.size()) fault("bad bytecode (slot)");
			s.push_back(r.locals[i]);
			return false;
		}
		case STORE: {
			int32_t i = arg(r);
			if (i < 0 || (size_t)i >= r.locals.size()) fault("bad bytecode (slot)");
			r.locals[i] = pop(r);
			return false;
		}
		case POP: pop(r); return false;
		case ADD: case SUB: case MUL: case DIV: case MOD: {
			int64_t b = pop_int(r), a = pop_int(r);
			if ((op == DIV || op == MOD) && b == 0) fault("division by zero");
			uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
			int64_t v;
			if (op == ADD) v = (int64_t)(ua + ub);
			else if (op == SUB) v = (int64_t)(ua - ub);
			else if (op == MUL) v = (int64_t)(ua * ub);
			else if (b == -1) v = op == DIV ? (int64_t)(0 - ua) : 0;
			else v = op == DIV ? a / b : a % b;
			s.push_back(value::of_int((int32_t)(uint32_t)(uint64_t)v));
			return false;
		}
		case CONCAT: {
			value b = pop(r), a = pop(r);
			a_string t = a.text() + b.text();
			if (t.size() > max_text) fault("text longer than " + num(max_text) + " bytes");
			s.push_back(value::of_str(std::move(t)));
			return false;
		}
		case EQ: case NE: {
			value b = pop(r), a = pop(r);
			bool eq = a == b;
			s.push_back(value::of_bool(op == EQ ? eq : !eq));
			return false;
		}
		case LT: case LE: case GT: case GE: {
			int64_t b = pop_int(r), a = pop_int(r);
			s.push_back(value::of_bool(op == LT ? a < b : op == LE ? a <= b : op == GT ? a > b : a >= b));
			return false;
		}
		case NOT: s.push_back(value::of_bool(!pop_bool(r))); return false;
		case NEG: s.push_back(value::of_int((int64_t)(0 - (uint64_t)pop_int(r)))); return false;
		case JMP: {
			int32_t t = arg(r);
			if (t < 0 || (size_t)t > prog.code.size()) fault("bad bytecode (jump)");
			r.pc = (size_t)t;
			return false;
		}
		case JF: {
			int32_t t = arg(r);
			if (t < 0 || (size_t)t > prog.code.size()) fault("bad bytecode (jump)");
			if (!pop_bool(r)) r.pc = (size_t)t;
			return false;
		}
		case CALL: {
			int32_t f = arg(r);
			if (f < 0 || f >= fn_count || is_event(f)) fault("no such function " + num(f));
			a_vector<value> a((size_t)fn_params[f]);
			for (size_t i = a.size(); i-- > 0;) a[i] = pop(r);
			value res = call(f, a);
			if (fn_returns[f]) s.push_back(std::move(res));
			return rt.ended;
		}
		case END: return true;
		default: fault("bad instruction " + num(op));
		}
	}

	static const a_string& need_str(const value& v) { if (v.t != 2) fault("bad bytecode (text)"); return v.s; }
	static int64_t need_int(const value& v) { if (v.t != 1) fault("bad bytecode (int)"); return v.i; }
	static int player(const value& v) {
		int64_t p = need_int(v);
		if (p < 0 || p > 11) fault("player " + num(p) + " is not 0-11");
		return (int)p;
	}
	int unit_type(const value& v) {
		if (v.t == 2) {
			// an Extended mod type by its namespaced id
			int t = host.oes_unit_ref(v.s);
			if (t < 0) fault("no active mod defines the unit \"" + v.s + "\"");
			return t;
		}
		if (v.t != 3) fault("bad bytecode (unit)");
		if (v.i < 0 || !host.oes_unit_valid((int)v.i)) fault("unit #" + num(v.i) + " does not exist");
		return (int)v.i;
	}
	int location(const value& v) {
		if (v.t != 4) fault("bad bytecode (location)");
		int i = host.oes_location(v.s);
		if (i < 0) fault("the map has no location \"" + v.s + "\"");
		return i;
	}
	void set_objective(const a_string& id, int status) {
		static const char* const names[3] = {"active", "done", "failed"};
		bool found = false;
		for (auto& o : rt.objectives) if (o.first == id) { o.second = status; found = true; }
		if (!found) rt.objectives.emplace_back(id, status);
		++rt.objectives_version;
		log("objective " + id + ": " + names[status]);
	}
	static int64_t count_arg(const value& v) { return std::min<int64_t>(need_int(v), 1700); }
	static int percent(const value& v) {
		int64_t n = need_int(v);
		if (n < 0 || n > 100) fault("percent " + num(n) + " is not 0-100");
		return (int)n;
	}
	static int64_t amount(const value& v, const char* what) {
		int64_t n = need_int(v);
		if (n < 0 || n > 0x7fffffff) fault(a_string(what) + " " + num(n) + " is not 0-2147483647");
		return n;
	}
	int64_t switch_index(const value& v) {
		int64_t i = need_int(v);
		if (i < 1 || i > 256) fault("switch " + num(i) + " is not 1-256");
		return i;
	}

	// API 3 arguments. `all` (the last, optional) picks every matching unit instead of the first.
	static bool flag(const value& v) {
		if (v.t == 0) return false;
		if (v.t != 5) fault("bad bytecode (bool)");
		return v.i != 0;
	}
	int hero_type(const value& v) {
		int t = unit_type(v);
		if (!host.oes_is_hero_type(t)) fault("unit " + v.text() + " is not a hero");
		return t;
	}
	int ability(const value& v) {
		const a_string& id = need_str(v);
		int a = host.oes_ability_ref(id);
		if (a < 0) fault("no active mod defines the ability \"" + id + "\"");
		return a;
	}
	static int64_t ranged(const value& v, const char* what, int64_t lo, int64_t hi) {
		int64_t n = need_int(v);
		if (n < lo || n > hi) fault(a_string(what) + " " + num(n) + " is not " + num(lo) + "-" + num(hi));
		return n;
	}
	// The player's live, completed units of a type, lowest unit id first (just that one unless all).
	a_vector<int> heroes(int p, int t, bool all) {
		a_vector<int> r;
		host.oes_units_of(p, t, r);
		if (!all && r.size() > 1) r.resize(1);
		return r;
	}
	// Those of them that have the ability (a hero's only, if hero_only): unit and slot.
	a_vector<std::pair<int, int>> holders(int p, int t, int ab, bool hero_only, bool all) {
		a_vector<int> u;
		host.oes_units_of(p, t, u);
		a_vector<std::pair<int, int>> r;
		for (int x : u) {
			int slot = host.oes_ability_slot(x, ab, hero_only);
			if (slot < 0) continue;
			r.emplace_back(x, slot);
			if (!all) break;
		}
		return r;
	}
	// A unit type in a log line; a mod type (a number past the classic ones) by its "mod:unit:id".
	a_string type_text(const value& v) { return v.t == 3 && !unit_name((int)v.i) ? host.oes_type_text((int)v.i) : v.text(); }
	a_string hero_text(int p, const value& type) { return who(p) + " hero " + type_text(type); }
	void log_levels(int p, const value& type, int from, int to) {
		for (int l = from + 1; l <= to; ++l) log(hero_text(p, type) + " reached level " + num(l));
	}
	static bool event_name_ok(const a_string& n) {
		if (n.empty() || n.size() > 64 || n[0] < 'a' || n[0] > 'z') return false;
		for (char c : n) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
		return true;
	}
	// ability.cast / cast_at / cast_on: the first holder that can cast it (every one, if all).
	value cast(int f, a_vector<value>& a) {
		int p = player(a[0]);
		int t = unit_type(a[1]);
		int ab = ability(a[2]);
		int mode = f == fn_ability_cast ? 0 : f == fn_ability_cast_at ? 1 : 2;
		int at = -1, target = -1, to_p = 0;
		bool all;
		if (mode == 0) all = flag(a[3]);
		else if (mode == 1) { at = location(a[3]); all = flag(a[4]); }
		else {
			to_p = player(a[3]);
			int to_t = unit_type(a[4]);
			int in = a[5].t == 0 ? -1 : location(a[5]);
			all = flag(a[6]);
			target = host.oes_first_unit(to_p, to_t, in);
			if (target < 0) return value::of_bool(false);
		}
		bool any = false;
		for (auto& h : holders(p, t, ab, false, true)) {
			if (!host.oes_ability_cast(h.first, h.second, mode, mode == 1 ? at : target)) continue;
			any = true;
			a_string m = who(p) + " " + type_text(a[1]) + " casts " + a[2].s;
			if (mode == 1) m += " at " + a[3].s;
			if (mode == 2) m += " on " + type_text(a[4]) + " of " + who(to_p);
			log(m);
			if (!all) break;
		}
		return value::of_bool(any);
	}

	value call(int f, a_vector<value>& a) {
		switch (f) {
		case fn_location: return value::of_loc(need_str(a[0]));
		case fn_objective_set: set_objective(need_str(a[0]), 0); return value();
		case fn_objective_complete: set_objective(need_str(a[0]), 1); return value();
		case fn_objective_fail: set_objective(need_str(a[0]), 2); return value();
		case fn_timer_start: {
			int64_t fr = need_int(a[1]);
			if (fr < 1) fault("timer frames must be 1 or more");
			rt.timers[need_str(a[0])] = fr;
			log("timer " + a[0].s + ": " + num(fr) + " frames");
			return value();
		}
		case fn_timer_stop: {
			auto i = rt.timers.find(need_str(a[0]));
			if (i != rt.timers.end()) { rt.timers.erase(i); log("timer " + a[0].s + ": stopped"); }
			return value();
		}
		case fn_timer_left: {
			auto i = rt.timers.find(need_str(a[0]));
			return value::of_int(i == rt.timers.end() ? 0 : i->second);
		}
		case fn_counter_get: {
			auto i = rt.counters.find(need_str(a[0]));
			return value::of_int(i == rt.counters.end() ? 0 : i->second);
		}
		case fn_counter_set: case fn_counter_add: {
			const a_string& id = need_str(a[0]);
			auto i = rt.counters.find(id);
			int64_t old = i == rt.counters.end() ? 0 : i->second;
			int64_t v = f == fn_counter_set ? need_int(a[1]) : (int64_t)((uint64_t)old + (uint64_t)need_int(a[1]));
			v = (int32_t)(uint32_t)(uint64_t)v;
			rt.counters[id] = v;
			if (v != old) {
				log("counter " + id + " = " + num(v));
				fire(ev_counter_changed, {value::of_str(id)});
			}
			return value();
		}
		case fn_switch_get: return value::of_bool(host.oes_switch((int)switch_index(a[0]) - 1));
		case fn_switch_set: {
			int64_t i = switch_index(a[0]);
			if (a[1].t != 5) fault("bad bytecode (bool)");
			bool on = a[1].i != 0;
			bool& sw = host.oes_switch((int)i - 1);
			if (sw != on) {
				sw = on;
				rt.switches_seen[(size_t)i - 1] = on;
				log("switch " + num(i) + " " + (on ? "set" : "cleared"));
				fire(ev_switch_changed, {value::of_int(i)});
			}
			return value();
		}
		case fn_units_spawn: {
			int p = player(a[0]);
			int64_t n = need_int(a[2]);
			if (n < 0 || n > 1700) fault("count " + num(n) + " is not 0-1700");
			int t = unit_type(a[1]);
			int at = location(a[3]);
			host.oes_spawn(p, t, (int)n, at);
			log("spawn " + num(n) + " " + a[1].text() + " for " + who(p) + " at " + a[3].s);
			return value();
		}
		case fn_units_count: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = a[2].t == 0 ? -1 : location(a[2]);
			return value::of_int(host.oes_count(p, t, at));
		}
		case fn_units_kill: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = location(a[3]);
			int64_t want = need_int(a[2]);
			int n = want > 0 ? host.oes_kill(p, t, (int)std::min<int64_t>(want, 1700), at) : 0;
			if (n > 0) log("kill " + num(n) + " " + a[1].text() + " of " + who(p) + " at " + a[3].s);
			return value();
		}
		case fn_units_give: {
			int from = player(a[0]), to = player(a[1]);
			int t = unit_type(a[2]);
			int at = location(a[4]);
			int64_t want = need_int(a[3]);
			int n = want > 0 ? host.oes_give(from, to, t, (int)std::min<int64_t>(want, 1700), at) : 0;
			if (n > 0) log("give " + num(n) + " " + a[2].text() + " from " + who(from) + " to " + who(to) + " at " + a[4].s);
			return value();
		}
		case fn_resources_add: {
			int p = player(a[0]);
			int64_t m = host.oes_add_minerals(p, need_int(a[1]));
			int64_t g = a[2].t == 0 ? host.oes_gas(p) : host.oes_add_gas(p, need_int(a[2]));
			log(who(p) + " minerals " + num(m) + ", gas " + num(g));
			return value();
		}
		case fn_resources_minerals: return value::of_int(host.oes_minerals(player(a[0])));
		case fn_ai_attack: {
			int p = player(a[0]);
			int at = location(a[1]);
			host.oes_attack(p, at);
			log(who(p) + " attacks " + a[1].s);
			return value();
		}
		case fn_mission_victory: case fn_mission_defeat: {
			int p = player(a[0]);
			bool v = f == fn_mission_victory;
			rt.ended = true;
			rt.outcome_player = p;
			rt.outcome_victory = v ? 1 : 0;
			log(a_string(v ? "Victory" : "Defeat") + " for " + who(p));
			host.oes_outcome(p, v);
			return value();
		}
		case fn_text_show: {
			int p = player(a[0]);
			host.oes_text(p, need_str(a[1]));
			log("text to " + who(p) + ": " + a[1].s);
			return value();
		}
		case fn_sound_play: {
			int p = player(a[0]);
			host.oes_sound(p, need_str(a[1]));
			log("sound to " + who(p) + ": " + a[1].s);
			return value();
		}
		case fn_camera_center: {
			int p = player(a[0]);
			int at = location(a[1]);
			host.oes_camera(p, at);
			log("camera of " + who(p) + " to " + a[1].s);
			return value();
		}
		case fn_random: {
			int64_t m = need_int(a[0]);
			if (m < 1) fault("random max must be 1 or more");
			return value::of_int((int64_t)host.oes_random() % m);
		}
		case fn_game_frame: return value::of_int(rt.frame);
		case fn_units_remove: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = location(a[3]);
			int64_t want = count_arg(a[2]);
			size_t before = rt.kills.size();
			int n = want > 0 ? host.oes_remove(p, t, (int)want, at) : 0;
			rt.kills.resize(before); // removed, not killed: no unit.killed
			if (n > 0) log("remove " + num(n) + " " + a[1].text() + " of " + who(p) + " at " + a[3].s);
			return value::of_int(n);
		}
		case fn_units_order: {
			static const char* const names[3] = {"move", "attack", "patrol"};
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = location(a[2]);
			int64_t o = need_int(a[3]);
			if (o < 0 || o > 2) fault("order " + num(o) + " is not 0-2");
			int to = location(a[4]);
			host.oes_order(p, t, at, (int)o, to);
			log(a_string(names[o]) + " " + a[1].text() + " of " + who(p) + " at " + a[2].s + " to " + a[4].s);
			return value();
		}
		case fn_units_set_hp: case fn_units_set_shields: case fn_units_set_energy: {
			static const char* const names[3] = {"hit points", "shields", "energy"};
			int stat = f - fn_units_set_hp;
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = location(a[3]);
			int pc = percent(a[4]);
			int64_t want = count_arg(a[2]);
			if (want > 0) {
				host.oes_set_stat(stat, p, t, (int)want, at, pc);
				log(a_string(names[stat]) + " " + num(pc) + "% for " + num(want) + " " + a[1].text() + " of " + who(p) + " at " + a[3].s);
			}
			return value();
		}
		case fn_units_set_invincible: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int at = location(a[2]);
			if (a[3].t != 5) fault("bad bytecode (bool)");
			bool on = a[3].i != 0;
			host.oes_invincible(p, t, at, on);
			log(a[1].text() + " of " + who(p) + " at " + a[2].s + (on ? " invincible" : " vulnerable"));
			return value();
		}
		case fn_resources_gas: return value::of_int(host.oes_gas(player(a[0])));
		case fn_resources_set: {
			int p = player(a[0]);
			int64_t m = amount(a[1], "minerals");
			int64_t g = a[2].t == 0 ? -1 : amount(a[2], "gas");
			host.oes_set_resources(p, m, g);
			log(who(p) + " minerals " + num(host.oes_minerals(p)) + ", gas " + num(host.oes_gas(p)));
			return value();
		}
		case fn_deaths_get: {
			int p = player(a[0]);
			return value::of_int(host.oes_deaths(p, unit_type(a[1])));
		}
		case fn_deaths_set: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int64_t v = amount(a[2], "deaths");
			host.oes_set_deaths(p, t, v);
			log("deaths of " + a[1].text() + " for " + who(p) + " = " + num(v));
			return value();
		}
		case fn_score_get: {
			int p = player(a[0]);
			int64_t k = need_int(a[1]);
			if (k < 0 || k > 7) fault("score kind " + num(k) + " is not 0-7");
			return value::of_int(host.oes_score(p, (int)k));
		}
		case fn_score_set: {
			int p = player(a[0]);
			int64_t v = need_int(a[1]);
			host.oes_set_score(p, v);
			log(who(p) + " custom score = " + num(v));
			return value();
		}
		case fn_player_allied: {
			int x = player(a[0]), y = player(a[1]);
			return value::of_bool(x == y || host.oes_allied(x, y));
		}
		case fn_player_alliance: {
			static const char* const names[3] = {"enemy", "ally", "allied victory"};
			int x = player(a[0]), y = player(a[1]);
			int64_t v = need_int(a[2]);
			if (v < 0 || v > 2) fault("alliance state " + num(v) + " is not 0-2");
			if (x != y) {
				host.oes_alliance(x, y, (int)v);
				log(who(x) + " sets " + who(y) + " to " + names[v]);
			}
			return value();
		}
		case fn_mission_draw:
			rt.ended = true;
			rt.outcome_player = -1;
			rt.outcome_victory = 5;
			log("Draw");
			host.oes_draw();
			return value();
		case fn_location_move: {
			int which = location(a[0]);
			int to = location(a[1]);
			host.oes_move_location(which, to);
			log("location " + a[0].s + " moved to " + a[1].s);
			return value();
		}
		case fn_minimap_ping: {
			int p = player(a[0]);
			int at = location(a[1]);
			host.oes_ping(p, at);
			log("ping for " + who(p) + " at " + a[1].s);
			return value();
		}
		case fn_timer_show: {
			int64_t fr = need_int(a[0]);
			if (fr < 0 || fr > 30000000) fault("countdown frames " + num(fr) + " is not 0-30000000");
			host.oes_countdown((int)fr);
			log("countdown " + num(fr) + " frames");
			return value();
		}
		case fn_objective_state: return value::of_int(rt.objective(need_str(a[0])));
		case fn_ai_run: {
			int p = player(a[0]);
			const a_string& id = need_str(a[1]);
			if (id.size() != 4) fault("AI script \"" + id + "\" is not 4 characters");
			uint32_t script = 0;
			for (int i = 0; i != 4; ++i) script |= (uint32_t)(uint8_t)id[i] << (8 * i);
			int at = a[2].t == 0 ? -1 : location(a[2]);
			host.oes_ai_run(p, script, at);
			log(who(p) + " runs AI script " + id + (at < 0 ? a_string() : " at " + a[2].s));
			return value();
		}
		case fn_hero_add_xp: {
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int64_t n = amount(a[2], "amount");
			for (int u : heroes(p, t, flag(a[3]))) {
				int from = host.oes_hero_level(u);
				host.oes_hero_add_xp(u, (int)n);
				log(hero_text(p, a[1]) + " +" + num(n) + " xp (level " + num(host.oes_hero_level(u)) + ", xp " + num(host.oes_hero_xp(u)) + ")");
				log_levels(p, a[1], from, host.oes_hero_level(u));
			}
			return value();
		}
		case fn_hero_set_level: {
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int64_t lv = need_int(a[2]);
			int max = host.oes_hero_max_level(t);
			if (lv < 1 || lv > max) fault("level " + num(lv) + " is not 1-" + num(max));
			for (int u : heroes(p, t, flag(a[3]))) {
				int from = host.oes_hero_set_level(u, (int)lv);
				log(hero_text(p, a[1]) + " level " + num(from) + " -> " + num(lv));
				log_levels(p, a[1], from, (int)lv);
			}
			return value();
		}
		case fn_hero_level: case fn_hero_xp: case fn_hero_skill_points: {
			int p = player(a[0]);
			auto u = heroes(p, hero_type(a[1]), false);
			if (u.empty()) return value::of_int(0);
			return value::of_int(f == fn_hero_level ? host.oes_hero_level(u[0]) : f == fn_hero_xp ? host.oes_hero_xp(u[0]) : host.oes_hero_points(u[0]));
		}
		case fn_hero_add_skill_points: {
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int64_t n = ranged(a[2], "amount", -1000000, 1000000);
			for (int u : heroes(p, t, flag(a[3]))) log(hero_text(p, a[1]) + " skill points " + num(host.oes_hero_add_points(u, (int)n)));
			return value();
		}
		case fn_hero_learn: case fn_hero_unlearn: {
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int ab = ability(a[2]);
			bool learn = f == fn_hero_learn;
			bool free = learn && flag(a[3]);
			bool any = false;
			for (auto& h : holders(p, t, ab, true, flag(a[learn ? 4 : 3]))) {
				int r = learn ? host.oes_hero_learn(h.first, h.second, free) : host.oes_hero_unlearn(h.first, h.second);
				if (r < 0) continue;
				any = true;
				log(hero_text(p, a[1]) + (learn ? " learned " : " unlearned ") + a[2].s + " rank " + num(r));
			}
			return value::of_bool(any);
		}
		case fn_hero_attribute: {
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int k = (int)ranged(a[2], "attribute", 0, 2);
			auto u = heroes(p, t, false);
			return value::of_int(u.empty() ? 0 : host.oes_hero_attr(u[0], k));
		}
		case fn_hero_set_attribute: {
			static const char* const names[3] = {"strength", "agility", "intelligence"};
			int p = player(a[0]);
			int t = hero_type(a[1]);
			int k = (int)ranged(a[2], "attribute", 0, 2);
			int v = (int)ranged(a[3], "value", 0, 100000);
			for (int u : heroes(p, t, flag(a[4]))) {
				host.oes_hero_set_attr(u, k, v);
				log(hero_text(p, a[1]) + " " + names[k] + " = " + num(v));
			}
			return value();
		}
		case fn_ability_rank: case fn_ability_cooldown: case fn_ability_ready: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int ab = ability(a[2]);
			auto h = holders(p, t, ab, false, false);
			if (f == fn_ability_ready) return value::of_bool(!h.empty() && host.oes_ability_ready(h[0].first, h[0].second));
			if (h.empty()) return value::of_int(-1);
			return value::of_int(f == fn_ability_rank ? host.oes_ability_rank(h[0].first, h[0].second) : host.oes_ability_cooldown(h[0].first, h[0].second));
		}
		case fn_ability_set_cooldown: {
			int p = player(a[0]);
			int t = unit_type(a[1]);
			int ab = ability(a[2]);
			int64_t fr = ranged(a[3], "frames", 0, 30000000);
			for (auto& h : holders(p, t, ab, false, flag(a[4]))) {
				host.oes_ability_set_cooldown(h.first, h.second, (int)fr);
				log(a[2].s + " of " + type_text(a[1]) + " of " + who(p) + " cooldown " + num(fr) + " frames");
			}
			return value();
		}
		case fn_ability_cast: case fn_ability_cast_at: case fn_ability_cast_on: return cast(f, a);
		case fn_signal_send: {
			const a_string& name = need_str(a[0]);
			if (!event_name_ok(name)) fault("event name \"" + name + "\" is not a-z, 0-9, _ and .");
			event_ctx c;
			c.owner = a[1].t == 0 ? -1 : player(a[1]);
			if ((size_t)raised_frame + rt.raised.size() >= (size_t)max_raised) fault("too many custom events raised: stopped");
			log("signal " + name + " sent");
			if (prog.wants_custom) rt.raised.push_back({name, c});
			return value();
		}
		case fn_context_owner: return value::of_int(ctx.owner);
		case fn_context_type: return ctx.type < 0 ? value() : value::of_unit(ctx.type);
		case fn_context_x: return value::of_int(ctx.x);
		case fn_context_y: return value::of_int(ctx.y);
		case fn_context_value: return value::of_int(ctx.value);
		case fn_location_set_center: {
			int which = location(a[0]);
			int64_t x = need_int(a[1]), y = need_int(a[2]);
			host.oes_set_center(which, (int)std::max<int64_t>(-1000000, std::min<int64_t>(1000000, x)), (int)std::max<int64_t>(-1000000, std::min<int64_t>(1000000, y)));
			log("location " + a[0].s + " set to (" + num(x) + ", " + num(y) + ")");
			return value();
		}
		default: fault("no such function " + num(f));
		}
	}

	// The first script frame: game.started, with the frame counter at 0.
	void start() {
		rt.started = true;
		rt.v3 = prog.api >= 3;
		rt.frame = 0;
		for (size_t i = 0; i != 256; ++i) rt.switches_seen[i] = host.oes_switch((int)i);
		rt.kills.clear();
		rt.entered.assign(prog.entered_locations.size(), {});
		for (size_t k = 0; k != prog.entered_locations.size(); ++k) {
			int at = host.oes_location(prog.handlers_loc(prog.entered_locations[k]));
			if (at < 0) continue;
			a_vector<entered_unit> now;
			host.oes_units_in(at, now);
			for (auto& u : now) rt.entered[k].push_back(u.id);
		}
		baseline();
		fire(ev_game_started, {});
		run();
		drain_kills();
	}

	void baseline() {
		rt.ext = prog.wants_production || prog.wants_players;
		rt.incomplete.clear();
		rt.born.clear();
		rt.humans = 0;
		if (!rt.ext) return;
		host.oes_incomplete(rt.incomplete);
		rt.humans = host.oes_humans();
	}

	// Every later frame, in a fixed order: timer.expired (timer name order), frame.every (file order),
	// switch.changed from classic triggers (switch order), unit.killed (death order), unit.entered then
	// unit.left (location in file order, then unit id), unit.trained / building.finished (unit id),
	// player.left (player order); then the queue runs, then deaths the script caused.
	void step() {
		if (rt.ended) { rt.kills.clear(); rt.raised.clear(); return; }
		++rt.frame;
		// raised after the last step (a player's cast between frames): first
		fire_raised(std::min(rt.raised_early, rt.raised.size()));
		a_vector<a_string> expired;
		for (auto i = rt.timers.begin(); i != rt.timers.end();) {
			if (--i->second <= 0) { expired.push_back(i->first); i = rt.timers.erase(i); }
			else ++i;
		}
		for (auto& id : expired) { log("timer " + id + " expired"); fire(ev_timer_expired, {value::of_str(id)}); }
		for (size_t i = 0; i != prog.handlers.size(); ++i) {
			auto& h = prog.handlers[i];
			if (h.event == ev_frame_every && h.filter[0].t == 1 && h.filter[0].i > 0 && rt.frame % h.filter[0].i == 0) enqueue(i, {h.filter[0]});
		}
		for (size_t i = 0; i != 256; ++i) {
			bool on = host.oes_switch((int)i);
			if (on != rt.switches_seen[i]) {
				rt.switches_seen[i] = on;
				fire(ev_switch_changed, {value::of_int((int64_t)i + 1)});
			}
		}
		fire_kills();
		for (size_t k = 0; k != prog.entered_locations.size() && k < rt.entered.size(); ++k) {
			const a_string& name = prog.handlers_loc(prog.entered_locations[k]);
			int at = host.oes_location(name);
			if (at < 0) continue;
			a_vector<entered_unit> now;
			host.oes_units_in(at, now);
			auto& before = rt.entered[k];
			a_vector<uint16_t> ids;
			for (auto& u : now) {
				ids.push_back(u.id);
				if (!std::binary_search(before.begin(), before.end(), u.id)) {
					fire(ev_unit_entered, {value::of_loc(name), value::of_int(u.owner), value::of_unit(u.type)}, unit_ctx(u.owner, u.type));
				}
			}
			if (prog.wants_left) {
				for (uint16_t id : before) {
					if (std::binary_search(ids.begin(), ids.end(), id)) continue;
					entered_unit u;
					if (host.oes_live_unit(id, u)) fire(ev_unit_left, {value::of_loc(name), value::of_int(u.owner), value::of_unit(u.type)}, unit_ctx(u.owner, u.type));
				}
			}
			before = std::move(ids);
		}
		if (prog.wants_production || prog.wants_players) {
			if (!rt.ext) baseline();
			else production_and_players();
		}
		fire_raised(rt.raised.size());
		run();
		drain_kills();
	}

	void production_and_players() {
		a_vector<pending_unit> done;
		for (auto& v : rt.incomplete) done.push_back(v);
		for (uint16_t id : rt.born) done.push_back({id, -1});
		std::sort(done.begin(), done.end());
		for (auto& v : done) {
			entered_unit u;
			bool building = false;
			if (!host.oes_completed(v.id, v.type, u, building)) continue;
			fire(building ? ev_building_finished : ev_unit_trained, {value::of_int(u.owner), value::of_unit(u.type)}, unit_ctx(u.owner, u.type));
		}
		rt.born.clear();
		rt.incomplete.clear();
		host.oes_incomplete(rt.incomplete);
		uint32_t now = host.oes_humans();
		for (int p = 0; p != 8; ++p) {
			if ((rt.humans >> p & 1) && !(now >> p & 1)) fire(ev_player_left, {value::of_int(p)}, unit_ctx(p, -1));
		}
		rt.humans = now;
	}

	static event_ctx unit_ctx(int owner, int type) {
		event_ctx c;
		c.owner = owner;
		c.type = type;
		return c;
	}
	void fire_kills() {
		a_vector<kill_event> k;
		k.swap(rt.kills);
		for (auto& e : k) fire(ev_unit_killed, {value::of_int(e.owner), value::of_unit(e.type)}, unit_ctx(e.owner, e.type));
	}
	// The first n raised signals / level-ups, in raise order.
	void fire_raised(size_t n) {
		if (!n) return;
		a_vector<raised_event> now(rt.raised.begin(), rt.raised.begin() + n);
		rt.raised.erase(rt.raised.begin(), rt.raised.begin() + n);
		for (auto& e : now) {
			if (raised_frame >= max_raised) {
				record_fault("too many custom events raised: stopped");
				rt.raised.clear();
				return;
			}
			++raised_frame;
			if (e.name.empty()) fire(ev_hero_leveled, {value::of_int(e.ctx.owner), value::of_unit(e.ctx.type)}, e.ctx);
			else fire(ev_signal_received, {value::of_str(e.name), value::of_int(e.ctx.owner)}, e.ctx);
		}
	}
	// What the handlers caused: raised signals first, then deaths, until none are left.
	void drain_kills() {
		while ((!rt.kills.empty() || !rt.raised.empty()) && !rt.ended && frame_steps <= frame_budget) {
			fire_raised(rt.raised.size());
			run();
			fire_kills();
			run();
		}
		rt.kills.clear();
		rt.raised.clear();
	}
};

// An OESC chunk (the .obc bytes) appended to a CHK, so a script travels with the map data
// that replays and save files already carry.
inline void add_chunk(a_vector<uint8_t>& chk, const uint8_t* obc, size_t size) {
	const char tag[4] = {'O', 'E', 'S', 'C'};
	chk.insert(chk.end(), tag, tag + 4);
	for (int i = 0; i != 4; ++i) chk.push_back((uint8_t)((uint32_t)size >> (8 * i)));
	chk.insert(chk.end(), obc, obc + size);
}

}
}

#endif
