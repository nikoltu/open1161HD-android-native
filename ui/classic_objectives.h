#pragma once
// G12: objective state for classic (non-OEScript) missions. Ganymede's Campaign
// Studio lowers each objective to classic triggers tagged with a Comment action
// "[objective:<mission>:<id>]": a completion trigger sets a switch (Set Switch,
// set) and shows "Objective complete: <title>"; a protect objective's trigger
// shows "Objective failed: <title>" and runs Defeat; the "start" trigger's Set
// Mission Objectives text marks optional ones ("Optional: <title>").
// Read-only over game_state / state: nothing here can change the simulation.
#include "bwgame.h"

namespace classic_objectives {

struct objective {
	bwgame::a_string id;
	bwgame::a_string title;
	int sw = -1;          // completion switch, or -1 (protect)
	bool protect = false;
	bool optional = false;
};

enum state_t { active = 0, done = 1, failed = 2 };

inline bwgame::a_string map_string(const bwgame::game_state& g, int index) {
	if (index <= 0 || (size_t)(index - 1) >= g.map_strings.size()) return bwgame::a_string();
	return g.map_strings[(size_t)index - 1];
}

inline bool starts_with(const bwgame::a_string& s, const char* p) {
	size_t n = 0;
	while (p[n]) ++n;
	return s.size() >= n && s.compare(0, n, p) == 0;
}

// The tagged objectives of the map's triggers that run for `player` (trigger order).
inline bwgame::a_vector<objective> read(const bwgame::game_state& g, const bwgame::state& st, int player) {
	bwgame::a_vector<objective> out;
	bwgame::a_string start_text;
	for (const bwgame::trigger& t : g.triggers) {
		if (player < 0 || player >= 8) break;
		int force = st.players.at((size_t)player).force;
		bool runs = t.enabled[(size_t)player] || t.enabled[17] || (force >= 0 && force < 4 && t.enabled[(size_t)(18 + force)]);
		if (!runs) continue;
		bwgame::a_string tag, shown;
		int sw = -1;
		bool defeat = false;
		for (const auto& a : t.actions) {
			if (a.type == 0) break;
			if (a.type == 47) {
				bwgame::a_string c = map_string(g, a.string_index);
				if (starts_with(c, "[objective:")) tag = c;
			} else if (a.type == 13 && a.num_n == 4 && a.group2_n >= 0 && a.group2_n < 256) sw = a.group2_n;
			else if (a.type == 9) shown = map_string(g, a.string_index);
			else if (a.type == 2) defeat = true;
			else if (a.type == 12) start_text = map_string(g, a.string_index);
		}
		if (tag.empty()) continue;
		// "[objective:<mission>:<what>]"
		size_t c2 = tag.find(':', 11);
		if (c2 == bwgame::a_string::npos) continue;
		bwgame::a_string what = tag.substr(c2 + 1);
		if (!what.empty() && what.back() == ']') what.pop_back();
		if (what == "start" || what == "victory" || what.empty()) continue;
		objective o;
		o.id = what;
		if (starts_with(shown, "Objective complete: ")) o.title = shown.substr(20);
		else if (starts_with(shown, "Objective failed: ")) o.title = shown.substr(18);
		else continue; // hidden objective: no text, nothing to show
		o.protect = defeat && sw < 0;
		o.sw = o.protect ? -1 : sw;
		if (!o.protect && o.sw < 0) continue;
		out.push_back(o);
	}
	for (auto& o : out) {
		bwgame::a_string key = "Optional: " + o.title;
		size_t p = 0;
		while ((p = start_text.find(key, p)) != bwgame::a_string::npos) {
			if (p == 0 || start_text[p - 1] == '\n') { o.optional = true; break; }
			++p;
		}
	}
	return out;
}

inline state_t state_of(const objective& o, const bwgame::state& st, int player) {
	if (o.sw >= 0) return st.trigger_switches.at((size_t)o.sw) ? done : active;
	// A protect objective fails with the mission; a won mission kept it.
	int v = player >= 0 && player < 12 ? st.players.at((size_t)player).victory_state : 0;
	if (v >= 3) return done;
	if (v) return failed;
	return active;
}

} // namespace classic_objectives
