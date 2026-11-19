#ifndef BWGAME_EUD_H
#define BWGAME_EUD_H

#include <cstddef>
#include <cstdint>

// StarCraft 1.16.1 memory layout that EUD triggers address through Deaths / Set Deaths
// (address = 0x58A364 + 4 * (unit * 12 + player), wrapping). Only addresses with an
// engine equivalent are listed; anything else falls back to a plain per-map store.
namespace bwgame {
namespace eud {

static const uint32_t deaths_base = 0x0058A364;
static const uint32_t cunit_base = 0x0059CCA8;

// 1.16.1 requirement tables (units, upgrades, tech research, tech use, orders: opcode lists
// keyed by id, ended by 0xFFFF) and the u16 offset indexes the game builds from them on first use.
static const uint32_t req_base = 0x00514178, req_end = 0x0051521C;
struct req_table { uint32_t table, end_marker, index, count, first; };
static const req_table req_tables[5] = {
	{0x00514178, 0x005145B8, 0x00660A70, 228, 0},
	{0x005145C0, 0x00514904, 0x006558C0, 61, 228},
	{0x00514908, 0x00514A44, 0x00656198, 44, 289},
	{0x00514A48, 0x00514CF6, 0x006562F8, 44, 333},
	{0x00514CF8, 0x0051521A, 0x00665580, 189, 377},
};
static const uint32_t req_index_total = 566;
enum req_kind { req_unit, req_upgrade, req_research, req_use, req_order };

enum region_id : uint8_t {
	r_none,
	// [unit][player] dword tables
	r_deaths, r_kills, r_unit_counts, r_completed_counts,
	// [player] dword tables
	r_minerals, r_gas, r_total_gas, r_total_minerals, r_custom_score,
	r_unit_score_total, r_kill_score_total, r_building_score, r_razings_score,
	r_supply_avail_z, r_supply_used_z, r_supply_max_z,
	r_supply_avail_t, r_supply_used_t, r_supply_max_t,
	r_supply_avail_p, r_supply_used_p, r_supply_max_p,
	// [player][id] byte tables
	r_sc_tech_avail, r_sc_tech_researched, r_bw_tech_avail, r_bw_tech_researched,
	r_sc_upgrade_max, r_sc_upgrade_level, r_bw_upgrade_max, r_bw_upgrade_level,
	r_alliances,
	// singletons / small tables
	r_cp, r_countdown, r_elapsed_seconds, r_frame, r_switches, r_locations, r_objectives,
	// units.dat
	r_u_flingy, r_u_subunit1, r_u_subunit2, r_u_infestation, r_u_construction_image, r_u_direction,
	r_u_has_shields, r_u_shields, r_u_hp, r_u_elevation, r_u_movement_flags, r_u_rank,
	r_u_computer_idle, r_u_human_idle, r_u_return_idle, r_u_attack_unit, r_u_attack_move,
	r_u_ground_weapon, r_u_max_ground_hits, r_u_air_weapon, r_u_max_air_hits, r_u_ai_internal,
	r_u_flags, r_u_seek_range, r_u_sight_range, r_u_armor_upgrade, r_u_size, r_u_armor,
	r_u_right_click, r_u_ready_sound, r_u_what_start, r_u_what_end, r_u_pissed_start, r_u_pissed_end,
	r_u_yes_start, r_u_yes_end, r_u_placement, r_u_addon_pos, r_u_dimensions, r_u_portrait,
	r_u_mineral_cost, r_u_gas_cost, r_u_build_time, r_u_requirements, r_u_group_flags,
	r_u_supply_provided, r_u_supply_required, r_u_space_required, r_u_space_provided,
	r_u_build_score, r_u_kill_score, r_u_map_string, r_u_broodwar, r_u_staredit_flags,
	// weapons.dat
	r_w_label, r_w_flingy, r_w_special, r_w_target_flags, r_w_min_range, r_w_max_range,
	r_w_upgrade, r_w_damage_type, r_w_behavior, r_w_remove_after, r_w_effect, r_w_inner_splash,
	r_w_medium_splash, r_w_outer_splash, r_w_damage, r_w_bonus, r_w_cooldown, r_w_factor,
	r_w_attack_angle, r_w_launch_spin, r_w_x_offset, r_w_y_offset, r_w_error_message, r_w_icon,
	// flingy.dat
	r_f_sprite, r_f_top_speed, r_f_acceleration, r_f_halt_distance, r_f_turn_rate, r_f_unused,
	r_f_movement,
	// sprites.dat / images.dat
	r_s_image, r_s_hp_bar, r_s_unknown, r_s_visible, r_s_sel_circle, r_s_sel_vpos,
	r_i_grp, r_i_turns, r_i_clickable, r_i_full_iscript, r_i_draw_cloaked, r_i_draw_func,
	r_i_remap, r_i_iscript,
	// upgrades.dat / techdata.dat
	r_up_mineral_base, r_up_mineral_factor, r_up_gas_base, r_up_gas_factor, r_up_time_base,
	r_up_time_factor, r_up_max_level,
	r_t_mineral, r_t_gas, r_t_time, r_t_energy,
	// CUnit table
	r_cunit, r_first_empty_unit,
	// input state the sim does not have (reads 0, writes ignored)
	r_keys, r_input_ignored,
	// pointer to the map's STR section (emulated at str_base)
	r_str_ptr,
	// trigger loop timer (hyper triggers) and the per-player info table (type / race bytes)
	r_trigger_timer, r_player_struct,
	r_count
};

struct region {
	uint32_t base;
	uint32_t elem_size;  // bytes per element
	uint32_t count;      // elements
	uint32_t first_id;   // id of element 0 (units.dat partial fields start at 106)
	region_id id;
	bool dat;            // per-game type data: a write is journaled for save / load
};

// Sorted by base address.
static const region regions[] = {
	{0x0057EEE0, 1, 36 * 12, 0, r_player_struct, false},
	{0x0057F0F0, 4, 12, 0, r_minerals, false},
	{0x0057F120, 4, 12, 0, r_gas, false},
	{0x0057F150, 4, 12, 0, r_total_gas, false},
	{0x0057F180, 4, 12, 0, r_total_minerals, false},
	{0x0057F23C, 4, 1, 0, r_frame, false},
	{0x00581ED4, 4, 12, 0, r_unit_score_total, false},
	{0x00581F04, 4, 12, 0, r_kill_score_total, false},
	{0x00582024, 4, 12, 0, r_building_score, false},
	{0x00582054, 4, 12, 0, r_razings_score, false},
	{0x00582144, 4, 12, 0, r_supply_avail_z, false},
	{0x00582174, 4, 12, 0, r_supply_used_z, false},
	{0x005821A4, 4, 12, 0, r_supply_max_z, false},
	{0x005821D4, 4, 12, 0, r_supply_avail_t, false},
	{0x00582204, 4, 12, 0, r_supply_used_t, false},
	{0x00582234, 4, 12, 0, r_supply_max_t, false},
	{0x00582264, 4, 12, 0, r_supply_avail_p, false},
	{0x00582294, 4, 12, 0, r_supply_used_p, false},
	{0x005822C4, 4, 12, 0, r_supply_max_p, false},
	{0x005822F4, 4, 12, 0, r_custom_score, false},
	{0x00582324, 4, 228 * 12, 0, r_unit_counts, false},
	{0x00584DE4, 4, 228 * 12, 0, r_completed_counts, false},
	{0x005878A4, 4, 228 * 12, 0, r_kills, false},
	{0x0058A364, 4, 228 * 12, 0, r_deaths, false},
	{0x0058CE24, 1, 24 * 12, 0, r_sc_tech_avail, true},
	{0x0058CF44, 1, 24 * 12, 0, r_sc_tech_researched, false},
	{0x0058D088, 1, 46 * 12, 0, r_sc_upgrade_max, true},
	{0x0058D2B0, 1, 46 * 12, 0, r_sc_upgrade_level, false},
	{0x0058D634, 1, 12 * 12, 0, r_alliances, false},
	{0x0058D6C4, 4, 12, 0, r_objectives, false},
	{0x0058D6F4, 4, 1, 0, r_countdown, false},
	{0x0058D6F8, 4, 1, 0, r_elapsed_seconds, false},
	{0x0058DC40, 4, 8, 0, r_switches, false},
	{0x0058DC60, 4, 255 * 5, 0, r_locations, false},
	{0x0058F050, 1, 20 * 12, 0, r_bw_tech_avail, true},
	{0x0058F140, 1, 20 * 12, 0, r_bw_tech_researched, false},
	{0x0058F278, 1, 15 * 12, 0, r_bw_upgrade_max, true},
	{0x0058F32C, 1, 15 * 12, 0, r_bw_upgrade_level, false},
	{0x00596A18, 1, 256, 0, r_keys, false},
	{0x005993D4, 4, 1, 0, r_str_ptr, false},
	{0x0059CCA8, 4, 336 / 4 * 1700, 0, r_cunit, false},
	{0x00628438, 4, 1, 0, r_first_empty_unit, false},   // read-only: next CUnit a create will use
	{0x006509A0, 4, 1, 0, r_trigger_timer, false},
	{0x006509B0, 4, 1, 0, r_cp, false},
	{0x00655700, 1, 61, 0, r_up_max_level, true},
	{0x00655740, 2, 61, 0, r_up_mineral_base, true},
	{0x006557C0, 2, 61, 0, r_up_gas_factor, true},
	{0x00655840, 2, 61, 0, r_up_gas_base, true},
	{0x00655940, 2, 61, 0, r_up_time_factor, true},
	{0x006559C0, 2, 61, 0, r_up_mineral_factor, true},
	{0x00655B80, 2, 61, 0, r_up_time_base, true},
	{0x006561F0, 2, 44, 0, r_t_gas, true},
	{0x00656248, 2, 44, 0, r_t_mineral, true},
	{0x00656380, 2, 44, 0, r_t_energy, true},
	{0x006563D8, 2, 44, 0, r_t_time, true},
	{0x006564E0, 1, 130, 0, r_w_factor, true},
	{0x00656568, 2, 130, 0, r_w_error_message, true},
	{0x00656670, 1, 130, 0, r_w_behavior, true},
	{0x006566F8, 1, 130, 0, r_w_effect, true},
	{0x00656780, 2, 130, 0, r_w_icon, true},
	{0x00656888, 2, 130, 0, r_w_inner_splash, true},
	{0x00656990, 1, 130, 0, r_w_attack_angle, true},
	{0x00656A18, 4, 130, 0, r_w_min_range, true},
	{0x00656C20, 1, 130, 0, r_w_y_offset, true},
	{0x00656CA8, 4, 130, 0, r_w_flingy, true},
	{0x00656EB0, 2, 130, 0, r_w_damage, true},
	{0x00656FB8, 1, 130, 0, r_w_cooldown, true},
	{0x00657040, 1, 130, 0, r_w_remove_after, true},
	{0x006570C8, 2, 130, 0, r_w_medium_splash, true},
	{0x006571D0, 1, 130, 0, r_w_upgrade, true},
	{0x00657258, 1, 130, 0, r_w_damage_type, true},
	{0x006572E0, 2, 130, 0, r_w_label, true},
	{0x006573E8, 1, 130, 0, r_w_special, true},
	{0x00657470, 4, 130, 0, r_w_max_range, true},
	{0x00657678, 2, 130, 0, r_w_bonus, true},
	{0x00657780, 2, 130, 0, r_w_outer_splash, true},
	{0x00657888, 1, 130, 0, r_w_launch_spin, true},
	{0x00657910, 1, 130, 0, r_w_x_offset, true},
	{0x00657998, 2, 130, 0, r_w_target_flags, true},
	{0x0065FC18, 1, 228, 0, r_u_max_air_hits, true},
	{0x0065FD00, 2, 228, 0, r_u_gas_cost, true},
	{0x0065FEC8, 1, 228, 0, r_u_armor, true},
	{0x0065FFB0, 2, 228, 0, r_u_what_start, true},
	{0x00660178, 1, 228, 0, r_u_ai_internal, true},
	{0x00660260, 2, 228, 0, r_u_map_string, true},
	{0x00660428, 2, 228, 0, r_u_build_time, true},
	{0x006605F0, 1, 228, 0, r_u_direction, true},
	{0x006606D8, 1, 228, 0, r_u_broodwar, true},
	{0x006607C0, 2, 228, 0, r_u_subunit1, true},
	{0x00660988, 1, 228, 0, r_u_space_provided, true},
	{0x00660A70, 2, 228, 0, r_u_requirements, true},
	{0x00660C38, 2, 228, 0, r_u_subunit2, true},
	{0x00660E00, 2, 228, 0, r_u_shields, true},
	{0x00660FC8, 1, 228, 0, r_u_movement_flags, true},
	{0x006610B0, 4, 228, 0, r_u_construction_image, true},
	{0x00661440, 2, 106, 0, r_u_yes_end, true},
	{0x00661518, 2, 228, 0, r_u_staredit_flags, true},
	{0x006616E0, 1, 228, 0, r_u_air_weapon, true},
	{0x006617C8, 8, 228, 0, r_u_dimensions, true},
	{0x00661EE8, 2, 106, 0, r_u_pissed_end, true},
	{0x00661FC0, 2, 106, 0, r_u_ready_sound, true},
	{0x00662098, 1, 228, 0, r_u_right_click, true},
	{0x00662180, 1, 228, 0, r_u_size, true},
	{0x00662268, 1, 228, 0, r_u_human_idle, true},
	{0x00662350, 4, 228, 0, r_u_hp, true},
	{0x006626E0, 4, 96, 106, r_u_addon_pos, true},
	{0x00662860, 4, 228, 0, r_u_placement, true},
	{0x00662BF0, 2, 228, 0, r_u_what_end, true},
	{0x00662DB8, 1, 228, 0, r_u_seek_range, true},
	{0x00662EA0, 1, 228, 0, r_u_computer_idle, true},
	{0x00662F88, 2, 228, 0, r_u_portrait, true},
	{0x00663150, 1, 228, 0, r_u_elevation, true},
	{0x00663238, 1, 228, 0, r_u_sight_range, true},
	{0x00663320, 1, 228, 0, r_u_attack_unit, true},
	{0x00663408, 2, 228, 0, r_u_build_score, true},
	{0x006635D0, 1, 228, 0, r_u_armor_upgrade, true},
	{0x006636B8, 1, 228, 0, r_u_ground_weapon, true},
	{0x006637A0, 1, 228, 0, r_u_group_flags, true},
	{0x00663888, 2, 228, 0, r_u_mineral_cost, true},
	{0x00663A50, 1, 228, 0, r_u_attack_move, true},
	{0x00663B38, 2, 106, 0, r_u_pissed_start, true},
	{0x00663C10, 2, 106, 0, r_u_yes_start, true},
	{0x00663CE8, 1, 228, 0, r_u_supply_required, true},
	{0x00663DD0, 1, 228, 0, r_u_rank, true},
	{0x00663EB8, 2, 228, 0, r_u_kill_score, true},
	{0x00664080, 4, 228, 0, r_u_flags, true},
	{0x00664410, 1, 228, 0, r_u_space_required, true},
	{0x006644F8, 1, 228, 0, r_u_flingy, true},
	{0x006645E0, 1, 228, 0, r_u_max_ground_hits, true},
	{0x006646C8, 1, 228, 0, r_u_supply_provided, true},
	{0x006647B0, 1, 228, 0, r_u_has_shields, true},
	{0x00664898, 1, 228, 0, r_u_return_idle, true},
	{0x00664980, 2, 96, 106, r_u_infestation, true},
	{0x00665AC0, 1, 387, 130, r_s_sel_circle, true},
	{0x00665C48, 1, 517, 0, r_s_visible, true},
	{0x00665E50, 1, 387, 130, r_s_hp_bar, true},
	{0x00665FD8, 1, 387, 130, r_s_sel_vpos, true},
	{0x00666160, 2, 517, 0, r_s_image, true},
	{0x00666570, 1, 517, 0, r_s_unknown, true},
	{0x00667718, 1, 999, 0, r_i_draw_cloaked, true},
	{0x00668AA0, 4, 999, 0, r_i_grp, true},   // read-only: the exe reads it only while loading
	{0x00669A40, 1, 999, 0, r_i_remap, true},   // the exe's .dat load table: remapping here, drawing function at 0x669E28
	{0x00669E28, 1, 999, 0, r_i_draw_func, true},
	{0x0066C150, 1, 999, 0, r_i_clickable, true},
	{0x0066D4D8, 1, 999, 0, r_i_full_iscript, true},
	{0x0066E860, 1, 999, 0, r_i_turns, true},
	{0x0066EC48, 4, 999, 0, r_i_iscript, true},
	{0x006C9858, 1, 209, 0, r_f_movement, true},
	{0x006C9930, 4, 209, 0, r_f_halt_distance, true},
	{0x006C9C78, 2, 209, 0, r_f_acceleration, true},
	{0x006C9E20, 1, 209, 0, r_f_turn_rate, true},
	{0x006C9EF8, 4, 209, 0, r_f_top_speed, true},
	{0x006CA240, 1, 209, 0, r_f_unused, true},
	{0x006CA318, 2, 209, 0, r_f_sprite, true},
	{0x006CDDC4, 4, 2, 0, r_input_ignored, false},   // mouse x/y (UI only; screen pos 0x62848C/0x6284A8 stays plain memory)
	{0x006D0FD1, 1, 36 * 12, 0, r_player_struct, false},   // replay header copy
};

// Where the emulated STR section lives (1.16.1 has it on the heap; maps read 0x5993D4).
const uint32_t str_base = 0x10000000;

// Button arrays (20 bytes each: pos, icon, ...) and the button-set table (250 x {count,
// array, ?}) of the 1.16.1 exe; maps that rewrite them change the command card (UI only).
const uint32_t buttons_base = 0x00515BE8;
const uint32_t button_sets = 0x005187E8;
const uint32_t buttons_end = button_sets + 12 * 250;

inline const region* find_region(uint32_t addr) {
	size_t lo = 0, hi = sizeof(regions) / sizeof(regions[0]);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (regions[mid].base <= addr) lo = mid + 1;
		else hi = mid;
	}
	if (lo == 0) return nullptr;
	const region& r = regions[lo - 1];
	if (addr - r.base < r.elem_size * r.count) return &r;
	return nullptr;
}

// Optional access observer for harnesses (tools/umsscan); null in the game.
typedef void (*access_hook_fn)(uint32_t addr, bool write, const region* r, uint32_t value);
inline access_hook_fn& access_hook() {
	static access_hook_fn h = nullptr;
	return h;
}

} // namespace eud
} // namespace bwgame

#endif
