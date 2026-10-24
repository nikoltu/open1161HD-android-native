// Bot host: the C-ABI function table (bot_api.h) implemented over the engine's
// action functions. Shared VERBATIM by the app (android_main.cpp) and the headless
// harness (tools/botmatch.cpp), so a bot measured on the host runs against exactly
// the host code that ships. Not a standalone header -- the includer provides, before
// including it (inside the namespace/using context it wants):
//   BOT_HOST_T          host type with members: ui (anything derived from
//                       bwgame::action_functions), int bot_player, int bot_level,
//                       std::function<void(const uint8_t*, size_t)> bot_action_record
//                       (may be empty; see "replay recording" below)
//   BOT_HOST_LOG(...)   printf-style log sink
//   static void diag_bot_cmd(BOT_HOST_T&);
//   static void diag_bot_invalid(BOT_HOST_T&, const char* api, const char* reason,
//                                const std::string& extra = "");
//   static void diag_bot_stale(BOT_HOST_T&, unit_t*, const char* api);
// and declares  static BOT_HOST_T* bot_m(ob_bot_host* h);  (defined here).
// The handle a bot gets (ob_bot_host*) is the BOT_HOST_T*.

static BOT_HOST_T* bot_m(ob_bot_host* h) { return (BOT_HOST_T*)h; }

// --- replay recording (v1.0.2) ------------------------------------------------
// A bot issues its commands by calling the engine's action_* functions directly,
// so nothing of what it did reached the replay: a game against the built-in AI
// could not be played back at all. Every command below is now ALSO written out in
// the classic 1.16.1 action encoding (the bytes actions.h read_action_* parses back
// into the very same action_* call) and handed to the host's recorder. The direct
// call stays as it was -- bot behaviour is unchanged; playback re-issues the bytes
// for the bot's slot like any human player's.
static void bot_rec(BOT_HOST_T& m, const uint8_t* d, size_t n) {
	if (m.bot_action_record) m.bot_action_record(d, n);
}
static void bot_rec_select(BOT_HOST_T& m, unit_t* u) {
	if (!m.bot_action_record) return;
	const uint16_t id = (uint16_t)m.ui.get_unit_id(u).raw_value;
	const uint8_t b[4] = {9, 1, (uint8_t)(id & 0xff), (uint8_t)(id >> 8)};
	bot_rec(m, b, sizeof b);
}
static void bot_rec_u16(BOT_HOST_T& m, uint8_t op, int v) {   // train 31, morph 35, morph building 53
	const uint8_t b[3] = {op, (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
	bot_rec(m, b, sizeof b);
}
static void bot_rec_u8(BOT_HOST_T& m, uint8_t op, int v) {    // stop 26, research 48, upgrade 50, siege 38, ...
	const uint8_t b[2] = {op, (uint8_t)v};
	bot_rec(m, b, sizeof b);
}
static void bot_rec_op(BOT_HOST_T& m, uint8_t op) {           // stim 54, train fighter 39
	bot_rec(m, &op, 1);
}
static void bot_rec_order(BOT_HOST_T& m, int x, int y, unit_t* target, int order_id) {   // 21
	if (!m.bot_action_record) return;
	const uint16_t tid = target ? (uint16_t)m.ui.get_unit_id(target).raw_value : 0;
	const uint16_t none = (uint16_t)UnitTypes::None;
	const uint8_t b[11] = {21, (uint8_t)(x & 0xff), (uint8_t)((x >> 8) & 0xff), (uint8_t)(y & 0xff), (uint8_t)((y >> 8) & 0xff),
		(uint8_t)(tid & 0xff), (uint8_t)(tid >> 8), (uint8_t)(none & 0xff), (uint8_t)(none >> 8), (uint8_t)order_id, 0};
	bot_rec(m, b, sizeof b);
}
static void bot_rec_default_order(BOT_HOST_T& m, int x, int y, unit_t* target) {   // 20
	if (!m.bot_action_record) return;
	const uint16_t tid = target ? (uint16_t)m.ui.get_unit_id(target).raw_value : 0;
	const uint16_t none = (uint16_t)UnitTypes::None;
	const uint8_t b[10] = {20, (uint8_t)(x & 0xff), (uint8_t)((x >> 8) & 0xff), (uint8_t)(y & 0xff), (uint8_t)((y >> 8) & 0xff),
		(uint8_t)(tid & 0xff), (uint8_t)(tid >> 8), (uint8_t)(none & 0xff), (uint8_t)(none >> 8), 0};
	bot_rec(m, b, sizeof b);
}
static void bot_rec_build(BOT_HOST_T& m, int order_id, int tile_x, int tile_y, int unit_type) {   // 12
	const uint8_t b[8] = {12, (uint8_t)order_id, (uint8_t)(tile_x & 0xff), (uint8_t)((tile_x >> 8) & 0xff),
		(uint8_t)(tile_y & 0xff), (uint8_t)((tile_y >> 8) & 0xff), (uint8_t)(unit_type & 0xff), (uint8_t)((unit_type >> 8) & 0xff)};
	bot_rec(m, b, sizeof b);
}

static int32_t bot_api_current_frame(ob_bot_host* h) {
	return (int32_t)bot_m(h)->ui.st.current_frame;
}
static int32_t bot_api_my_player(ob_bot_host* h) {
	return bot_m(h)->bot_player;
}
static int32_t bot_api_minerals(ob_bot_host* h, int32_t p) {
	if (p < 0 || p >= 12) return 0;
	return bot_m(h)->ui.st.current_minerals[p];
}
static int32_t bot_api_gas(ob_bot_host* h, int32_t p) {
	if (p < 0 || p >= 12) return 0;
	return bot_m(h)->ui.st.current_gas[p];
}
static int32_t bot_api_supply_used(ob_bot_host* h, int32_t p) {
	if (p < 0 || p >= 12) return 0;
	auto& m = *bot_m(h);
	race_t r = m.ui.st.players[p].race;
	return (int32_t)m.ui.st.supply_used[p][(size_t)r].raw_value; // fp1 raw = half-supply units
}
static int32_t bot_api_supply_available(ob_bot_host* h, int32_t p) {
	if (p < 0 || p >= 12) return 0;
	auto& m = *bot_m(h);
	race_t r = m.ui.st.players[p].race;
	return (int32_t)m.ui.st.supply_available[p][(size_t)r].raw_value; // fp1 raw = half-supply units
}
static int32_t bot_api_units(ob_bot_host* h, int32_t p, ob_unit* out, int32_t max) {
	if (p < 0 || p >= 12 || max <= 0) return 0;
	auto& m = *bot_m(h);
	int32_t n = 0;
	for (auto i = m.ui.st.player_units[p].begin(); i != m.ui.st.player_units[p].end() && n != max; ++i) {
		unit_t* u = &*i;
		if (!u->sprite) continue;
		ob_unit& o = out[n++];
		o.id = (intptr_t)u;
		o.type = (int32_t)u->unit_type->id;
		o.owner = u->owner;
		o.x = u->sprite->position.x;
		o.y = u->sprite->position.y;
		o.hp = (int32_t)u->hp.integer_part();
		o.shields = (int32_t)u->shield_points.integer_part();
		o.order = (int32_t)u->order_type->id;
		auto oid = u->order_type->id;
		// PickupIdle is the idle order of transports (overlord / shuttle / dropship):
		// without it an overlord never reads as idle and a bot can't tell a parked one
		// from one on its way (measured: all overlords "order 93", none idle).
		o.idle = (oid == Orders::Guard || oid == Orders::PlayerGuard || oid == Orders::Nothing || oid == Orders::PickupIdle) ? 1 : 0;
		o.completed = m.ui.u_completed(u) ? 1 : 0;
	}
	return n;
}
static int32_t bot_api_enemy_start(ob_bot_host* h, int32_t* x, int32_t* y) {
	auto& m = *bot_m(h);
	for (int p = 0; p != 8; ++p) {
		if (p == m.bot_player) continue;
		if (!m.ui.player_slot_active(p)) continue;
		if (m.ui.st.alliances[m.bot_player][p] != 0) continue;
		xy pos = m.ui.game_st.start_locations[p];
		if (pos == xy()) continue;
		*x = pos.x;
		*y = pos.y;
		return 1;
	}
	return 0;
}
static int32_t bot_api_map_size(ob_bot_host* h, int32_t* w, int32_t* hh) {
	auto& m = *bot_m(h);
	*w = (int32_t)m.ui.game_st.map_width;
	*hh = (int32_t)m.ui.game_st.map_height;
	return 1;
}
static void bot_api_order_move(ob_bot_host* h, intptr_t unit, int32_t x, int32_t y) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "move", u ? "wrong_owner" : "null_unit"); return; }
	diag_bot_stale(m, u, "move");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	m.ui.action_order(m.bot_player, m.ui.get_order_type(Orders::Move), xy(x, y), nullptr, nullptr, false);
	bot_rec_select(m, u); bot_rec_order(m, x, y, nullptr, (int)Orders::Move);
}
static void bot_api_order_attack_move(ob_bot_host* h, intptr_t unit, int32_t x, int32_t y) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "attack_move", u ? "wrong_owner" : "null_unit"); return; }
	diag_bot_stale(m, u, "attack_move");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	m.ui.action_order(m.bot_player, m.ui.get_order_type(Orders::AttackMove), xy(x, y), nullptr, nullptr, false);
	bot_rec_select(m, u); bot_rec_order(m, x, y, nullptr, (int)Orders::AttackMove);
}
static void bot_api_order_right_click(ob_bot_host* h, intptr_t unit, intptr_t target) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	unit_t* t = (unit_t*)target;
	if (!u || !t || u->owner != m.bot_player) { diag_bot_invalid(m, "right_click", !u ? "null_unit" : (!t ? "null_target" : "wrong_owner")); return; }
	diag_bot_stale(m, u, "right_click"); diag_bot_stale(m, t, "right_click_target");
	diag_bot_cmd(m);
	bot_rec_select(m, u); bot_rec_default_order(m, t->sprite->position.x, t->sprite->position.y, t);
	m.ui.action_select(m.bot_player, u);
	m.ui.action_default_order(m.bot_player, t->sprite->position, t, nullptr, false);
}
static void bot_api_order_stop(ob_bot_host* h, intptr_t unit) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "stop", u ? "wrong_owner" : "null_unit"); return; }
	diag_bot_stale(m, u, "stop");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	m.ui.action_stop(m.bot_player, false);
	bot_rec_select(m, u); bot_rec_u8(m, 26, 0);
}
static int32_t bot_api_train(ob_bot_host* h, intptr_t trainer, int32_t unit_type) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)trainer;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "train", u ? "wrong_owner" : "null_unit"); return 0; }
	if ((size_t)unit_type >= 228) { diag_bot_invalid(m, "train", "bad_unit_type", "\"type\":" + std::to_string(unit_type)); return 0; }
	diag_bot_stale(m, u, "train");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u);
	const unit_type_t* ut = m.ui.get_unit_type((UnitTypes)unit_type);
	// Zerg produces by MORPHING, never by the Train command (which is Terran/
	// Protoss only). Route ANY Zerg producer through action_morph: larva ->
	// drone/overlord/zergling/hydralisk/mutalisk/..., hatchery -> lair, lair ->
	// hive, spire -> greater spire, creep colony -> sunken/spore, hydralisk ->
	// lurker, mutalisk -> guardian/devourer. (Real BWAPI bots like Steamhammer
	// call Unit::morph() for all of these; the shim's morph() maps to train().)
	// Previously only Zerg_Larva morphed, so a real Zerg bot could not tech or
	// make defensive/aerial morphs -- it built drones and nothing else.
	// EXCEPTION: the Infested Terran is TRAINED from an Infested Command Center,
	// not morphed (BWAPI/Steamhammer route it through train(), not morph()), so it
	// must NOT go through action_morph even though its producer is Zerg race.
	if (m.ui.unit_race(u) == race_t::zerg && unit_type != (int32_t)UnitTypes::Zerg_Infested_Terran) {
		// BUILDING morphs (hatchery -> lair -> hive, creep colony -> sunken/spore,
		// spire -> greater spire) are a different engine action: action_morph only
		// handles larva / mutalisk / hydralisk and silently `continue`s on a building.
		// Measured with tools/botmatch: UnifiedAI's Lair step was "affordable" for
		// 4000+ frames and never happened -- so no bot ever got past hatchery tech
		// (Steamhammer included).
		if (m.ui.unit_is_zerg_building(ut)) { bot_rec_u16(m, 53, unit_type); return m.ui.action_morph_building(m.bot_player, ut) ? 1 : 0; }
		bot_rec_u16(m, 35, unit_type);
		return m.ui.action_morph(m.bot_player, ut) ? 1 : 0;
	}
	bot_rec_u16(m, 31, unit_type);
	return m.ui.action_train(m.bot_player, ut) ? 1 : 0;
}
static int32_t bot_api_build(ob_bot_host* h, intptr_t worker, int32_t unit_type, int32_t tile_x, int32_t tile_y) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)worker;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "build", u ? "wrong_owner" : "null_unit"); return 0; }
	if ((size_t)unit_type >= 228) { diag_bot_invalid(m, "build", "bad_unit_type", "\"type\":" + std::to_string(unit_type)); return 0; }
	const unit_type_t* ut = m.ui.get_unit_type((UnitTypes)unit_type);
	const order_type_t* order;
	auto r = m.ui.unit_race(u);
	if (r == race_t::zerg) order = m.ui.get_order_type(Orders::DroneStartBuild);
	else if (r == race_t::protoss) order = m.ui.get_order_type(Orders::PlaceProtossBuilding);
	else order = m.ui.get_order_type(Orders::PlaceBuilding);
	xy pos(tile_x * 32 + ut->placement_size.x / 2, tile_y * 32 + ut->placement_size.y / 2);
	std::string where = "\"type\":" + std::to_string(unit_type) + ",\"tile\":[" + std::to_string(tile_x) + "," + std::to_string(tile_y) + "]";
	if (!m.ui.is_in_map_bounds(pos)) { diag_bot_invalid(m, "build", "out_of_bounds", where); return 0; }
	// Note: a can_place_building() rejection is NOT flagged -- speculative placement probing
	// is normal bot trial-and-error and would spam. Only hard invariant breaks emit above.
	if (!m.ui.can_place_building(u, m.bot_player, ut, pos, false, false)) return 0;
	diag_bot_stale(m, u, "build");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_build(m, (int)order->id, tile_x, tile_y, unit_type);
	return m.ui.action_build(m.bot_player, order, ut, xy_t<size_t>{(size_t)tile_x, (size_t)tile_y}) ? 1 : 0;
}
static void bot_api_log(ob_bot_host* h, const char* text) {
	(void)h;
	BOT_HOST_LOG("bot: %s\n", text ? text : "(null)");
}
// --- ABI v2 ------------------------------------------------------------
static int32_t bot_api_research(ob_bot_host* h, intptr_t building, int32_t tech_id) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)building;
	if (!u || u->owner != m.bot_player) return 0;
	if ((size_t)tech_id >= 44) return 0;
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_u8(m, 48, tech_id);
	return m.ui.action_research(m.bot_player, m.ui.get_tech_type((TechTypes)tech_id)) ? 1 : 0;
}
static int32_t bot_api_upgrade(ob_bot_host* h, intptr_t building, int32_t upgrade_id) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)building;
	if (!u || u->owner != m.bot_player) return 0;
	if ((size_t)upgrade_id >= 61) return 0;
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_u8(m, 50, upgrade_id);
	return m.ui.action_upgrade(m.bot_player, m.ui.get_upgrade_type((UpgradeTypes)upgrade_id)) ? 1 : 0;
}
static int32_t bot_api_upgrade_level(ob_bot_host* h, int32_t p, int32_t upgrade_id) {
	if (p < 0 || p >= 12 || (size_t)upgrade_id >= 61) return 0;
	return bot_m(h)->ui.player_upgrade_level(p, (UpgradeTypes)upgrade_id);
}
static int32_t bot_api_tech_researched(ob_bot_host* h, int32_t p, int32_t tech_id) {
	if (p < 0 || p >= 12 || (size_t)tech_id >= 44) return 0;
	return bot_m(h)->ui.st.tech_researched[p][(TechTypes)tech_id] ? 1 : 0;
}
static int32_t bot_api_enemy_player(ob_bot_host* h) {
	auto& m = *bot_m(h);
	for (int p = 0; p != 8; ++p) {
		if (p == m.bot_player) continue;
		if (!m.ui.player_slot_active(p)) continue;
		if (m.ui.st.alliances[m.bot_player][p] != 0) continue;
		return p;
	}
	return -1;
}
static int32_t bot_api_start_locations(ob_bot_host* h, int32_t* out_xy, int32_t max) {
	auto& m = *bot_m(h);
	int32_t n = 0;
	for (int p = 0; p != 8 && n != max; ++p) {
		xy pos = m.ui.game_st.start_locations[p];
		if (pos == xy()) continue;
		out_xy[n * 2] = pos.x;
		out_xy[n * 2 + 1] = pos.y;
		++n;
	}
	return n;
}

// v3: per-build-tile static terrain, row-major. Real BWAPI bots build their
// whole map model (base locations, connectivity flood-fill, choke analysis)
// from walkability/buildability, so without this they plan on a blank map.
static int32_t bot_api_map_tiles(ob_bot_host* h, uint8_t* out, int32_t max) {
	auto& m = *bot_m(h);
	auto& gs = m.ui.game_st;
	int w = (int)gs.map_tile_width, hgt = (int)gs.map_tile_height;
	int64_t need = (int64_t)w * hgt;
	if (!out || need <= 0 || need > max) return 0;
	for (int y = 0; y != hgt; ++y) {
		for (int x = 0; x != w; ++x) {
			size_t i = (size_t)y * w + x;
			auto flags = m.ui.st.tiles[i].flags;
			uint8_t b = 0;
			if (flags & bwgame::tile_t::flag_walkable) b |= OB_TILE_WALKABLE;
			if (~flags & bwgame::tile_t::flag_unbuildable) b |= OB_TILE_BUILDABLE;
			if (flags & bwgame::tile_t::flag_has_creep) b |= OB_TILE_HAS_CREEP;
			out[i] = b;
		}
	}
	return (int32_t)need;
}

// v4: the bot's exploration map. tile.explored is an INVERTED per-player mask
// in bwgame (bit set = NOT yet explored) -- exactly the bit can_place_building
// rejects on -- so report its complement: 1 = the bot may build there.
static int32_t bot_api_explored_tiles(ob_bot_host* h, uint8_t* out, int32_t max) {
	auto& m = *bot_m(h);
	auto& gs = m.ui.game_st;
	int w = (int)gs.map_tile_width, hgt = (int)gs.map_tile_height;
	int64_t need = (int64_t)w * hgt;
	if (!out || need <= 0 || need > max) return 0;
	int mask = 1 << m.bot_player;
	for (int64_t i = 0; i != need; ++i) out[i] = (m.ui.st.tiles[(size_t)i].explored & mask) ? 0 : 1;
	return (int32_t)need;
}

// v4: build a Terran addon on `building`. The addon's tile is derived exactly as
// the engine does (parent top-left + units.dat addon_position) and the order is
// PlaceAddon -- the plain build() path used PlaceBuilding at a bogus (0,0) tile,
// which can_place_building always rejected, so no bot ever got a Machine Shop.
static int32_t bot_api_build_addon(ob_bot_host* h, intptr_t building, int32_t addon_type) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)building;
	if (!u || u->owner != m.bot_player || !u->sprite) return 0;
	if ((size_t)addon_type >= 228) return 0;
	const unit_type_t* ut = m.ui.get_unit_type((UnitTypes)addon_type);
	if (!m.ui.ut_addon(ut)) return 0;
	xy tl = u->sprite->position - u->unit_type->placement_size / 2;
	xy_t<size_t> tile{(size_t)((tl.x + ut->addon_position.x) / 32), (size_t)((tl.y + ut->addon_position.y) / 32)};
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_build(m, (int)Orders::PlaceAddon, (int)tile.x, (int)tile.y, addon_type);
	return m.ui.action_build(m.bot_player, m.ui.get_order_type(Orders::PlaceAddon), ut, tile) ? 1 : 0;
}
static int32_t bot_api_difficulty(ob_bot_host* h) { return bot_m(h)->bot_level; }
static int32_t bot_api_player_race(ob_bot_host* h, int32_t p) {
	if (p < 0 || p >= 12) return -1;
	int r = (int)bot_m(h)->ui.st.players[(size_t)p].race;
	return r >= 0 && r <= 2 ? r : -1;
}
static int32_t bot_api_is_reachable(ob_bot_host* h, intptr_t unit, int32_t x, int32_t y) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) return 0;
	return m.ui.is_reachable(u, xy(x, y)) ? 1 : 0;
}

// v3: extended per-unit state, queried on demand. Kept separate from
// ob_unit so that struct (written in bulk into bot memory) stays frozen.
static int32_t bot_api_unit_ex(ob_bot_host* h, intptr_t unit, ob_unit_ex* out) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || !out) return 0;
	memset(out, 0, sizeof(*out));
	out->energy = u->energy.integer_part();
	out->resources = m.ui.ut_resource(u) ? u->building.resource.resource_count : 0;
	out->target = (intptr_t)u->order_target.unit;
	out->addon = m.ui.ut_building(u) ? (intptr_t)u->building.addon : 0;
	out->remaining_build_time = (int32_t)u->remaining_build_time;
	// The countdown for a unit in production lives on the child unit being
	// built, not on the producing building - the parent's own
	// remaining_build_time refers to the parent's construction.
	out->remaining_train_time = u->current_build_unit
			? (int32_t)u->current_build_unit->remaining_build_time : 0;
	out->ground_cooldown = (int32_t)u->ground_weapon_cooldown;
	out->air_cooldown = (int32_t)u->air_weapon_cooldown;
	out->is_training = u->build_queue.empty() ? 0 : 1;
	out->build_type = u->build_queue.empty() ? -1 : (int32_t)(UnitTypes)u->build_queue.front()->id;
	out->is_lifted = m.ui.ut_building(u) && m.ui.u_flying(u) ? 1 : 0;
	out->is_burrowed = m.ui.u_burrowed(u) ? 1 : 0;
	return 1;
}

// --- ABI v5: abilities ------------------------------------------------------
static int32_t bot_api_use_ability(ob_bot_host* h, intptr_t unit, int32_t ability) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "use_ability", u ? "wrong_owner" : "null_unit"); return 0; }
	diag_bot_stale(m, u, "use_ability");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u);
	switch (ability) {
	case OB_AB_STIM: bot_rec_op(m, 54); return m.ui.action_stim_pack(m.bot_player) ? 1 : 0;
	case OB_AB_SIEGE: bot_rec_u8(m, 38, 0); return m.ui.action_siege(m.bot_player, false) ? 1 : 0;
	case OB_AB_UNSIEGE: bot_rec_u8(m, 37, 0); return m.ui.action_unsiege(m.bot_player, false) ? 1 : 0;
	case OB_AB_BURROW: bot_rec_u8(m, 44, 0); return m.ui.action_burrow(m.bot_player, false) ? 1 : 0;
	case OB_AB_UNBURROW: bot_rec_u8(m, 45, 0); return m.ui.action_unburrow(m.bot_player) ? 1 : 0;
	case OB_AB_CLOAK: bot_rec_u8(m, 33, 0); return m.ui.action_cloak(m.bot_player) ? 1 : 0;
	case OB_AB_DECLOAK: bot_rec_u8(m, 34, 0); return m.ui.action_decloak(m.bot_player) ? 1 : 0;
	case OB_AB_HOLD_POSITION: bot_rec_u8(m, 43, 0); return m.ui.action_hold_position(m.bot_player, false) ? 1 : 0;
	case OB_AB_TRAIN_FIGHTER: bot_rec_op(m, 39); return m.ui.action_train_fighter(m.bot_player) ? 1 : 0;
	default: diag_bot_invalid(m, "use_ability", "bad_ability", "\"ability\":" + std::to_string(ability)); return 0;
	}
}
// Only real spell orders may be issued through cast_*: everything else already
// has its own entry (move / attack / build ...), and action_order would accept
// internal orders that make no sense from outside.
static bool bot_cast_order_ok(int32_t id) {
	switch (id) {
	case OB_ORDER_INFESTATION: case OB_ORDER_LOCKDOWN: case OB_ORDER_DARK_SWARM: case OB_ORDER_PARASITE:
	case OB_ORDER_SPAWN_BROODLINGS: case OB_ORDER_EMP: case OB_ORDER_PLACE_MINE: case OB_ORDER_RECALL:
	case OB_ORDER_SCANNER_SWEEP: case OB_ORDER_DEFENSIVE_MATRIX: case OB_ORDER_PSI_STORM: case OB_ORDER_IRRADIATE:
	case OB_ORDER_PLAGUE: case OB_ORDER_CONSUME: case OB_ORDER_ENSNARE: case OB_ORDER_STASIS:
	case OB_ORDER_HALLUCINATION: case OB_ORDER_RESTORATION: case OB_ORDER_DISRUPTION_WEB:
	case OB_ORDER_MIND_CONTROL: case OB_ORDER_FEEDBACK: case OB_ORDER_OPTICAL_FLARE: case OB_ORDER_MAELSTROM:
		return true;
	default: return false;
	}
}
static int32_t bot_api_cast_at(ob_bot_host* h, intptr_t unit, int32_t order_id, int32_t x, int32_t y) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	if (!u || u->owner != m.bot_player) { diag_bot_invalid(m, "cast_at", u ? "wrong_owner" : "null_unit"); return 0; }
	if (!bot_cast_order_ok(order_id)) { diag_bot_invalid(m, "cast_at", "bad_order", "\"order\":" + std::to_string(order_id)); return 0; }
	diag_bot_stale(m, u, "cast_at");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_order(m, x, y, nullptr, order_id);
	return m.ui.action_order(m.bot_player, m.ui.get_order_type((Orders)order_id), xy(x, y), nullptr, nullptr, false) ? 1 : 0;
}
static int32_t bot_api_cast_on(ob_bot_host* h, intptr_t unit, int32_t order_id, intptr_t target) {
	auto& m = *bot_m(h);
	unit_t* u = (unit_t*)unit;
	unit_t* t = (unit_t*)target;
	if (!u || !t || u->owner != m.bot_player || !t->sprite) { diag_bot_invalid(m, "cast_on", !u ? "null_unit" : (!t ? "null_target" : "wrong_owner")); return 0; }
	if (!bot_cast_order_ok(order_id)) { diag_bot_invalid(m, "cast_on", "bad_order", "\"order\":" + std::to_string(order_id)); return 0; }
	diag_bot_stale(m, u, "cast_on"); diag_bot_stale(m, t, "cast_on_target");
	diag_bot_cmd(m);
	m.ui.action_select(m.bot_player, u);
	bot_rec_select(m, u); bot_rec_order(m, t->sprite->position.x, t->sprite->position.y, t, order_id);
	return m.ui.action_order(m.bot_player, m.ui.get_order_type((Orders)order_id), t->sprite->position, t, nullptr, false) ? 1 : 0;
}

static const ob_host_api bot_host_api_table = {
	OPENBW_BOT_ABI_VERSION,
	bot_api_current_frame,
	bot_api_my_player,
	bot_api_minerals,
	bot_api_gas,
	bot_api_supply_used,
	bot_api_supply_available,
	bot_api_units,
	bot_api_enemy_start,
	bot_api_map_size,
	bot_api_order_move,
	bot_api_order_attack_move,
	bot_api_order_right_click,
	bot_api_order_stop,
	bot_api_train,
	bot_api_build,
	bot_api_log,
	bot_api_research,
	bot_api_upgrade,
	bot_api_upgrade_level,
	bot_api_tech_researched,
	bot_api_enemy_player,
	bot_api_start_locations,
	bot_api_map_tiles,
	bot_api_unit_ex,
	bot_api_explored_tiles,   // v4
	bot_api_build_addon,
	bot_api_difficulty,
	bot_api_player_race,
	bot_api_is_reachable,
	bot_api_use_ability,      // v5
	bot_api_cast_at,
	bot_api_cast_on,
};
