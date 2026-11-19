// Map-written command card button sets (EUD), members of ui_functions.
//
// A map rewrites the exe's button sets (0x5187E8: count, array pointer) or builds its own
// arrays in memory it writes through EUD triggers. Every 20-byte entry is
//   { pos u16, icon u16, cond fn, act fn, cv u16, av u16, enable string u16, disable string u16 }
// where cond / act are addresses of StarCraft.exe 1.16.1 functions. They cannot run here, so each of
// the 67 condition and 60 action functions the exe's tables use is emulated by name (read off the
// disassembly); a pointer outside that list is a map's own x86 code and its entry is skipped.
// Actions become the ordinary card actions, so the recorded game actions are exactly what the
// vanilla button would issue and every HUD, the keyboard and the controller already know them.

	struct eud_entry_t { int pos, icon; uint32_t cond, act; int cv, av, es, ds; };
	enum { eud_k_none, eud_k_back, eud_k_page, eud_k_cancel_slot };
	struct eud_act_t { int kind, a; };
	a_vector<eud_act_t> eud_acts;
	std::array<a_string, 9> eud_labels;
	int eud_page_set = -1;          // a map-driven submenu (set id) shown instead of the unit's own set
	uint32_t eud_page_unit = 0;
	bool eud_key_busy = false;
	bool eud_card_shown = false;    // the last card_buttons() came from a button set

	a_string eud_btn_text(int id, int* hotkey = nullptr) {
		load_unit_names();
		const a_vector<uint8_t>& d = unit_names_tbl.data;
		if (hotkey) *hotkey = 0;
		if (id <= 0 || d.size() < 2) return {};
		const size_t n = d[0] | d[1] << 8;
		if ((size_t)id > n || 2 + 2 * (size_t)id > d.size()) return {};
		size_t o = d[2 * id] | d[2 * id + 1] << 8;
		if (o + 2 >= d.size()) return {};
		if (hotkey) *hotkey = d[o];
		a_string s;
		for (size_t i = o + 2; i < d.size() && d[i]; ++i) if (d[i] >= 0x20 && d[i] < 0x7f) s += (char)d[i];
		return s;
	}
	// The wheel label: the map's own text, short and upper case (HUD v3 wheel style).
	static a_string eud_short_label(a_string l) {
		for (const char* p : {"Activate ", "Use ", "Build ", "Warp in ", "Morph to ", "Research ", "Evolve ", "Develop ", "Upgrade ", "Train ", "Cast "})
			if (l.compare(0, strlen(p), p) == 0) { l = l.substr(strlen(p)); break; }
		for (char& c : l) c = (char)toupper((unsigned char)c);
		if (l.size() > 12) l = l.substr(0, 11) + ".";
		return l;
	}

	// --- conditions (the exe's 0x4282D0 ... 0x429740 family, return 1 shown / -1 shown disabled / 0 hidden) ---
	a_vector<unit_t*> eud_group() {
		a_vector<unit_t*> g;
		for (auto uid : current_selection) if (unit_t* x = get_unit(uid)) g.push_back(x);
		return g;
	}
	bool eud_unit_usable(const unit_t* u) { return u->owner == game_local_player && u_completed(u) && !unit_is_disabled(u); }
	int eud_train_check(unit_t* u, int type) {
		if (type < 0 || type >= 228) return 0;
		if (req_active()) return req_unit_check(u, type, game_local_player);
		if (!eud_unit_usable(u) || u_hallucination(u) || !game_st.unit_type_allowed[game_local_player][(UnitTypes)type]) return 0;
		return unit_can_build(u, get_unit_type((UnitTypes)type)) ? 1 : -1;
	}
	int eud_use_check(unit_t* u, int tech) {
		if (tech < 0 || tech >= 44) return 0;
		if (req_active()) return req_use_check(u, tech, game_local_player);
		if (!eud_unit_usable(u) || u_hallucination(u) || !player_tech_available(game_local_player, (TechTypes)tech)) return 0;
		return unit_can_use_tech(u, get_tech_type((TechTypes)tech)) ? 1 : -1;
	}
	int eud_research_check(unit_t* u, int tech) {
		if (tech < 0 || tech >= 44) return 0;
		if (req_active()) return req_research_check(u, tech, game_local_player);
		const int pl = game_local_player;
		if (!eud_unit_usable(u) || !player_tech_available(pl, (TechTypes)tech) || player_has_researched(pl, (TechTypes)tech) || player_is_researching(pl, (TechTypes)tech)) return 0;
		return unit_can_research(u, get_tech_type((TechTypes)tech), pl) ? 1 : -1;
	}
	int eud_upgrade_check(unit_t* u, int up) {
		if (up < 0 || up >= 61) return 0;
		if (req_active()) return req_upgrade_check(u, up, game_local_player);
		const int pl = game_local_player;
		if (!eud_unit_usable(u) || player_upgrade_level(pl, (UpgradeTypes)up) >= player_max_upgrade_level(pl, (UpgradeTypes)up) || player_is_upgrading(pl, (UpgradeTypes)up)) return 0;
		return unit_can_upgrade(u, get_upgrade_type((UpgradeTypes)up), pl) ? 1 : -1;
	}
	bool eud_queue_busy(const unit_t* u) {   // 0x401500
		if (!u->build_queue.empty()) return true;
		if (u->secondary_order_type && u->secondary_order_type->id == Orders(37) && u_grounded_building(u) && u->building.addon && !u_completed(u->building.addon)) return true;
		if (u->order_type->id == Orders(31) && u_grounded_building(u) && u->order_target.unit && !u_completed(u->order_target.unit)) return true;
		return false;
	}
	bool eud_has_cargo(const unit_t* u) {    // 0x4E7110
		for (auto id : u->loaded_units) {
			unit_t* c = get_unit(id);
			if (c && !(c->order_type->id == Orders::Die && c->order_state == 1)) return true;
		}
		return false;
	}
	int eud_cargo_space(const unit_t* u) {   // 0x4E7170
		int n = 0;
		for (auto id : u->loaded_units) {
			unit_t* c = get_unit(id);
			if (c && !(c->order_type->id == Orders::Die && c->order_state == 1)) n += c->unit_type->space_required;
		}
		return n;
	}
	static bool eud_sieged(const unit_t* g) { return g->unit_type->id == UnitTypes::Terran_Siege_Tank_Siege_Mode || g->unit_type->id == UnitTypes::Hero_Edmund_Duke_Siege_Mode; }
	static int eud_cloak_tech(const unit_t* g) {   // 0x423540 / 0x429210: the tech a unit cloaks with
		switch ((int)g->unit_type->id) {
		case 1: case 16: case 100: case 99: case 104: case 51: return 10;
		case 8: case 21: return 9;
		default: return 44;
		}
	}
	bool eud_can_rc_or_rally(const unit_t* g) { return g->unit_type->right_click_action || (u_grounded_building(g) && req_rally_building(g)); }

	int eud_rally_cond(unit_t* u) {   // 0x429520
		if (u->owner != game_local_player || unit_is_disabled(u)) return 0;
		if (u_burrowed(u) && !unit_is(u, UnitTypes::Zerg_Lurker)) return 0;
		const int t = (int)u->unit_type->id, o = (int)u->order_type->id;
		if (t == 7 && o == 33) return 0;
		if ((t == 1 || t == 16 || t == 100 || t == 99 || t == 104) && o == 129) return 0;
		if ((t == 68 || t == 63) && o == 106) return 0;
		return req_active() ? req_order_check(u, 40) : 1;
	}

	int eud_cond(uint32_t fn, int cv, unit_t* u, bool& known) {
		known = true;
		const int pl = game_local_player;
		const a_vector<unit_t*> G = eud_group();
		auto any = [&](auto f) { for (unit_t* g : G) if (f(g)) return true; return false; };
		auto all = [&](auto f) { for (unit_t* g : G) if (!f(g)) return false; return true; };
		auto constructing = [](unit_t* g) { return g->order_type->id == Orders::ConstructingBuilding; };
		auto carrying = [](unit_t* g) { return (g->carrying_flags & 3) != 0; };
		auto is_t = [](unit_t* g, std::initializer_list<int> l) { for (int t : l) if ((int)g->unit_type->id == t) return true; return false; };
		auto build_page = [&](std::initializer_list<int> l) {
			for (int t : l) if (eud_train_check(u, t) != 0) return true;
			return false;
		};
		auto can_attack_g = [&](unit_t* g) {
			if (!((unit_is(g, UnitTypes::Zerg_Lurker) && u_burrowed(g)) || g->unit_type->right_click_action || (u_grounded_building(g) && req_rally_building(g)))) return false;
			return unit_can_attack(g);
		};
		switch (fn) {
		case 0x4282d0: return 1;
		case 0x428310: return all(constructing) ? 1 : 0;
		case 0x428340: return req_unit_d0(u) ? 0 : 1;
		case 0x428360: { const bool p = u_burrowed(u); return all([&](unit_t* g) { return u_burrowed(g) == p; }) ? 1 : 0; }
		case 0x4283c0: return any([&](unit_t* g) { return u_burrowed(g); }) ? 0 : 1;
		case 0x4283f0: return G.size() == 1 && !u_grounded_building(u) ? 1 : 0;
		case 0x428420: return u_grounded_building(u) ? 0 : 1;
		case 0x428440: return unit_is_disabled(u) ? 0 : 1;
		case 0x428480: return any(carrying) ? 1 : 0;
		case 0x4284b0: return any([&](unit_t* g) { return !carrying(g); }) ? 1 : 0;
		case 0x4284e0: return u_completed(u) ? 0 : 1;
		case 0x428500: return is_t(u, {131, 132, 133}) ? 1 : 0;
		case 0x428530: return !u->build_queue.empty() && (int)u->build_queue.front()->id <= 105 ? 1 : 0;
		case 0x428560: return any(carrying) && any([&](unit_t* g) { return !constructing(g); }) ? 1 : 0;
		case 0x4285a0: return any([&](unit_t* g) { return !carrying(g); }) && any([&](unit_t* g) { return !constructing(g); }) ? 1 : 0;
		case 0x4285e0: case 0x428610: case 0x428640: case 0x428670:
			return any([&](unit_t* g) { return !constructing(g); }) ? 1 : 0;
		case 0x4286a0: return any([&](unit_t* g) { return is_t(g, {83, 81}) && req_unit_c8(g) != 0; }) ? 1 : -1;
		case 0x4286e0: return any([&](unit_t* g) { return is_t(g, {72, 82}) && (uint8_t)(req_unit_c8(g) + req_unit_c9(g)) != 0; }) ? 1 : -1;
		case 0x428730: return !any([&](unit_t* g) { return u_burrowed(g); }) && any(carrying) ? 1 : 0;
		case 0x428780: return !any([&](unit_t* g) { return u_burrowed(g); }) && any([&](unit_t* g) { return !carrying(g); }) ? 1 : 0;
		case 0x4287d0: return u_grounded_building(u) && !eud_queue_busy(u) && req_unit_c8(u) == 44 && req_unit_c9(u) == 61 ? 1 : 0;
		case 0x428810: {
			for (unit_t* x : ptr(st.player_units[(size_t)u->owner])) if (unit_is(x, UnitTypes::Terran_Nuclear_Silo) && x->building.silo.nuke) return 1;
			return -1;
		}
		case 0x428860: return any(eud_sieged) ? 0 : 1;
		case 0x4288e0: return req_unit_c8(u) != 44 ? 1 : 0;
		case 0x428900: return req_unit_c9(u) != 61 ? 1 : 0;
		case 0x428920: return u->secondary_order_type && u->secondary_order_type->id == Orders(37) && u_grounded_building(u) && u->building.addon && !u_completed(u->building.addon) ? 1 : 0;
		case 0x428960: return G.size() == 1 ? eud_train_check(u, cv) : 0;
		case 0x428990: return G.size() == 1 && any([&](unit_t* g) { return !constructing(g); }) && build_page({113, 114, 116, 123}) ? 1 : 0;
		case 0x428a10: return G.size() == 1 && any([&](unit_t* g) { return !constructing(g); }) && build_page({106, 109, 110, 111, 122, 124, 112, 125}) ? 1 : 0;
		case 0x428ad0: return G.size() == 1 && build_page({155, 167, 163, 171, 169, 165, 159, 170}) ? 1 : 0;
		case 0x428b80: return G.size() == 1 && build_page({154, 156, 157, 160, 166, 162, 164, 172}) ? 1 : 0;
		case 0x428c30: return G.size() == 1 && !u_burrowed(u) && build_page({134, 141, 138, 140, 136}) ? 1 : 0;
		case 0x428cb0: return G.size() == 1 && !u_burrowed(u) && build_page({131, 143, 149, 142, 139, 135}) ? 1 : 0;
		case 0x428d40:
			return any([&](unit_t* g) { return (unit_is(g, UnitTypes::Zerg_Lurker) && u_burrowed(g)) || eud_can_rc_or_rally(g); }) ? 1 : 0;
		case 0x428da0:
			return any([&](unit_t* g) {
				if (unit_is(g, UnitTypes::Zerg_Lurker) && u_burrowed(g)) return false;
				const int rc = (int)g->unit_type->right_click_action;
				return rc ? rc != 3 : (u_grounded_building(g) && req_rally_building(g));
			}) ? 1 : 0;
		case 0x428e00: {
			if (G.empty()) return 0;
			for (unit_t* g : G) if (g->unit_type != u->unit_type) return 0;
			return eud_train_check(G[0], cv);
		}
		case 0x428e60:
			if (G.size() > 1 && !is_t(u, {35, 43, 38})) return 0;
			return eud_train_check(u, cv);
		case 0x428ea0: return any([&](unit_t* g) { return eud_has_cargo(g); }) ? 1 : 0;
		case 0x428ed0: return any([&](unit_t* g) { return !unit_is_disabled(g) && unit_can_attack(g); }) ? 1 : 0;
		case 0x428f30: return any(can_attack_g) ? 1 : 0;
		case 0x428fa0: return !any([&](unit_t* g) { return u_burrowed(g); }) && any(can_attack_g) ? 1 : 0;
		case 0x428ff0:
			for (unit_t* g : G) {
				if (u_hallucination(g)) return 0;
				if (unit_is(g, UnitTypes::Zerg_Overlord) && !player_upgrade_level(g->owner, UpgradeTypes::Ventral_Sacs)) return 0;
				if (!g->unit_type->space_provided) return 0;
				if (eud_cargo_space(g) < (int)g->unit_type->space_provided) return 1;
			}
			return 0;
		case 0x429070: {
			for (unit_t* g : G) {
				if (!(g->unit_type->flags & 0x100000)) continue;
				if (!u_burrowed(g) || g->order_type->id == Orders::Burrowing || g->order_type->id == Orders::Unburrowing) return 0;
				if (eud_use_check(g, cv) != 1) return 0;
			}
			return 1;
		}
		case 0x4290f0:
			for (unit_t* g : G) {
				if (!(g->unit_type->flags & 0x100000)) continue;
				const int r = eud_use_check(g, cv);
				if (r != 1) return r;
				if (u_burrowed(g) || g->order_type->id == Orders::Burrowing || g->order_type->id == Orders::Unburrowing) continue;
				return 1;
			}
			return 0;
		case 0x429170: return eud_use_check(u, cv) == 1 && any(eud_sieged) ? 1 : 0;
		case 0x4291c0: { const int r = eud_use_check(u, cv); return r != 1 ? r : (any(eud_sieged) ? 0 : 1); }
		case 0x429210: case 0x429370:
			for (unit_t* g : G) {
				if (eud_use_check(g, fn == 0x429210 ? eud_cloak_tech(g) : cv) != 1) return 0;
				if ((g->status_flags & 0x800) && !u_burrowed(g)) return 0;
				if (!(g->status_flags & 0x100)) return 0;
			}
			return 1;
		case 0x4292c0:
			for (unit_t* g : G) {
				if (eud_use_check(g, eud_cloak_tech(g)) != 1) continue;
				if ((g->status_flags & 0x800) && !u_burrowed(g)) continue;
				if (!(g->status_flags & 0x100)) return 1;
			}
			return 0;
		case 0x4293e0:
			for (unit_t* g : G) {
				const int r = eud_use_check(g, cv);
				if (r != 1) return r;
				if ((g->status_flags & 0x800) && !u_burrowed(g)) continue;
				if (!(g->status_flags & 0x100)) return 1;
			}
			return 0;
		case 0x429450: return eud_upgrade_check(u, cv);
		case 0x429470: {
			int best = 0;
			for (unit_t* g : G) {
				const int r = eud_use_check(g, cv);
				if (r == 1) return 1;
				if (r < best) best = r;
			}
			return best;
		}
		case 0x4294e0: return eud_use_check(u, cv);
		case 0x429500: return eud_research_check(u, cv);
		case 0x429520: return eud_rally_cond(u);
		case 0x429640: case 0x4296b0:
			return eud_use_check(u, fn == 0x429640 ? 28 : 23) == 1 ? (G.size() == 1 ? -1 : 1) : 0;
		case 0x429680: case 0x4296f0: return eud_use_check(u, cv) == -1 ? -1 : 0;
		case 0x429720: return eud_use_check(u, 32);
		case 0x429740: return is_t(u, {131, 132, 133}) ? eud_rally_cond(u) : 0;
		case 0x4deac0: case 0x4deaf0: case 0x4deb40: (void)pl; return 0;   // replay speed controls: the replay bar has its own
		default: known = false; return 0;
		}
	}

	// --- actions -----------------------------------------------------------------------------
	// An action argument the exe would hand to the engine as a type that does not exist hides the entry.
	static bool eud_av_ok(uint32_t fn, int av) {
		switch (fn) {
		case 0x4234b0: case 0x423790: case 0x423860: case 0x423c50: case 0x423d10: case 0x423dd0: case 0x423eb0: return av >= 0 && av < 228;
		case 0x423350: return (av & 0xff) < 44;
		case 0x423310: return (av & 0xff) < 61;
		default: return true;
		}
	}
	// Fills b for the entry's action function; false = not one of the exe's functions.
	bool eud_action(uint32_t fn, int av, unit_t* u, card_button_t& b) {
		auto tgt = [&](Orders o) { b.action = cardact_ability_target; b.arg = (int)o; };
		auto abil = [&](int op) { b.action = cardact_ability; b.arg = op; };
		auto act = [&](int a, int arg) { b.action = a; b.arg = arg; };
		auto ext = [&](int kind, int a) { eud_acts.push_back({kind, a}); b.action = cardact_eud; b.arg = (int)eud_acts.size() - 1; };
		const bool bld = u_grounded_building(u);
		switch (fn) {
		case 0x423180: case 0x429770: case 0x429780: ext(eud_k_back, 0); return true;
		case 0x4231d0: act(cardact_cancel, 46); return true;
		case 0x4231f0: abil(90); return true;
		case 0x423210: abil(42); return true;
		case 0x423230: act(cardact_liftoff, 0); return true;
		case 0x423270: abil(34); return true;
		case 0x423290: abil(45); return true;
		case 0x4232b0: abil(44); return true;
		case 0x4232d0: act(cardact_cancel, 52); return true;
		case 0x4232f0: act(cardact_cancel, 51); return true;
		case 0x423310: act(cardact_upgrade, av & 0xff); return true;
		case 0x423330: act(cardact_cancel, 49); return true;
		case 0x423350: act(cardact_research, av & 0xff); return true;
		case 0x423370: abil(43); return true;
		case 0x423390: abil(39); return true;
		case 0x4233b0: abil(28); return true;
		case 0x4233d0: abil(27); return true;
		case 0x4233f0: act(cardact_stop, 0); return true;
		case 0x423410: act(cardact_cancel, 25); return true;
		case 0x423430: act(cardact_cancel, 24); return true;
		case 0x423450: abil(38); return true;
		case 0x423470: abil(37); return true;
		case 0x423490: if ((av & 0xffff) == 254) act(cardact_cancel, 32); else ext(eud_k_cancel_slot, av & 0xffff); return true;
		case 0x4234b0: act(cardact_train, av); return true;
		case 0x4234d0: abil(54); return true;
		case 0x423730: abil(33); return true;
		case 0x423760: abil(30); return true;
		case 0x423790: act(cardact_morph, av); return true;
		case 0x423860: act(cardact_morph_building, av); return true;
		case 0x423930: act(cardact_select_larva, 0); return true;
		case 0x4239e0: act(cardact_attack, 0); return true;
		case 0x423a40: tgt(Orders::NukePaint); return true;
		case 0x423aa0: tgt(Orders::RechargeShieldsBattery); return true;
		case 0x423b00: if (bld) abil(40); else tgt(Orders::MoveUnload); return true;
		case 0x423b40: tgt(bld ? Orders::PickupBunker : Orders::PickupTransport); return true;
		case 0x423b70: tgt(Orders::Harvest1); return true;
		case 0x423bd0: tgt(Orders::Repair); return true;
		case 0x423c30: act(cardact_land, 0); return true;
		case 0x423c40: act(cardact_nydus_exit, 0); return true;
		case 0x423c50: case 0x423dd0: case 0x423eb0: act(cardact_build, av); return true;
		case 0x423d10: act(cardact_addon, av); return true;
		case 0x423f70: {
			static const int order_of_tech[32] = {0, 115, 122, 132, 139, 0, 141, 143, 113, 0, 0, 0, 27, 121, 119, 144, 145, 146, 120, 142, 148, 137, 147, 0, 180, 181, 0, 182, 0, 184, 185, 186};
			const int o = (av & 0xff) < 32 ? order_of_tech[av & 0xff] : 0;
			if (o) tgt((Orders)o); else ext(eud_k_none, 0);
			return true;
		}
		case 0x423e90: case 0x4df260: case 0x4df2c0: case 0x4df300: ext(eud_k_none, 0); return true;
		case 0x424140: tgt(Orders::Patrol); return true;
		case 0x4241a0: case 0x424200: case 0x4242c0: case 0x424320: case 0x424380: act(cardact_attack, 0); return true;
		case 0x424260: case 0x424440: tgt(Orders::Move); return true;
		case 0x4244a0: tgt(Orders::RallyPointTile); return true;
		case 0x459af0: ext(eud_k_page, av); return true;
		default: return false;
		}
	}

	void eud_activate(int idx) {
		if (idx < 0 || (size_t)idx >= eud_acts.size()) return;
		const eud_act_t a = eud_acts[(size_t)idx];
		switch (a.kind) {
		case eud_k_back:
			eud_page_set = -1;
			card_menu = 0;
			attack_armed = false;
			ability_armed_order = -1;
			break;
		case eud_k_page:
			if (unit_t* u = focus_or_primary()) { eud_page_set = a.a > 228 && a.a < 250 ? a.a : -1; eud_page_unit = get_unit_id(u).raw_value; }
			attack_armed = false;
			ability_armed_order = -1;
			break;
		case eud_k_cancel_slot:
			if (submit_select_action()) {
				game_action_writer w;
				w.u8(32);
				w.u16((uint16_t)a.a);
				submit_game_action(w);
			}
			break;
		default: break;
		}
	}

	// --- the card ----------------------------------------------------------------------------
	uint32_t eud_exe_dword(uint32_t a) const {
		const a_vector<uint8_t>& x = global_st.exe_buttons;
		if (x.empty() || a - eud::buttons_base > x.size() - 4) return 0;
		const size_t o = a - eud::buttons_base;
		return (uint32_t)x[o] | (uint32_t)x[o + 1] << 8 | (uint32_t)x[o + 2] << 16 | (uint32_t)x[o + 3] << 24;
	}
	// A set counts as touched only when the map changed its count, pointer or a word of its own array:
	// writing the original value back (memory-restore loops do) leaves the app's layout alone.
	bool eud_force_all = false;   // harness only
	bool eud_set_touched(uint32_t count, uint32_t arr, uint32_t exe_count, uint32_t exe_arr) const {
		if (eud_force_all || count != exe_count || arr != exe_arr) return true;
		for (uint32_t k = 0; k != 5 * exe_count; ++k) {
			auto i = st.eud_deaths.find((arr + 4 * k - eud::deaths_base) / 4u);
			if (i != st.eud_deaths.end() && (uint32_t)i->second != eud_exe_dword(arr + 4 * k)) return true;
		}
		return false;
	}
	// The set's entries, or false when it is not one a map touched. page: a submenu the card moved to.
	bool eud_read_set(uint32_t set_id, bool page, a_vector<eud_entry_t>& out) {
		const uint32_t a = eud::button_sets + 12 * set_id;
		uint32_t count = 0, arr = 0, exe_count = 0, exe_arr = 0;
		if (!eud_ui_dword(a, count) || !eud_ui_dword(a + 4, arr) || !arr) return false;
		const a_vector<uint8_t>& x = global_st.exe_buttons;
		if (!x.empty()) {
			size_t o = a - eud::buttons_base;
			for (int k = 0; k != 4; ++k) exe_count |= (uint32_t)x[o + k] << (8 * k), exe_arr |= (uint32_t)x[o + 4 + k] << (8 * k);
		}
		if (!page && !eud_set_touched(count, arr, exe_count, exe_arr)) return false;
		count = std::min<uint32_t>(count, arr == exe_arr ? exe_count : 16);
		out.clear();
		for (uint32_t k = 0; k != count; ++k) {
			uint32_t w[5] = {0, 0, 0, 0, 0};
			for (int j = 0; j != 5; ++j) eud_ui_dword(arr + 20 * k + 4 * j, w[j]);
			out.push_back({(int)(w[0] & 0xffff), (int)(w[0] >> 16), w[1], w[2], (int)(w[3] & 0xff), (int)(w[3] >> 16), (int)(w[4] & 0xffff), (int)(w[4] >> 16)});
		}
		return true;
	}

	void apply_eud_button_set(static_vector<card_button_t, 9>& r) {
		eud_card_shown = false;
		if (!game_st.eud_triggers || card_menu != 0 || placing_type) return;
		unit_t* u = focus_or_primary();
		if (!u || !u_completed(u) || (size_t)u->unit_type->id >= 228) return;
		if (eud_page_set >= 0 && eud_page_unit != get_unit_id(u).raw_value) eud_page_set = -1;
		const bool page = eud_page_set > 228 && eud_page_set < 250;
		a_vector<eud_entry_t> e;
		if (!eud_read_set(page ? (uint32_t)eud_page_set : (uint32_t)u->unit_type->id, page, e)) { eud_page_set = -1; return; }
		eud_card_shown = true;
		eud_acts.clear();
		a_vector<int> cache(e.size(), 2);
		auto cond = [&](size_t i) {
			if (cache[i] == 2) { bool known; cache[i] = eud_av_ok(e[i].act, e[i].av) ? eud_cond(e[i].cond, e[i].cv, u, known) : 0; }
			return cache[i];
		};
		const int own_icon = (int)get_unit_type(u->unit_type->id)->behaves_as;
		r.clear();
		for (int i = 0; i != 9; ++i) r.push_back({"", cardact_none, 0});
		size_t idx = 0;
		for (int pos = 1; pos <= 9 && idx < e.size(); ++pos) {
			while (idx < e.size() && cond(idx) == 0) ++idx;
			if (idx >= e.size() || e[idx].pos > pos) continue;
			const eud_entry_t& en = e[idx];
			card_button_t b{"", cardact_none, 0};
			if (!eud_action(en.act, en.av, u, b)) { eud_acts.push_back({eud_k_none, 0}); b.action = cardact_eud; b.arg = (int)eud_acts.size() - 1; }
			b.icon = cmdicons_grp.frames.empty() || en.icon < (int)cmdicons_grp.frames.size() ? en.icon : own_icon;
			b.available = cond(idx) > 0;
			b.tbl = en.es;
			b.hint = en.ds;
			eud_labels[(size_t)pos - 1] = eud_short_label(eud_btn_text(en.es));
			b.label = eud_labels[(size_t)pos - 1].c_str();
			r[(size_t)pos - 1] = b;
			++idx;
		}
	}

	// 0x68C14C, "Current Button Set": what the card shows (229 while a target is armed, 230 while a building is
	// placed, 228 with nothing selected). Single player only; every change is logged as (frame, value) for the
	// replay trailer, and a playback applies it at the same point of the frame (replay.h), so the sim sees the same.
	a_vector<std::pair<uint32_t, uint32_t>> eud_set_log;
	uint32_t eud_set_last = 0;
	uint32_t eud_current_set() {
		if (placing_type) return 230;
		if (attack_armed || ability_armed_order >= 0) return 229;
		unit_t* u = focus_or_primary();
		if (u && u_completed(u) && (size_t)u->unit_type->id < 228)
			return eud_page_set > 228 && eud_page_unit == get_unit_id(u).raw_value ? (uint32_t)eud_page_set : (uint32_t)u->unit_type->id;
		return 228;
	}
	void eud_button_set_tick() {
		if (!game_st.eud_triggers || game_local_player < 0) return;
		const uint32_t set = eud_current_set();
		if (set == eud_set_last) return;
		eud_set_last = set;
		eud_set_log.emplace_back((uint32_t)st.current_frame, set);
		eud_write(0x68C14Cu, set, 0xffffu);
	}
