#ifndef BWGAME_UI_UMS_SLOTS_H
#define BWGAME_UI_UMS_SLOTS_H

// Use Map Settings lobby: which map slots play and how (single device).
// Shared by the app (android_main.cpp) and tools/umsscan so the harness
// measures exactly what the app sets up.
//
// Rules (SC:R lobby, measured from UMS replay headers):
//  - the first occupied/open human slot is the local player;
//  - every other open human slot is a lobby choice: Computer (default) or Closed;
//  - a slot whose SIDE race is "User Selectable" (5) gets melee starting units
//    and a concrete race (the lobby pick, else random);
//  - map computer slots stay computer.

#include "bwgame.h"

#include <array>
#include <cstdint>

namespace bwgame {

struct ums_slot_plan {
	// per slot 0-7, for open human slots other than the local one:
	// 'c' computer (default), 'x' closed, 'h' another human (harness only)
	std::array<char, 8> kind{};
	// per slot 0-7: 0 zerg, 1 terran, 2 protoss, anything else = random
	std::array<int, 8> race{{-1, -1, -1, -1, -1, -1, -1, -1}};
	int local_race = -1;     // the local slot's race when race[] leaves it unset
	int local_slot = -1;     // the human's slot when the map has one there (.oecampaign "player"), else the first
	bool melee_units = true; // false + kind 'x' everywhere = the pre-1.0.27 app setup
};

struct ums_slot_result {
	int local = -1;
	int first_enemy = -1;
	static_vector<size_t, 12> active;
	std::array<bool, 8> lobby_computer{};   // a human slot the lobby made a computer
	std::array<bool, 8> random_race{};
};

inline void ums_slot_plan_parse_kinds(ums_slot_plan& plan, const a_string& s) {
	for (size_t i = 0; i != 8 && i < s.size(); ++i) {
		char c = s[i];
		plan.kind[i] = c == 'x' || c == 'h' ? c : 'c';
	}
}

inline void ums_slot_plan_parse_races(ums_slot_plan& plan, const a_string& csv) {
	size_t sp = 0, k = 0;
	while (sp <= csv.size() && k != 8) {
		size_t c = csv.find(',', sp);
		if (c == a_string::npos) c = csv.size();
		if (c > sp) plan.race[k] = atoi(csv.substr(sp, c - sp).c_str());
		++k;
		sp = c + 1;
	}
}

// Call from the map load setup callback (after OWNR/SIDE, before units).
// Random races advance st.lcg_rand_state; the caller seeds it first and records
// the state after this call as the replay seed.
inline ums_slot_result ums_apply_slots(state& st, game_load_functions::setup_info_t& setup_info, const ums_slot_plan& plan) {
	ums_slot_result r;
	if (plan.local_slot >= 0 && plan.local_slot < 8) {
		auto& v = st.players[plan.local_slot];
		if (v.controller == player_t::controller_occupied || v.controller == player_t::controller_open) {
			v.controller = player_t::controller_occupied;
			r.local = plan.local_slot;
			r.active.push_back((size_t)plan.local_slot);
		}
	}
	for (size_t i = 0; i != 8; ++i) {
		if (st.players[i].controller == player_t::controller_occupied && r.local < 0) {
			r.local = (int)i;
			r.active.push_back(i);
		}
	}
	for (size_t i = 0; i != 8; ++i) {
		auto& v = st.players[i];
		if (v.controller == player_t::controller_open) {
			if (r.local < 0) {
				r.local = (int)i;
				v.controller = player_t::controller_occupied;
				r.active.push_back(i);
			} else if (plan.kind[i] == 'x') {
				v.controller = player_t::controller_closed;
			} else if (plan.kind[i] == 'h') {
				v.controller = player_t::controller_occupied;
				r.active.push_back(i);
			} else {
				v.controller = player_t::controller_computer_game;
				r.lobby_computer[i] = true;
				if (r.first_enemy < 0) r.first_enemy = (int)i;
				r.active.push_back(i);
			}
		} else if (v.controller == player_t::controller_computer) {
			// providing a setup callback skips the engine's own flip to computer_game
			v.controller = player_t::controller_computer_game;
			if (r.first_enemy < 0) r.first_enemy = (int)i;
			r.active.push_back(i);
		}
	}
	if (!plan.melee_units) return r;
	for (size_t i : r.active) {
		auto& v = st.players[i];
		if ((int)v.race != 5) continue;
		setup_info.create_melee_units_for_player[i] = true;
		int pick = plan.race[i];
		if ((pick < 0 || pick > 2) && (int)i == r.local) pick = plan.local_race;
		if (pick < 0 || pick > 2) {
			st.lcg_rand_state = st.lcg_rand_state * 22695477 + 1;
			pick = (int)((st.lcg_rand_state >> 16) % 3);
			r.random_race[i] = true;
		}
		v.race = (race_t)pick;
	}
	return r;
}

// Run AI Script (15 / 16) owned by this player, other than the shared-vision ids.
inline bool ums_player_runs_ai_script(const state& st, int player) {
	if (player < 0 || player >= 8) return false;
	for (auto& rt : st.running_triggers[player]) {
		if (!rt.t) continue;
		for (auto& a : rt.t->actions) {
			if (a.type == 0) break;
			if ((a.type != 15 && a.type != 16) || (a.flags & 2)) continue;
			if ((((uint32_t)a.group2_n >> 8) & 0xffff) == 0x6956) continue; // "+Vi?" shared vision
			return true;
		}
	}
	return false;
}

inline uint32_t ums_melee_ai_script_id(race_t r) {
	char c = r == race_t::zerg ? 'Z' : r == race_t::protoss ? 'P' : 'T';
	return (uint32_t)c | ((uint32_t)'M' << 8) | ((uint32_t)'C' << 16) | ((uint32_t)'u' << 24);
}

// After the load: a lobby computer that got a melee base and has no AI script
// of its own from the map plays the original melee AI. Returns the script per slot (0 = none).
template<typename funcs_T>
std::array<uint32_t, 8> ums_start_lobby_ai(funcs_T& funcs, const state& st, const game_load_functions::setup_info_t& setup_info, const ums_slot_result& r) {
	std::array<uint32_t, 8> started{};
	for (size_t i = 0; i != 8; ++i) {
		if (!r.lobby_computer[i] || !setup_info.create_melee_units_for_player[i]) continue;
		if (ums_player_runs_ai_script(st, (int)i)) continue;
		uint32_t id = ums_melee_ai_script_id(st.players[i].race);
		if (funcs.ai_start_script((int)i, id)) started[i] = id;
	}
	return started;
}

}

#endif
