// Keyboard + mouse play (Settings > Valdymas > "Įvesties režimas").
//
// Included INSIDE struct ui_functions (ui.h), after hud3.h. With kbm_mode off
// nothing here runs and touch play is unchanged. Every action goes through the
// existing paths (card_activate, recall_control_group, center_camera_on, ...):
// no new game actions, the simulation and replays see what a tap would send.
//
// Bindings arrive from the launcher as --key-binds "id=code,id=code". A code is an
// SDL scancode, or native_window::event_t::mouse_key_base + an SDL mouse button.

	enum {
		kb_cam_up, kb_cam_down, kb_cam_left, kb_cam_right,
		kb_card0, kb_card8 = kb_card0 + 8,
		kb_cancel, kb_menu, kb_chat, kb_alert, kb_idle, kb_army, kb_center, kb_zoom_in, kb_zoom_out,
		kb_group0, kb_group9 = kb_group0 + 9,
		kb_camloc0, kb_camloc2 = kb_camloc0 + 2,
		kb_mouse_select, kb_mouse_command, kb_mouse_pan,
		kb_count
	};
	static constexpr int kb_mouse = native_window::event_t::mouse_key_base;

	bool kbm_mode = false;
	bool kbm_edge_scroll = true;
	std::array<int, kb_count> kbm_bind = kbm_default_binds();
	std::array<bool, kb_count> kbm_held{};
	std::array<xy, 3> kbm_camloc{{xy(-1, -1), xy(-1, -1), xy(-1, -1)}};
	int kbm_last_group = -1;
	std::chrono::steady_clock::time_point kbm_last_group_t;
	// LAN: the players whose launcher runs in keyboard + mouse mode (--kbm-players).
	a_vector<a_string> kbm_players;

	static std::array<int, kb_count> kbm_default_binds() {
		std::array<int, kb_count> b{};
		b[kb_cam_up] = 82; b[kb_cam_down] = 81; b[kb_cam_left] = 80; b[kb_cam_right] = 79;
		static const int card[9] = {20, 26, 8, 4, 22, 7, 29, 27, 6};   // Q W E / A S D / Z X C
		for (int i = 0; i != 9; ++i) b[kb_card0 + i] = card[i];
		b[kb_cancel] = 41; b[kb_menu] = 67; b[kb_chat] = 40; b[kb_alert] = 44;
		b[kb_idle] = 58; b[kb_army] = 59; b[kb_center] = 9; b[kb_zoom_in] = 46; b[kb_zoom_out] = 45;
		for (int i = 0; i != 10; ++i) b[kb_group0 + i] = i == 0 ? 39 : 29 + i;
		for (int i = 0; i != 3; ++i) b[kb_camloc0 + i] = 62 + i;   // F5 F6 F7
		b[kb_mouse_select] = kb_mouse + 1; b[kb_mouse_command] = kb_mouse + 3; b[kb_mouse_pan] = kb_mouse + 2;
		return b;
	}

	static a_string kbm_id(int a) {
		static const char* const fixed[] = {"cam_up", "cam_down", "cam_left", "cam_right"};
		if (a < kb_card0) return fixed[a];
		if (a <= kb_card8) return "card" + std::to_string(a - kb_card0 + 1);
		static const char* const mid[] = {"cancel", "menu", "chat", "alert", "idle", "army", "center", "zoom_in", "zoom_out"};
		if (a < kb_group0) return mid[a - kb_cancel];
		if (a <= kb_group9) return "group" + std::to_string(a - kb_group0);
		if (a <= kb_camloc2) return "camloc" + std::to_string(a - kb_camloc0 + 1);
		static const char* const mouse[] = {"mouse_select", "mouse_command", "mouse_pan"};
		return mouse[a - kb_mouse_select];
	}

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
			for (int a = 0; a != kb_count; ++a) if (kbm_id(a) == id) { kbm_bind[a] = code; break; }
		}
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
		bool any = false;
		for (int a = 0; a != kb_count; ++a) {
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
			if (a == kb_cancel) { if (pause_screen > 1) pause_screen = 1; else close_pause_menu(); }
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
	// Shortcut of command-card slot i, top-right inside its cell.
	void kbm_draw_card_key(uint32_t* data, size_t pitch, rect r, int i, int pct) {
		if (!kbm_mode || i < 0 || i > 8) return;
		char k[8]; kbm_key_name(kbm_bind[kb_card0 + i], k, sizeof k);
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
