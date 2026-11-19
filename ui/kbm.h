// Keyboard + mouse play (Settings > Valdymas > "Įvesties režimas").
//
// Included INSIDE struct ui_functions (ui.h), after hud3.h. With kbm_mode off
// nothing here runs and touch play is unchanged. Every action goes through the
// existing paths (card_activate, recall_control_group, center_camera_on, ...):
// no new game actions, the simulation and replays see what a tap would send.
//
// Bindings arrive from the launcher as --key-binds "id=code,id=code". A code is an
// SDL scancode, or native_window::event_t::mouse_key_base + an SDL mouse button.
// --key-layout classic: command-card buttons answer to the original letters from
// rez\stat_txt.tbl instead of the Q W E / A S D / Z X C grid slots; "cmd<N>=code"
// in --key-binds moves the button whose stat_txt entry is N to another key.

	enum {
		kb_cam_up, kb_cam_down, kb_cam_left, kb_cam_right,
		kb_card0, kb_card8 = kb_card0 + 8,
		kb_cancel, kb_menu, kb_chat, kb_alert, kb_idle, kb_army, kb_center, kb_zoom_in, kb_zoom_out, kb_subgroup,
		kb_group0, kb_group9 = kb_group0 + 9,
		kb_camloc0, kb_camloc2 = kb_camloc0 + 2,
		kb_mouse_select, kb_mouse_command, kb_mouse_pan,
		kb_count
	};
	static constexpr int kb_mouse = native_window::event_t::mouse_key_base;
	static_assert(kbm_defaults::count == kb_count && kbm_defaults::mouse_base == kb_mouse, "kbm_defaults.h out of step with kb_*");

	bool kbm_mode = false;
	bool kbm_edge_scroll = true;
	bool kbm_classic = false;
	std::map<int, int> kbm_cmd_keys;   // classic: stat_txt entry -> scancode
	std::array<int, kb_count> kbm_bind = kbm_default_binds();
	std::array<bool, kb_count> kbm_held{};
	std::array<xy, 3> kbm_camloc{{xy(-1, -1), xy(-1, -1), xy(-1, -1)}};
	int kbm_last_group = -1;
	std::chrono::steady_clock::time_point kbm_last_group_t;
	// LAN: the players whose launcher runs in keyboard + mouse mode (--kbm-players).
	a_vector<a_string> kbm_players;

	static std::array<int, kb_count> kbm_default_binds() {
		std::array<int, kb_count> b{};
		for (int a = 0; a != kb_count; ++a) b[(size_t)a] = kbm_defaults::table[a].code;
		return b;
	}

	static a_string kbm_id(int a) { return kbm_defaults::table[a].id; }

	void kbm_parse_binds(const a_string& s) {
		size_t p = 0;
		while (p < s.size()) {
			size_t e = s.find(',', p);
			if (e == a_string::npos) e = s.size();
			a_string item = s.substr(p, e - p);
			p = e + 1;
			size_t eq = item.find('=');
			if (eq == a_string::npos) continue;
			a_string id = item.substr(0, eq);
			int code = atoi(item.substr(eq + 1).c_str());
			if (id.size() > 3 && id.compare(0, 3, "cmd") == 0) {
				const int t = atoi(id.c_str() + 3);
				if (t > 0 && code > 0) kbm_cmd_keys[t] = code;
				continue;
			}
			for (int a = 0; a != kb_count; ++a) if (kbm_id(a) == id) { kbm_bind[a] = code; break; }
		}
	}

	// stat_txt.tbl entry (1-based) of the original button string for a card button, 0 = none.
	// An entry is: hotkey byte, type byte, label. Indices are those of the 1.16.1 table;
	// a localised table keeps the indices and brings its own letters.
	struct kbm_tbl_unit_t { int16_t unit, tbl; };
	static const a_vector<kbm_tbl_unit_t>& kbm_tbl_units() {
		static const a_vector<kbm_tbl_unit_t> v = {
			{37, 575}, {38, 576}, {39, 577}, {41, 578}, {42, 579}, {43, 580}, {44, 581}, {45, 582}, {46, 583}, {47, 584},
			{50, 585}, {0, 586}, {1, 587}, {32, 588}, {2, 589}, {3, 590}, {5, 591}, {7, 592}, {8, 593}, {9, 594},
			{11, 595}, {12, 596}, {14, 597}, {84, 598}, {64, 599}, {65, 600}, {66, 601}, {67, 602}, {69, 603}, {70, 604},
			{71, 605}, {72, 606}, {73, 607}, {83, 608}, {85, 609}, {131, 613}, {143, 614}, {149, 615}, {142, 616}, {139, 617},
			{135, 618}, {134, 619}, {141, 620}, {138, 621}, {140, 622}, {136, 623}, {132, 624}, {133, 625}, {137, 626}, {144, 627},
			{146, 628}, {154, 630}, {156, 631}, {157, 632}, {160, 633}, {166, 634}, {162, 635}, {164, 636}, {172, 637}, {155, 638},
			{159, 639}, {163, 640}, {165, 641}, {167, 642}, {169, 643}, {170, 644}, {171, 645}, {106, 646}, {109, 647}, {110, 648},
			{111, 649}, {122, 650}, {124, 651}, {112, 652}, {125, 653}, {113, 654}, {114, 655}, {116, 656}, {123, 657}, {107, 658},
			{108, 659}, {115, 660}, {117, 661}, {118, 662}, {120, 663}, {103, 1289}, {34, 1290}, {58, 1291}, {60, 1292}, {61, 1293},
			{62, 1288}};
		return v;
	}
	static int kbm_tbl_order(Orders o) {
		switch (o) {
		case Orders::Move: return 664;
		case Orders::Patrol: return 667;
		case Orders::RallyPointTile: return 672;
		case Orders::Harvest1: return 675;
		case Orders::Repair: return 677;
		case Orders::PickupTransport: return 683;
		case Orders::CastNuclearStrike: return 685;
		case Orders::CastLockdown: return 335;
		case Orders::PlaceMine: return 336;
		case Orders::CastScannerSweep: return 337;
		case Orders::CastDefensiveMatrix: return 340;
		case Orders::CastEMPShockwave: return 341;
		case Orders::CastIrradiate: return 342;
		case Orders::FireYamatoGun: return 343;
		case Orders::CastInfestation: return 374;
		case Orders::CastSpawnBroodlings: return 375;
		case Orders::CastDarkSwarm: return 376;
		case Orders::CastParasite: return 377;
		case Orders::CastPlague: return 378;
		case Orders::CastConsume: return 379;
		case Orders::CastEnsnare: return 381;
		case Orders::CastPsionicStorm: return 400;
		case Orders::CastHallucination: return 401;
		case Orders::CastRecall: return 402;
		case Orders::CastStasisField: return 403;
		case Orders::CastRestoration: return 1248;
		case Orders::CastOpticalFlare: return 1249;
		case Orders::CastDisruptionWeb: return 1265;
		case Orders::CastMindControl: return 1266;
		case Orders::CastFeedback: return 1267;
		case Orders::CastMaelstrom: return 1268;
		default: return 0;
		}
	}
	static int kbm_tbl_opcode(int opcode, int icon) {
		switch (opcode) {
		case 30: return 676;   // return cargo
		case 33: return 344;   // cloak
		case 34: return 345;   // decloak
		case 37: return 339;   // tank mode
		case 38: return 338;   // siege mode
		case 39: return icon == (int)UnitTypes::Protoss_Scarab ? 609 : 607;
		case 40: return 684;   // unload all
		case 42: return 404;   // archon warp
		case 43: return 668;   // hold position
		case 44: return 372;   // burrow
		case 45: return 373;   // unburrow
		case 54: return 334;   // stim packs
		case 90: return 1264;  // dark archon meld
		default: return 0;
		}
	}
	static int kbm_tbl_tech(int t) {
		static const int16_t v[] = {324, 325, 326, 327, 0, 328, 329, 330, 331, 332, 333, 364, 365, 366, 367, 369, 371, 370, 368,
			395, 396, 397, 398, 399, 1245, 1260, 0, 1261, 0, 1262, 1246, 1263, 1253};
		return t >= 0 && t < (int)(sizeof v / sizeof v[0]) ? v[t] : 0;
	}
	static int kbm_tbl_upgrade(int u) {
		if (u >= 0 && u <= 44) return 456 + u;
		switch (u) {
		case 47: return 1285; case 49: return 1286; case 51: return 1281;
		case 52: return 1284; case 53: return 1283; case 54: return 1282;
		default: return 0;
		}
	}
	int kbm_button_tbl(int action, int arg, int icon) {
		switch (action) {
		case cardact_train: case cardact_build: case cardact_morph: case cardact_morph_building: case cardact_addon:
			for (auto& e : kbm_tbl_units()) if (e.unit == arg) return e.tbl;
			return 0;
		case cardact_stop: return 665;
		case cardact_attack: return icon == 365 ? 1247 : 666;   // the Medic's slot is Heal
		case cardact_menu_build:
		case cardact_menu_adv: {
			const bool zerg = game_local_player >= 0 && st.players[game_local_player].race == race_t::zerg;
			return action == cardact_menu_adv ? (zerg ? 681 : 679) : (zerg ? 680 : 678);
		}
		case cardact_select_larva: return 674;
		case cardact_land: return 670;
		case cardact_liftoff: return 671;
		case cardact_ability: return kbm_tbl_opcode(arg, icon);
		case cardact_ability_target: return kbm_tbl_order((Orders)arg);
		case cardact_research: return kbm_tbl_tech(arg);
		case cardact_upgrade: return kbm_tbl_upgrade(arg);
		default: return 0;
		}
	}
	// The original hotkey byte of a card button (lower case), 0x1b for Escape, 0 = none.
	int kbm_button_hotkey(const card_button_t& b) {
		if (b.action == cardact_train || b.action == cardact_build || b.action == cardact_morph) {
			if (const ext_unit_def* d = ext_def((UnitTypes)b.arg)) {
				int c = d->hotkey.empty() ? 0 : (unsigned char)d->hotkey[0];
				if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
				return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : 0;
			}
		}
		if (b.action == cardact_menu_mods) return 'o';   // free on every worker card
		if (b.action == cardact_menu_abilities) return 'q';
		if (b.action == cardact_ext_learn_menu) return 'l';
		if (ext_card_ability(b) >= 0) {
			const ext_ability_def& d = game_st.ext_abilities[(size_t)ext_card_ability(b)];
			int c = d.hotkey.empty() ? 0 : (unsigned char)d.hotkey[0];
			if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
			return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : 0;
		}
		const int t = kbm_button_tbl(b.action, b.arg, b.icon);
		if (!t) return 0;
		load_unit_names();
		if (!unit_names_ok) return 0;
		a_string s;
		try { s = unit_names_tbl[(size_t)t]; } catch (...) { return 0; }
		if (s.empty()) return 0;
		int c = (unsigned char)s[0];
		if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
		return (c >= 'a' && c <= 'z') || c == 0x1b ? c : 0;
	}
	// The key that activates card slot i holding b; -1 = none.
	int kbm_card_code(const card_button_t& b, int i) {
		if (b.action == cardact_none) return -1;
		if (!kbm_classic) return i >= 0 && i <= 8 ? kbm_bind[kb_card0 + i] : -1;
		if (b.action == cardact_cancel || b.action == cardact_menu_back || b.action == cardact_place_cancel) return kbm_bind[kb_cancel];
		if (!kbm_cmd_keys.empty()) {
			auto it = kbm_cmd_keys.find(kbm_button_tbl(b.action, b.arg, b.icon));
			if (it != kbm_cmd_keys.end()) return it->second;
		}
		const int c = kbm_button_hotkey(b);
		if (c == 0x1b) return kbm_bind[kb_cancel];
		return c ? 4 + (c - 'a') : -1;
	}
	// Classic: the card slot answering to scancode code (first match on the card), -1 = none.
	int kbm_classic_slot(int code) {
		if (!kbm_classic) return -1;
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		for (size_t i = 0; i != buttons.size(); ++i) {
			if (!buttons[i].available) continue;
			if (kbm_card_code(buttons[i], (int)i) == code) return (int)i;
		}
		return -1;
	}

	bool kbm_player(const a_string& name) const {
		for (auto& n : kbm_players) if (n == name) return true;
		return false;
	}

	bool kbm_shift() { return wnd.get_key_state(225) || wnd.get_key_state(229); }
	bool kbm_ctrl() { return wnd.get_key_state(224) || wnd.get_key_state(228); }

	// A key (or a mouse button outside the select / command / pan roles) went down or up.
	// Returns true when it was one of ours.
	bool kbm_key(const native_window::event_t& e, bool down) {
		if (!kbm_mode || !game_input || e.scancode < 0) return false;
		// Classic: a letter on the current card wins over any other use of the key.
		if (down && kbm_classic && pause_screen == 0 && !briefing_active && !lobby_mode) {
			const int slot = kbm_classic_slot(e.scancode);
			if (slot >= 0) {
				if (!e.repeat) {
					if (hud_v3 && wheel_open) hud3_close_wheel(true);
					card_activate(slot);
				}
				return true;
			}
		}
		bool any = false;
		for (int a = 0; a != kb_count; ++a) {
			if (kbm_classic && a >= kb_card0 && a <= kb_card8) continue;
			if (kbm_bind[a] != e.scancode) continue;
			any = true;
			kbm_held[a] = down;
			if (down && !e.repeat) kbm_action(a);
		}
		return any;
	}

	void kbm_release_all() { kbm_held.fill(false); }

	void kbm_action(int a) {
		if (a == kb_menu) {
			if (pause_screen == 0) open_pause_menu(); else close_pause_menu();
			return;
		}
		if (pause_screen != 0) {
			if (a == kb_cancel) { if (pause_screen > 1 && pause_screen != 10) pause_screen = 1; else close_pause_menu(); }
			return;
		}
		if (briefing_active || lobby_mode) return;
		if (a >= kb_card0 && a <= kb_card8) {
			if (hud_v3 && wheel_open) hud3_close_wheel(true);
			card_activate(a - kb_card0);
			return;
		}
		if (a >= kb_group0 && a <= kb_group9) {
			const int n = a - kb_group0;
			if (kbm_ctrl()) { assign_control_group(n); kbm_last_group = -1; return; }
			if (kbm_shift()) { append_control_group(n); kbm_last_group = -1; return; }
			const auto now = std::chrono::steady_clock::now();
			const bool again = kbm_last_group == n && now - kbm_last_group_t < std::chrono::milliseconds(450);
			kbm_last_group = n;
			kbm_last_group_t = now;
			recall_control_group(n);
			if (again) if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position);
			return;
		}
		if (a >= kb_camloc0 && a <= kb_camloc2) {
			xy& c = kbm_camloc[(size_t)(a - kb_camloc0)];
			if (kbm_ctrl() || kbm_shift()) c = screen_pos;
			else if (c.x >= 0) screen_pos = c;
			return;
		}
		switch (a) {
		case kb_cancel:
			if (hud_v3 && wheel_open) hud3_close_wheel(false);
			else if (placing_type || attack_armed || ability_armed_order >= 0) hud3_cancel_targeting();
			else if (group_menu_n >= 0) group_menu_close();
			else if (card_menu != 0) card_menu = 0;
			break;
		case kb_chat: if (!chat_active) open_chat(); break;
		case kb_alert: if (alert_pos.x >= 0) center_camera_on(alert_pos); break;
		case kb_idle: select_next_idle_worker(); break;
		case kb_army: select_army_cluster(false); break;
		case kb_center: if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position); break;
		case kb_zoom_in: set_zoom(zoom_level + 1); break;
		case kb_zoom_out: set_zoom(zoom_level - 1); break;
		case kb_subgroup: cycle_subgroup(kbm_shift() ? -1 : 1); break;
		default: break;
		}
	}

	// The camera keys and the screen edge, as a direction (-1 / 0 / 1 per axis).
	xy kbm_scroll_dir() {
		xy d(0, 0);
		if (!kbm_mode || !game_input || pause_screen != 0 || chat_active || save_name_active) return d;
		if (kbm_held[kb_cam_down]) d.y = 1; else if (kbm_held[kb_cam_up]) d.y = -1;
		if (kbm_held[kb_cam_right]) d.x = 1; else if (kbm_held[kb_cam_left]) d.x = -1;
		int mx = 0, my = 0;
		if (kbm_edge_scroll && wnd.get_mouse_pointer(&mx, &my)) {
			const int m = std::max(2, (int)screen_height / 160);
			if (mx < m) d.x = -1; else if (mx >= (int)screen_width - m) d.x = 1;
			if (my < m) d.y = -1; else if (my >= (int)screen_height - m) d.y = 1;
		}
		return d;
	}

	static void kbm_key_name(int code, char* out, size_t n) {
		static const char punct[] = "-=[]\#;'`,./";
		if (code > kb_mouse) snprintf(out, n, "M%d", code - kb_mouse);
		else if (code >= 4 && code <= 29) snprintf(out, n, "%c", 'A' + code - 4);
		else if (code >= 30 && code <= 39) snprintf(out, n, "%c", code == 39 ? '0' : '1' + code - 30);
		else if (code >= 45 && code <= 56) snprintf(out, n, "%c", punct[code - 45]);
		else if (code >= 58 && code <= 69) snprintf(out, n, "F%d", code - 57);
		else if (code == 40) snprintf(out, n, "Ent");
		else if (code == 41) snprintf(out, n, "Esc");
		else if (code == 43) snprintf(out, n, "Tab");
		else if (code == 44) snprintf(out, n, "Spc");
		else if (code >= 79 && code <= 82) snprintf(out, n, "%s", code == 79 ? ">" : code == 80 ? "<" : code == 81 ? "v" : "^");
		else snprintf(out, n, "#%d", code);
	}
	// Shortcut of command-card slot i (holding b), top-right inside its cell.
	void kbm_draw_card_key(uint32_t* data, size_t pitch, rect r, const card_button_t& b, int i, int pct) {
		if (!kbm_mode || i < 0 || i > 8) return;
		const int code = kbm_card_code(b, i);
		if (code < 0) return;
		char k[8]; kbm_key_name(code, k, sizeof k);
		const int p = hud_text_pct(pct);
		const int adv = (int)((ui::unispace_w + 1) * p / 100.0f + 0.5f), th = ui::unispace_h * p / 100;
		const int pad = std::max(1, th / 6);
		const int w = (int)strlen(k) * adv + 2 * pad;
		const int x = r.to.x - w - 1, y = r.from.y + 1;
		blend_rect_rgba(data, pitch, {{x, y}, {x + w, y + th + 2 * pad}}, 0x000000u, 150);
		draw_text_rgba_scaled(data, pitch, x + pad, y + pad, k, 0xffffd060u, pct);
	}

	// The keyboard + mouse flag: a small orange keyboard, h px tall, left edge at x.
	// Returns its width.
	int draw_kbm_flag(uint32_t* data, size_t pitch, int x, int y, int h) {
		if (h < 7) h = 7;
		const int w = h * 8 / 5;
		const uint32_t body = 0xffff8a1eu, key = 0xff3a1c04u;
		fill_rect_rgba(data, pitch, {{x + 1, y}, {x + w - 1, y + h}}, body);
		fill_rect_rgba(data, pitch, {{x, y + 1}, {x + w, y + h - 1}}, body);
		const int u = std::max(1, h / 7);
		const int kx0 = x + 2 * u, kw = w - 4 * u;
		for (int row = 0; row != 2; ++row) {
			const int ky = y + u * (1 + 2 * row) + (h - 7 * u) / 2;
			for (int k = 0; k != 5; ++k) {
				const int px = kx0 + k * kw / 5;
				fill_rect_rgba(data, pitch, {{px, ky}, {px + std::max(1, kw / 5 - u), ky + u}}, key);
			}
		}
		const int sy = y + 5 * u + (h - 7 * u) / 2;
		fill_rect_rgba(data, pitch, {{x + w / 4, sy}, {x + w - w / 4, sy + u}}, key);
		return w;
	}
