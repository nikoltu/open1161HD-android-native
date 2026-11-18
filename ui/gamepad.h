// Game controller play (Settings > Valdymas > "Žaidimų pultas").
//
// Included INSIDE struct ui_functions (ui.h), after kbm.h. Input only: the pad
// drives a virtual cursor and turns its buttons into the same pointer events and
// card / control-group calls a mouse or a tap produces, so the simulation and
// replays see nothing new. Button / axis numbers follow SDL_GameController.

	enum {
		pad_a, pad_b, pad_x, pad_y, pad_back, pad_guide, pad_start, pad_l3, pad_r3, pad_l1, pad_r1,
		pad_up, pad_down, pad_left, pad_right, pad_buttons
	};
	enum { pad_lx, pad_ly, pad_rx, pad_ry, pad_lt, pad_rt, pad_axes };
	// What a button does (Settings > Controls, picked from a list; --pad-map). The order is the
	// launcher's PadBinds.ACTIONS order: never renumber, only append.
	enum {
		pact_none, pact_select, pact_command, pact_attack, pact_stop, pact_free_group, pact_cancel, pact_menu,
		pact_idle_worker, pact_next_base, pact_group_prev, pact_group_next, pact_center, pact_army, pact_precise,
		pact_alert, pact_zoom_in, pact_zoom_out, pact_count
	};
	enum { ptrig_none, ptrig_wheel, ptrig_card, ptrig_count };
	std::array<uint8_t, pad_buttons> pad_map{{
		pact_select, pact_command, pact_attack, pact_free_group, pact_cancel, pact_none, pact_menu, pact_precise,
		pact_stop, pact_idle_worker, pact_next_base, pact_center, pact_army, pact_group_prev, pact_group_next}};
	std::array<uint8_t, 2> pad_trig_map{{ptrig_card, ptrig_wheel}};   // LT, RT
	// "a,b,..." -- pad_buttons button actions, then LT and RT; anything malformed keeps the default.
	void pad_set_map(const std::string& m) {
		std::array<int, pad_buttons + 2> v{};
		size_t n = 0, i = 0;
		while (n < v.size() && i <= m.size()) {
			size_t j = m.find(',', i);
			if (j == std::string::npos) j = m.size();
			if (j == i) return;
			v[n++] = atoi(m.substr(i, j - i).c_str());
			i = j + 1;
		}
		if (n != v.size()) return;
		for (size_t k = 0; k != pad_buttons; ++k) if (v[k] < 0 || v[k] >= pact_count) return;
		for (size_t k = 0; k != 2; ++k) if (v[pad_buttons + k] < 0 || v[pad_buttons + k] >= ptrig_count) return;
		for (size_t k = 0; k != pad_buttons; ++k) pad_map[k] = (uint8_t)v[k];
		for (size_t k = 0; k != 2; ++k) pad_trig_map[k] = (uint8_t)v[pad_buttons + k];
	}
	bool pad_action_held(int act) const {
		for (size_t k = 0; k != pad_buttons; ++k) if (pad_map[k] == act && pad_held[k]) return true;
		return false;
	}
	bool pad_trig_held(int what) const {
		return (pad_trig_map[0] == what && pad_lt_down) || (pad_trig_map[1] == what && pad_rt_down);
	}

	bool pad_enabled = true;
	int pad_speed = 100;           // cursor speed, % of the base rate
	int pad_dead = 18;             // stick dead zone, % of full tilt
	bool pad_swap = false;         // right stick moves the cursor, left one the camera
	bool pad_active = false;       // the cursor is shown (a pad was used since the last touch)
	float pad_cx = -1, pad_cy = -1;
	float pad_pan_x = 0, pad_pan_y = 0;
	std::array<bool, pad_buttons> pad_held{};
	std::array<float, pad_axes> pad_axis{};
	bool pad_lt_down = false, pad_rt_down = false;
	int pad_group = 0;
	int pad_last_group = -1;
	std::chrono::steady_clock::time_point pad_last_group_t, pad_last_tick, pad_dpad_t;
	int pad_base_cycle = 0, pad_group_dir = 0;
	bool pad_wheel = false;
	bool pad_a_down = false, pad_a_dragged = false;
	float pad_a_x = 0, pad_a_y = 0;
	std::chrono::steady_clock::time_point pad_a_t;
	bool pad_click_armed = false;  // the last A release can become a double click
	std::chrono::steady_clock::time_point pad_click_t;
	float pad_click_x = 0, pad_click_y = 0;
	std::deque<native_window::event_t> pad_queue;
	bool pad_injected = false;     // the event being handled came from pad_queue

	bool pad_next(native_window::event_t& e) {
		pad_injected = false;
		if (pad_queue.empty()) return false;
		e = pad_queue.front();
		pad_queue.pop_front();
		pad_injected = true;
		return true;
	}

	void pad_push(int type, int button_state, int clicks = 1) {
		native_window::event_t e;
		e.type = type;
		e.button = type == native_window::event_t::type_mouse_motion ? -1 : 1;
		e.mouse_x = (int)pad_cx;
		e.mouse_y = (int)pad_cy;
		e.mouse_xrel = 0;
		e.mouse_yrel = 0;
		e.button_state = button_state;
		e.clicks = clicks;
		e.device = 1;   // the select button: A selects, B orders
		pad_queue.push_back(e);
	}

	void pad_reset() {
		if (pad_a_down) pad_push(native_window::event_t::type_mouse_button_up, 0);
		pad_a_down = false;
		pad_held.fill(false);
		pad_axis.fill(0.0f);
		pad_lt_down = pad_rt_down = false;
		pad_click_armed = false;
		pad_active = false;
	}

	void pad_wake() {
		if (!pad_active || pad_cx < 0) {
			if (pad_cx < 0 || pad_cx >= (float)screen_width || pad_cy >= (float)screen_height) {
				pad_cx = screen_width / 2.0f;
				pad_cy = screen_height / 2.0f;
			}
		}
		pad_active = true;
	}

	int pad_card_index(int action) {
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		for (size_t i = 0; i != buttons.size(); ++i) if (buttons[i].action == action) return (int)i;
		return -1;
	}

	void pad_recall(int n, bool repeat_centres) {
		const auto now = std::chrono::steady_clock::now();
		const bool again = repeat_centres && pad_last_group == n && now - pad_last_group_t < std::chrono::milliseconds(450);
		pad_last_group = n;
		pad_last_group_t = now;
		recall_control_group(n);
		if (again) if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position);
	}

	// LT held turns the 3x3 command card into buttons: top row X Y B, middle row the
	// D-pad left / up / right, bottom row A, D-pad down, RB (labels drawn on the card).
	static int pad_card_slot(int b) {
		switch (b) {
		case pad_x: return 0; case pad_y: return 1; case pad_b: return 2;
		case pad_left: return 3; case pad_up: return 4; case pad_right: return 5;
		case pad_a: return 6; case pad_down: return 7; case pad_r1: return 8;
		default: return -1;
		}
	}
	static const char* pad_slot_label(int slot) {
		static const char* l[9] = {"X", "Y", "B", "<", "^", ">", "A", "v", "RB"};
		return slot >= 0 && slot < 9 ? l[slot] : "";
	}
	bool pad_card_mode() const { return pad_enabled && pad_trig_held(ptrig_card) && game_input && pause_screen == 0; }

	// RB: the next own town hall, selected and centred.
	void pad_next_base() {
		if (game_local_player < 0) return;
		a_vector<unit_t*> halls;
		for (unit_t* u : ptr(st.player_units[game_local_player])) {
			if (u->owner != game_local_player || !u_completed(u)) continue;
			auto id = u->unit_type->id;
			if (id == UnitTypes::Terran_Command_Center || id == UnitTypes::Protoss_Nexus || id == UnitTypes::Zerg_Hatchery
				|| id == UnitTypes::Zerg_Lair || id == UnitTypes::Zerg_Hive) halls.push_back(u);
		}
		if (halls.empty()) return;
		if (pad_base_cycle >= (int)halls.size()) pad_base_cycle = 0;
		unit_t* u = halls[(size_t)pad_base_cycle++];
		current_selection_clear();
		current_selection_add(u);
		play_unit_what(u);
		center_camera_on(u->position);
	}

	// Y: the selection goes into the first empty group (1..9, then 0); every group full: the current one.
	void pad_assign_free_group() {
		if (!first_own_selected_unit()) return;
		int n = -1;
		for (int k = 1; k <= 10 && n < 0; ++k) if (control_group_live_count(k % 10) == 0) n = k % 10;
		if (n < 0) n = pad_group;
		assign_control_group(n);
		pad_group = n;
		char b[64];
		snprintf(b, sizeof b, "%s: %d", L("Group assigned"), n);
		show_hud_message(b);
	}

	// D-pad left / right: the previous / next non-empty group; the same direction again
	// within 450 ms centres the camera on it.
	void pad_cycle_group(int dir) {
		if (!game_input || pause_screen != 0) return;
		const auto now = std::chrono::steady_clock::now();
		const bool again = dir == pad_group_dir && now - pad_dpad_t < std::chrono::milliseconds(450);
		pad_dpad_t = now;
		pad_group_dir = dir;
		if (again && control_group_live_count(pad_group) > 0) {
			pad_recall(pad_group, false);
			if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position);
			return;
		}
		int n = -1;
		for (int k = 1; k <= 10 && n < 0; ++k) { int g = (pad_group + dir * k + 20) % 10; if (control_group_live_count(g) > 0) n = g; }
		if (n < 0) return;
		pad_group = n;
		pad_recall(n, false);
	}

	// RT: the command wheel (touch HUD) at the cursor while held; the right stick points at a
	// slot, letting go does it. A second ring (build lists) stays up: RT again or A picks there.
	xy pad_wheel_point() {
		const float sx = pad_axis[pad_rx], sy = pad_axis[pad_ry];
		const float m = std::sqrt(sx * sx + sy * sy);
		if (m < 0.45f) return wheel_c;
		const float r = (float)d3((wr_in + wr_out) / 2);
		return xy(wheel_c.x + (int)(sx / m * r), wheel_c.y + (int)(sy / m * r));
	}
	void pad_wheel_trigger(bool down) {
		if (!hud_v3 || !game_input || pause_screen != 0) return;
		if (down) {
			if (wheel_open) { pad_wheel = true; return; }
			pad_wheel = hud3_open_wheel_at((int)pad_cx, (int)pad_cy, 0) && wheel_open;
			if (pad_wheel) wheel_pt = xy(-100000, -100000);   // the stick, not the cursor spot, makes the hub
			return;
		}
		if (!wheel_open || !pad_wheel) return;
		const xy p = pad_wheel_point();
		hud3_wheel_release(p.x, p.y);
		pad_wheel = wheel_open;
	}
	void pad_wheel_tick() {
		if (!wheel_open) { pad_wheel = false; return; }
		if (!pad_wheel) return;
		const xy p = pad_wheel_point();
		hud3_wheel_motion(p.x, p.y);
	}

	void pad_button(int b, bool down) {
		using ev = native_window::event_t;
		if (b < 0 || b >= pad_buttons) return;
		pad_held[(size_t)b] = down;
		const int act = pad_map[(size_t)b];
		if (pad_wheel && wheel_open && down && (act == pact_select || act == pact_command || act == pact_cancel)) {
			if (act != pact_select) { hud3_close_wheel(false); pad_wheel = false; return; }
			const xy p = pad_wheel_point();
			hud3_wheel_release(p.x, p.y);
			pad_wheel = wheel_open;
			return;
		}
		if (pad_card_mode()) {
			const int slot = pad_card_slot(b);
			if (slot >= 0) {
				if (down) {
					static_vector<card_button_t, 9> buttons;
					card_buttons(buttons);
					if (slot < (int)buttons.size() && buttons[(size_t)slot].action != cardact_none) card_activate(slot);
				}
				return;
			}
		}
		if (act == pact_select) {
			if (down) {
				if (pad_a_down) return;
				const auto now = std::chrono::steady_clock::now();
				const bool dbl = pad_click_armed && now - pad_click_t <= std::chrono::milliseconds(300) &&
					std::abs(pad_cx - pad_click_x) <= 6 && std::abs(pad_cy - pad_click_y) <= 6;
				pad_a_down = true;
				pad_a_dragged = false;
				pad_a_x = pad_cx; pad_a_y = pad_cy;
				pad_a_t = now;
				pad_push(ev::type_mouse_button_down, 1, dbl ? 2 : 1);
				pad_click_armed = dbl;   // a held second press keeps its double-click count on release
			} else if (pad_a_down) {
				const auto now = std::chrono::steady_clock::now();
				pad_a_down = false;
				const bool tap = !pad_a_dragged && now - pad_a_t <= std::chrono::milliseconds(300);
				pad_push(ev::type_mouse_button_up, 0, pad_click_armed && tap ? 2 : 1);
				if (pad_click_armed) pad_click_armed = false;
				else if (tap) { pad_click_armed = true; pad_click_t = now; pad_click_x = pad_cx; pad_click_y = pad_cy; }
			}
			return;
		}
		if (!down) return;
		const bool play = game_input && pause_screen == 0;
		switch (act) {
		case pact_command: pad_push(ev::type_command_click, 0); break;
		case pact_attack:
			if (play && !attack_armed) { int i = pad_card_index(cardact_attack); if (i >= 0) card_activate(i); }
			break;
		case pact_stop:
			if (play) { int i = pad_card_index(cardact_stop); if (i >= 0) card_activate(i); }
			break;
		case pact_free_group: if (play) pad_assign_free_group(); else kbm_action(kb_cancel); break;
		case pact_cancel: kbm_action(kb_cancel); break;
		case pact_menu: kbm_action(kb_menu); break;
		case pact_idle_worker: if (play) select_next_idle_worker(); break;
		case pact_next_base: if (play) pad_next_base(); break;
		case pact_group_prev: pad_cycle_group(-1); break;
		case pact_group_next: pad_cycle_group(1); break;
		case pact_center: if (play) if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position); break;
		case pact_army: if (play) select_army(); break;
		case pact_alert: if (play) kbm_action(kb_alert); break;
		case pact_zoom_in: kbm_action(kb_zoom_in); break;
		case pact_zoom_out: kbm_action(kb_zoom_out); break;
		default: break;
		}
	}


	// A controller event from the window. Returns true when it was one (consumed).
	// Real touches and mouse events hide the cursor.
	bool pad_event(const native_window::event_t& e) {
		using ev = native_window::event_t;
		switch (e.type) {
		case ev::type_pad_button_down:
		case ev::type_pad_button_up:
			if (pad_enabled) {
				pad_wake();
				pad_button(e.button, e.type == ev::type_pad_button_down);
			}
			return true;
		case ev::type_pad_axis:
			if (pad_enabled && e.button >= 0 && e.button < pad_axes) {
				const int ax = pad_swap && e.button <= pad_ry ? e.button ^ 2 : e.button;   // swap: lx <-> rx, ly <-> ry
				const float v = e.mouse_x / 32767.0f;
				pad_axis[(size_t)ax] = v < -1 ? -1 : v > 1 ? 1 : v;
				if (e.button == pad_lt || e.button == pad_rt) {
					bool& was = e.button == pad_lt ? pad_lt_down : pad_rt_down;
					const bool now_down = v > 0.5f;
					if (now_down != was) {
						pad_wake();
						if (pad_trig_map[e.button == pad_lt ? 0 : 1] == ptrig_wheel) pad_wheel_trigger(now_down);
					}
					was = now_down;
				} else if (std::abs(v) > pad_dead / 100.0f) pad_wake();
			}
			return true;
		case ev::type_pad_reset:
			pad_reset();
			return true;
		case ev::type_mouse_button_down:
		case ev::type_touch_down:
		case ev::type_long_press:
			if (!pad_injected && pad_active) {
				if (pad_a_down) pad_reset();
				pad_active = false;
			}
			return false;
		default:
			return false;
		}
	}

	// Dead zone (pad_dead), rescaled, cubic response.
	float pad_curve(float v) const {
		const float a = std::abs(v), dz = pad_dead / 100.0f;
		if (a <= dz) return 0.0f;
		float t = (a - dz) / (1.0f - dz);
		if (t > 1) t = 1;
		t = 0.4f * t + 0.6f * t * t * t;
		return v < 0 ? -t : t;
	}

	// Once per frame, before the window events: moves the cursor (left stick) and the
	// camera (right stick / D-pad).
	void pad_tick() {
		const auto now = std::chrono::steady_clock::now();
		float dt = std::chrono::duration<float>(now - pad_last_tick).count();
		pad_last_tick = now;
		if (!pad_enabled || !pad_active) return;
		if (dt > 0.1f) dt = 0.1f;
		if (dt <= 0) return;
		const float precision = pad_action_held(pact_precise) ? 0.25f : 1.0f;
		const float speed = 360.0f * (float)screen_height / 480.0f * (float)pad_speed / 100.0f * precision;
		const float mx = pad_curve(pad_axis[pad_lx]), my = pad_curve(pad_axis[pad_ly]);
		if (mx != 0 || my != 0) {
			pad_cx += mx * speed * dt;
			pad_cy += my * speed * dt;
			if (pad_cx < 0) pad_cx = 0;
			if (pad_cy < 0) pad_cy = 0;
			if (pad_cx > (float)screen_width - 1) pad_cx = (float)screen_width - 1;
			if (pad_cy > (float)screen_height - 1) pad_cy = (float)screen_height - 1;
			if (pad_a_down && (std::abs(pad_cx - pad_a_x) > 6 || std::abs(pad_cy - pad_a_y) > 6)) pad_a_dragged = true;
			if (pad_click_armed && (std::abs(pad_cx - pad_click_x) > 6 || std::abs(pad_cy - pad_click_y) > 6)) pad_click_armed = false;
			pad_push(native_window::event_t::type_mouse_motion, pad_a_down ? 1 : 0, 0);
		}
		if (!game_input || pause_screen != 0 || chat_active || save_name_active || briefing_active || lobby_mode) return;
		pad_wheel_tick();
		if (pad_wheel) { pad_pan_x = pad_pan_y = 0; return; }   // the right stick aims the wheel
		float px = pad_curve(pad_axis[pad_rx]), py = pad_curve(pad_axis[pad_ry]);
		if (px == 0 && py == 0) { pad_pan_x = pad_pan_y = 0; return; }
		pad_click_armed = false;
		const float scale = view_scale.raw_value > 0 ? (float)view_scale.raw_value / 65536.0f : 1.0f;
		const float pan = 480.0f * (float)screen_height / 480.0f * (float)camera_speed / 100.0f / scale;
		pad_pan_x += px * pan * dt;
		pad_pan_y += py * pan * dt;
		const int sx = (int)pad_pan_x, sy = (int)pad_pan_y;
		pad_pan_x -= sx;
		pad_pan_y -= sy;
		screen_pos.x += sx;
		screen_pos.y += sy;
	}

	// An arrow cursor at the virtual pointer, only while the pad is in use.
	// The game's own cursors (cursor/*.grp): the arrow, the target cross while an order waits
	// for a target, the magnifier over a unit (green own, red enemy, yellow neutral).
	enum { pcur_arrow, pcur_target, pcur_mag_own, pcur_mag_enemy, pcur_mag_neutral, pcur_count };
	std::array<grp_t, pcur_count> pad_cursor_grp;
	bool pad_cursor_tried = false, pad_cursor_ok = false;
	a_vector<uint8_t> pad_cursor_scratch;

	void pad_load_cursors() {
		if (pad_cursor_tried) return;
		pad_cursor_tried = true;
		if (!load_data_file) return;
		static const char* names[pcur_count] = {"cursor\\arrow.grp", "cursor\\targg.grp", "cursor\\magg.grp", "cursor\\magr.grp", "cursor\\magy.grp"};
		for (int i = 0; i < pcur_count; ++i) {
			try {
				a_vector<uint8_t> buf;
				load_data_file(buf, names[i]);
				if (!buf.empty()) pad_cursor_grp[(size_t)i] = read_grp_tolerant(buf.data(), buf.size());
			} catch (...) {}
		}
		pad_cursor_ok = !pad_cursor_grp[pcur_arrow].frames.empty();
	}

	// SC:R cursors (cursor/*.dds.grp, uncompressed RGBA): each frame is the SD frame's
	// rectangle at 2x (HD2) or 4x (FULL), so the SD grp keeps giving the hotspot.
	std::array<ddsgrp::file_t, pcur_count> pad_cursor_hd;
	bool pad_cursor_hd_tried = false;
	std::map<std::pair<int, int>, std::vector<uint32_t>> pad_cursor_hd_cache;   // (kind << 8 | frame, scale)

	void pad_load_hd_cursors() {
		if (pad_cursor_hd_tried) return;
		pad_cursor_hd_tried = true;
		if (!hd_casc || !load_hd_bytes) return;
		static const char* names[pcur_count] = {"cursor/arrow.dds.grp", "cursor/targg.dds.grp", "cursor/magg.dds.grp", "cursor/magr.dds.grp", "cursor/magy.dds.grp"};
		for (int i = 0; i < pcur_count; ++i) {
			std::vector<uint8_t> tmp;
			if (!load_hd_bytes(hd_casc_prefix + names[i], tmp) || tmp.empty()) continue;
			ddsgrp::file_t f = ddsgrp::parse(tmp.data(), tmp.size());
			if (f.ok && f.frame_count() == pad_cursor_grp[(size_t)i].frames.size()) pad_cursor_hd[(size_t)i] = std::move(f);
		}
	}

	// The HD frame area-averaged to dw x dh (alpha-weighted); null when there is none.
	const std::vector<uint32_t>* pad_hd_cursor_frame(int kind, size_t frame, int dw, int dh, int sc) {
		pad_load_hd_cursors();
		const ddsgrp::file_t& f = pad_cursor_hd[(size_t)kind];
		if (!f.ok || frame >= f.frame_count()) return nullptr;
		auto key = std::make_pair(kind << 8 | (int)frame, sc);
		auto it = pad_cursor_hd_cache.find(key);
		if (it != pad_cursor_hd_cache.end()) return it->second.empty() ? nullptr : &it->second;
		std::vector<uint32_t>& out = pad_cursor_hd_cache[key];
		ddsgrp::frame_t src = f.decode(frame);
		if (src.w <= 0 || src.h <= 0 || dw <= 0 || dh <= 0) return nullptr;
		out.assign((size_t)dw * dh, 0);
		for (int y = 0; y != dh; ++y) {
			const int y0 = y * src.h / dh, y1 = std::max(y0 + 1, (y + 1) * src.h / dh);
			for (int x = 0; x != dw; ++x) {
				const int x0 = x * src.w / dw, x1 = std::max(x0 + 1, (x + 1) * src.w / dw);
				uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
				for (int sy = y0; sy < y1 && sy < src.h; ++sy) for (int sx = x0; sx < x1 && sx < src.w; ++sx) {
					const uint32_t c = src.rgba[(size_t)sy * src.w + sx], ca = c >> 24;
					a += ca; r += ((c >> 16) & 0xff) * ca; g += ((c >> 8) & 0xff) * ca; b += (c & 0xff) * ca; ++n;
				}
				if (!n || !a) continue;
				out[(size_t)y * dw + x] = (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a);
			}
		}
		return &out;
	}

	int pad_cursor_kind(int cx, int cy) {
		if (attack_armed || ability_armed_order >= 0) return pcur_target;
		if (!game_input || pause_screen != 0 || cy >= playfield_bottom_y() || hud_control_at(cx, cy)) return pcur_arrow;
		unit_t* u = select_get_unit_at(screen_to_world(cx, cy));
		if (!u) return pcur_arrow;
		if (u->owner == game_local_player) return pcur_mag_own;
		if (u->owner >= 8 || game_local_player < 0) return pcur_mag_neutral;
		return st.alliances[game_local_player][u->owner] ? pcur_mag_own : pcur_mag_enemy;
	}

	bool pad_draw_game_cursor(uint32_t* data, size_t pitch) {
		return draw_game_cursor_at(data, pitch, (int)pad_cx, (int)pad_cy, pad_action_held(pact_precise));
	}
	// The game's own animated cursor (arrow / target / magnifier) at x, y.
	bool draw_game_cursor_at(uint32_t* data, size_t pitch, int cx, int cy, bool slow) {
		pad_load_cursors();
		if (!pad_cursor_ok) return false;
		int kind = pad_cursor_kind(cx, cy);
		if (pad_cursor_grp[(size_t)kind].frames.empty()) kind = pcur_arrow;
		const grp_t& g = pad_cursor_grp[(size_t)kind];
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const size_t fi = (size_t)((ms / 100) % (long long)g.frames.size());
		const auto& f = g.frames[fi];
		const size_t fw = f.size.x, fh = f.size.y;
		if (fw == 0 || fh == 0) return true;
		{
			const int sc = std::max(1, ((int)screen_height + 240) / 480);
			const int dw = (int)fw * sc, dh = (int)fh * sc;
			if (const std::vector<uint32_t>* hd = pad_hd_cursor_frame(kind, fi, dw, dh, sc)) {
				const int ox = cx + ((int)f.offset.x - (int)g.width / 2) * sc;
				const int oy = cy + ((int)f.offset.y - (int)g.height / 2) * sc;
				for (int y = 0; y != dh; ++y) {
					const int py = oy + y;
					if (py < 0 || py >= (int)screen_height) continue;
					uint32_t* row = data + (size_t)py * pitch;
					const uint32_t* srow = hd->data() + (size_t)y * dw;
					for (int x = 0; x != dw; ++x) {
						uint32_t c = srow[x];
						const uint32_t a = c >> 24;
						const int px = ox + x;
						if (!a || px < 0 || px >= (int)screen_width) continue;
						if (slow) c = (c & 0xff000000u) | ((c >> 1) & 0x7f7f7fu) | 0x806000u;
						const uint32_t d = row[px];
						const uint32_t rr = (((c >> 16) & 0xff) * a + ((d >> 16) & 0xff) * (255 - a)) / 255;
						const uint32_t gg = (((c >> 8) & 0xff) * a + ((d >> 8) & 0xff) * (255 - a)) / 255;
						const uint32_t bb = ((c & 0xff) * a + (d & 0xff) * (255 - a)) / 255;
						row[px] = 0xff000000u | rr << 16 | gg << 8 | bb;
					}
				}
				return true;
			}
		}
		pad_cursor_scratch.assign(fw * fh, 0);
		draw_frame(f, false, pad_cursor_scratch.data(), fw, 0, 0, fw, fh);
		const int sc = std::max(1, ((int)screen_height + 240) / 480);
		const int ox = cx + ((int)f.offset.x - (int)g.width / 2) * sc;
		const int oy = cy + ((int)f.offset.y - (int)g.height / 2) * sc;
		for (size_t y = 0; y != fh * (size_t)sc; ++y) {
			const int py = oy + (int)y;
			if (py < 0 || py >= (int)screen_height) continue;
			const uint8_t* srow = pad_cursor_scratch.data() + (y / (size_t)sc) * fw;
			uint32_t* row = data + (size_t)py * pitch;
			for (size_t x = 0; x != fw * (size_t)sc; ++x) {
				const uint8_t idx = srow[x / (size_t)sc];
				if (idx == 0) continue;
				const int px = ox + (int)x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c = 0xff000000u | game_palette[idx];
				if (slow) c = 0xff000000u | ((c >> 1) & 0x7f7f7fu) | 0x806000u;   // precision: amber tint
				row[px] = c;
			}
		}
		return true;
	}

	// A real mouse gets the game cursor too; the system pointer is hidden while it is drawn.
	bool mouse_cursor_hidden = false;
	void mouse_draw_cursor(uint32_t* data, size_t pitch) {
		int mx = -1, my = -1;
		const bool want = !(pad_enabled && pad_active) && wnd.get_mouse_pointer(&mx, &my)
			&& draw_game_cursor_at(data, pitch, mx, my, false);
		if (want != mouse_cursor_hidden) { wnd.show_cursor(!want); mouse_cursor_hidden = want; }
	}
	void pad_draw_cursor(uint32_t* data, size_t pitch) {
		mouse_draw_cursor(data, pitch);
		if (!pad_enabled || !pad_active || pad_cx < 0) return;
		if (pad_draw_game_cursor(data, pitch)) {
			if (pad_card_mode()) pad_draw_card_legend(data, pitch);
			return;
		}
		const int s = std::max(10, (int)screen_height / 40);
		const int x0 = (int)pad_cx, y0 = (int)pad_cy;
		const uint32_t fill = pad_action_held(pact_precise) ? 0xffffd060u : 0xff40ff40u, edge = 0xff000000u;
		for (int r = 0; r <= s; ++r) {
			const int w = r * 2 / 3;
			const int y = y0 + r;
			if (y < 0 || y >= (int)screen_height) continue;
			for (int c = 0; c <= w; ++c) {
				const int x = x0 + c;
				if (x < 0 || x >= (int)screen_width) continue;
				data[(size_t)y * pitch + (size_t)x] = (c == 0 || c == w || r == s) ? edge : fill;
			}
		}
		if (pad_card_mode()) pad_draw_card_legend(data, pitch);
	}
	void pad_draw_card_legend(uint32_t* data, size_t pitch) {
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		const int ts = screen_height >= 900 ? 3 : 2;
		for (int i = 0; i < (int)buttons.size(); ++i) {
			if (buttons[(size_t)i].action == cardact_none) continue;
			const rect r = card_button_rect(i);
			const char* t = pad_slot_label(i);
			const int tw = (int)strlen(t) * (ui::unispace_w + 1) * ts, th = ui::unispace_h * ts;
			const int bx = r.from.x + 1, by = r.from.y + 1, bw = tw + 2 * ts, bh = th + 2 * ts;
			for (int y = std::max(0, by); y < std::min((int)screen_height, by + bh); ++y)
				for (int x = std::max(0, bx); x < std::min((int)screen_width, bx + bw); ++x)
					data[(size_t)y * pitch + (size_t)x] = (y == by || y == by + bh - 1 || x == bx || x == bx + bw - 1) ? 0xff4fd46au : 0xff05080au;
			draw_text_rgba(data, pitch, bx + ts, by + ts, t, 0xffffffffu, ts);
		}
	}
