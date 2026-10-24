// HUD v3 -- the touch HUD.
//
// Included INSIDE struct ui_functions (ui.h), so everything here is a member and
// sees the whole class. HUD v2 stays the default; v3 is chosen in the launcher's
// Settings and arrives as --hud v3. When it is off nothing in this file runs and
// HUD v2 is unchanged.
//
// How it fits in: v3 re-uses HUD v2's controls wherever the control is the same
// thing in a new place -- the minimap, IDLE / ARMY / PING, the control groups,
// SHIFT, MENU and the command card (the "dock") are found through the same
// hud2_* / *_area getters, which return v3 rectangles when hud_v3 is on. Drawing,
// touch and the DIAG checks therefore read one geometry (rule: "piesimas =
// lietimas"). What is new -- the info card, the group rail's "+", the chat
// button, the targeting bar, the placement tick / cross, the command wheel, the
// "hands" template's arc dock, latch and camera stick -- lives here.
//
// Every command still goes through the existing paths (card_activate,
// game_click, issue_default_order_at, submit_production_routed,
// submit_select_single / submit_select_action): no new actions, the simulation
// is not touched, replays and LAN games see exactly what HUD v2 sends.

	// ---- settings (from the launcher; --hud3-* in android_main) -------------------
	bool hud_v3 = false;
	int hud3_surface_dp_h = 0;      // game surface height in dp (glue); 0 = the S8 Ultra's 879
	int hud3_template = 0;          // 0 "Ant stalo / rasiklis" (desk, pen), 1 "Rankose" (hands)
	int hud3_dock_mode = 0;         // 0 by template, 1 3x3 grid, 2 arc
	bool hud3_left = false;         // left-handed: dock left, minimap and modifiers right
	int hud3_scale_pct = 100;       // HUD scale 80..140 %
	int hud3_wheel_ms = 300;        // hold time before the wheel opens (200..600)
	bool hud3_pen_wheel = true;     // S-Pen side button opens the wheel (else it is Shift)
	bool hud3_vibrate = true;
	bool hud3_labels_everywhere = false;   // labels under the dock icons too
	bool hud3_all_groups = false;   // all ten groups on the rail, not just the filled ones
	bool hud3_latch_everywhere = false;    // WHEEL latch and camera stick outside "hands" too
	bool hud3_hint_pending = false; // first v3 game: one-screen hint
	a_string hud3_hint_flag_path;   // written when the hint is dismissed
	std::function<void(int)> hud3_vibrate_fn;   // Android: VibrationEffect; host: none

	bool hud3_hands() const { return hud_v3 && hud3_template == 1; }
	bool hud3_arc_dock() const { return hud3_dock_mode == 2 || (hud3_dock_mode == 0 && hud3_template == 1); }
	bool hud3_has_latch() const { return hud3_hands() || hud3_latch_everywhere; }
	void hud3_buzz(int kind) { if (hud3_vibrate && hud3_vibrate_fn) hud3_vibrate_fn(kind); }
	enum { buzz_tick = 0, buzz_confirm = 1, buzz_error = 2 };

	// ---- units ----------------------------------------------------------------
	// Sizes are dp (measured in millimetres in the design, not screen percentages).
	// Buffer pixels per dp = buffer height / surface height in dp, so a pinch-zoom
	// that resizes the buffer leaves the HUD the same size on the glass, and the
	// FULL HD tier (buffer = device pixels) gets twice the pixels for the same dp.
	int hud3_ppd_milli() const {
		int sdh = hud3_surface_dp_h > 0 ? hud3_surface_dp_h : 879;
		long long v = (long long)screen_height * 1000LL * hud3_scale_pct / (100LL * sdh);
		return v < 250 ? 250 : (int)v;
	}
	int d3(int dp) const { long long v = (long long)dp * hud3_ppd_milli(); return (int)(v >= 0 ? (v + 500) / 1000 : (v - 500) / 1000); }
	int W3() const { return (int)((long long)screen_width * 1000 / hud3_ppd_milli()); }
	int H3() const { return (int)((long long)screen_height * 1000 / hud3_ppd_milli()); }
	// dp rectangle -> buffer rectangle. side=true: a bottom-corner element that
	// changes sides in the left-handed layout (x mirrored).
	rect R3(int x, int y, int w, int h, bool side = true) const {
		if (side && hud3_left) x = W3() - x - w;
		return {{d3(x), d3(y)}, {d3(x + w), d3(y + h)}};
	}
	static bool in3(const rect& r, int x, int y) { return r.to.x > r.from.x && x >= r.from.x && x < r.to.x && y >= r.from.y && y < r.to.y; }

	// ---- layout ---------------------------------------------------------------
	// One function for every rectangle (design 4.2, tablet, "Ant stalo"):
	// minimap 200 dp bottom-left, IDLE / ARMY / PING above it, the group rail above
	// those, SHIFT 72 dp beside the minimap, the info card 560 x 84 dp at the
	// bottom centre, the 3x3 dock bottom-right, MENU and chat top-left. 12 dp from
	// the edges, 8 dp between elements.
	struct hud3_layout_t {
		rect menu, chat, minimap, idle, army, ping, autow, shift, plus, card, card_full, dock, latch, stick;
		rect group[10];
		int group_order[10]; int ngroups = 0;
	};
	bool hud3_info_expanded = false;
	bool hud3_group_live(int n) {
		for (auto uid : control_groups[n]) { const unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) return true; }
		return false;
	}
	hud3_layout_t hud3_layout() {
		hud3_layout_t L;
		const int W = W3(), H = H3(), m = 12;
		L.menu = R3(m, 8, 56, 48, false);
		L.chat = R3(m + 56 + 8, 8, 56, 48, false);
		L.minimap = R3(m, H - m - 200, 200, 200);
		const int uy = H - m - 200 - 8 - 52;
		L.idle = R3(m, uy, 60, 52);
		L.army = R3(m + 70, uy, 60, 52);
		L.ping = R3(m + 140, uy, 60, 52);
		L.shift = R3(m + 200 + 12, H - m - 72, 72, 72);
		if (st.game && st.game->qol.auto_worker) L.autow = R3(m + 200 + 12, H - m - 72 - 8 - 52, 72, 52);
		// group rail: the filled groups (or all ten) from the bottom up, then "+"
		int y = uy - 10 - 46;
		for (int n = 1; n <= 10; ++n) {
			const int g = n % 10;            // 1..9 then 0, like the keyboard row
			if (!hud3_all_groups && !hud3_group_live(g)) continue;
			if (y < 8 + 48 + 8) break;
			L.group[g] = R3(m, y, 64, 46);
			L.group_order[L.ngroups++] = g;
			y -= 52;
		}
		if (!hud3_all_groups && y >= 8 + 48 + 8) L.plus = R3(m, y, 64, 46);
		// info card: centred between the corner groups
		int cw = 560, ch = 84;
		const int side_room = W - 2 * (m + 200 + 12 + 72 + 12);
		if (cw > side_room) cw = side_room < 320 ? 320 : side_room;
		const int cx = (W - cw) / 2;
		L.card = R3(cx, H - m - ch, cw, ch, false);
		L.card_full = R3(cx, H - m - (hud3_info_expanded ? 168 : ch), cw, hud3_info_expanded ? 168 : ch, false);
		// dock: 3x3, 64 dp cells, 6 dp gaps (the arc dock has its own geometry)
		L.dock = R3(W - m - 204, H - m - 204, 204, 204);
		if (hud3_has_latch()) {
			L.latch = R3(W - m - 64, H - m - 256 - 8 - 48, 64, 48);
			L.stick = R3(m + 200 + 12 + 72 + 16, H - m - 92, 92, 92);
		}
		return L;
	}
	// The dock's nine command cells: same slot order as the original card (row i/3,
	// column i%3), so muscle memory carries over.
	rect hud3_dock_cell(int i) {
		if (i < 0 || i > 8) return {};
		if (hud3_arc_dock()) return hud3_arc_cell_box(i);
		const int W = W3(), H = H3(), m = 12;
		const int col = i % 3, row = i / 3;
		// left-handed moves the dock, not the order inside it (the original's slots)
		const int x0 = hud3_left ? m : W - m - 204;
		const int x = x0 + col * 70, y = H - m - 204 + row * 70;
		return {{d3(x), d3(y)}, {d3(x + 64), d3(y + 64)}};
	}

	// ---- the "hands" arc dock (design 4.5) ------------------------------------
	// Around the bottom corner: the corner key is slot 8 (r 76 dp), the inner arc
	// three keys of 30 deg (slots 0-2, 84-168 dp), the outer arc five of 18 deg
	// (slots 3-7, 176-256 dp). Angles from the vertical edge (0) to the bottom
	// edge (90).
	xy hud3_arc_axis() const {
		int x = hud3_left ? d3(10) : (int)screen_width - d3(10);
		return xy(x, (int)screen_height - d3(10));
	}
	// The key under a point, or -1. Same function for drawing and touch.
	int hud3_arc_key_at(int px, int py) {
		xy a = hud3_arc_axis();
		double dx = hud3_left ? (px - a.x) : (a.x - px), dy = a.y - py;
		if (dx < 0 || dy < 0) return -1;
		double r = std::sqrt(dx * dx + dy * dy) * 1000.0 / hud3_ppd_milli();   // in dp
		double ang = std::atan2(dx, dy) * 180.0 / 3.14159265358979;            // 0 = straight up
		if (r <= 76) return 8;
		if (r >= 84 && r <= 168) { int k = (int)(ang / 30.0); return k > 2 ? 2 : k; }
		if (r >= 176 && r <= 256) { int k = (int)(ang / 18.0); return 3 + (k > 4 ? 4 : k); }
		return -1;
	}
	// A square inside each arc key: the icon box (also what card_hit_test uses for
	// tooltips -- the real hit test is hud3_arc_key_at).
	rect hud3_arc_cell_box(int i) {
		xy a = hud3_arc_axis();
		double r, ang;
		if (i == 8) { r = 40; ang = 45; }
		else if (i < 3) { r = 126; ang = 15 + 30 * i; }
		else { r = 216; ang = 9 + 18 * (i - 3); }
		double rad = ang * 3.14159265358979 / 180.0;
		int cx = a.x + (hud3_left ? 1 : -1) * d3((int)(r * std::sin(rad)));
		int cy = a.y - d3((int)(r * std::cos(rad)));
		int s = d3(i == 8 ? 44 : 50) / 2;
		return {{cx - s, cy - s}, {cx + s, cy + s}};
	}

	// Is (x, y) on any v3 HUD element (the minimap excluded: a hold there is an
	// order to that spot)? This is the HUD / field separation (design 4.18): a
	// point on the HUD never becomes a world command. 8 dp dead zone around each.
	bool hud3_rect_at(int x, int y, bool with_minimap = false) {
		if (!hud_v3) return false;
		hud3_layout_t L = hud3_layout();
		const int dz = d3(8);
		auto near = [&](const rect& r) { return r.to.x > r.from.x && x >= r.from.x - dz && x < r.to.x + dz && y >= r.from.y - dz && y < r.to.y + dz; };
		if (with_minimap && near(L.minimap)) return true;
		if (near(L.menu) || near(L.chat) || near(L.idle) || near(L.army) || near(L.ping) || near(L.autow) || near(L.shift) || near(L.plus) || near(L.card_full) || near(L.latch) || near(L.stick)) return true;
		for (int i = 0; i < L.ngroups; ++i) if (near(L.group[L.group_order[i]])) return true;
		if (hud3_arc_dock()) { if (hud3_arc_key_at(x, y) >= 0) return true; }
		else if (near(L.dock)) return true;
		if (hud3_bar_hit(x, y) || hud3_place_hit(x, y) != 0) return true;
		return false;
	}

	// ---- drawing helpers ------------------------------------------------------
	void hud3_box(uint32_t* data, size_t pitch, rect r, bool on = false, bool pressed = false, uint32_t line = 0) {
		const hud_theme_t th = hud_theme();
		fill_rect_rgba(data, pitch, r, 0xff000000u | ((on ? th.on_fill : pressed ? th.btn_pressed : th.btn_fill) & 0xffffffu));
		line_rectangle_rgba(data, pitch, r, line ? line : on ? th.on_line : th.btn_line);
	}
	// A thick line without an edge, for the drawn glyphs (menu bars, arrows...).
	void hud3_stroke(uint32_t* data, size_t pitch, int x0, int y0, int x1, int y1, uint32_t c, int t) {
		if (t < 1) t = 1;
		int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
		for (int k = 0; k < 8192; ++k) {
			mark_box(data, pitch, x0 - t / 2, y0 - t / 2, x0 - t / 2 + t, y0 - t / 2 + t, 0xff000000u | (c & 0xffffffu), (int)screen_height);
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	}
	// Small text centred in a rectangle at a percentage size (100 = the atlas).
	void hud3_text_c(uint32_t* data, size_t pitch, rect r, int yc, const char* s, uint32_t col, int pct = 100) {
		int adv = (int)((ui::unispace_w + 1) * hud_text_pct(pct) / 100.0f + 0.5f);
		int w = (int)strlen(s) * adv;
		int h = ui::unispace_h * hud_text_pct(pct) / 100;
		draw_text_rgba_scaled(data, pitch, (r.from.x + r.to.x) / 2 - w / 2, yc - h / 2, s, col, pct);
	}
	int hud3_text_w(const char* s, int pct = 100) { return (int)strlen(s) * (int)((ui::unispace_w + 1) * hud_text_pct(pct) / 100.0f + 0.5f); }
	int hud3_text_h(int pct = 100) { return ui::unispace_h * hud_text_pct(pct) / 100; }

	// ---- the top row: MENU, chat, FPS --------------------------------------------
	void draw_hud3_top(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		const hud_theme_t th = hud_theme();
		hud3_box(data, pitch, L.menu, false, hud_pressed(L.menu), th.border);
		{
			const int cx = (L.menu.from.x + L.menu.to.x) / 2, cy = (L.menu.from.y + L.menu.to.y) / 2;
			const int w = d3(22), t = d3(3);
			for (int k = -1; k <= 1; ++k) mark_box(data, pitch, cx - w / 2, cy + k * d3(7) - t / 2, cx + w / 2, cy + k * d3(7) - t / 2 + t, 0xff000000u | (th.accent & 0xffffffu), (int)screen_height);
		}
		hud3_box(data, pitch, L.chat, chat_active, hud_pressed(L.chat), th.border);
		{
			const int cx = (L.chat.from.x + L.chat.to.x) / 2, cy = (L.chat.from.y + L.chat.to.y) / 2;
			const int w = d3(22), h = d3(15), t = d3(2);
			rect b = {{cx - w / 2, cy - h / 2 - d3(2)}, {cx + w / 2, cy + h / 2 - d3(2)}};
			for (int k = 0; k < t; ++k) line_rectangle_rgba(data, pitch, {{b.from.x + k, b.from.y + k}, {b.to.x - k, b.to.y - k}}, th.accent);
			hud3_stroke(data, pitch, b.from.x + d3(5), b.to.y, b.from.x + d3(3), b.to.y + d3(6), th.accent, t);
			hud3_stroke(data, pitch, b.from.x + d3(3), b.to.y + d3(6), b.from.x + d3(11), b.to.y, th.accent, t);
		}
		if (hud_show_fps) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%d fps", fps_display);
			draw_text_rgba(data, pitch, L.chat.to.x + d3(10), (L.chat.from.y + L.chat.to.y) / 2 - usp_h() / 2, buf, 0xff90a0a0u);
		}
		if (is_net_game) {
			char buf[16];
			int ms = net_latency_ms;
			snprintf(buf, sizeof(buf), "%d ms", ms);
			uint32_t col = ms < 80 ? 0xff40ff60u : (ms <= 160 ? 0xffffa030u : 0xffff4040u);
			draw_text_rgba(data, pitch, L.chat.to.x + d3(10), (L.chat.from.y + L.chat.to.y) / 2 + usp_h() / 2 + d3(2), buf, col);
		}
	}

	// ---- IDLE / ARMY / PING ------------------------------------------------------
	void draw_hud3_utils(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		const hud_theme_t th = hud_theme();
		load_cmdicons();
		const int nidle = idle_worker_count(), narmy = (int)army_clusters().size();
		// v0.126.004: no captions under IDLE / ARMY / PING / SHIFT (they look
		// bad) -- the icon and the count say it; both sit in the middle of the key.
		struct { rect r; int icon; int n; } b[2] = {
			{L.idle, cmdicon_repair, nidle}, {L.army, cmdicon_attack, narmy}};
		for (auto& k : b) {
			const bool dim = k.n == 0;
			hud3_box(data, pitch, k.r, false, hud_pressed(k.r), dim ? th.util_line_zero : th.util_line);
			const int h = k.r.to.y - k.r.from.y;
			char buf[8]; snprintf(buf, sizeof buf, "%d", k.n);
			const int is = h * 70 / 100, gap = d3(3), tw = (int)strlen(buf) * usp_adv();
			const int x0 = (k.r.from.x + k.r.to.x) / 2 - (is + gap + tw) / 2, y0 = (k.r.from.y + k.r.to.y) / 2 - is / 2;
			rect icon = {{x0, y0}, {x0 + is, y0 + is}};
			if (cmdicons_ok) draw_cmdicon_fit(data, pitch, k.icon, icon, false, 6, dim ? 0x707880u : th.icon_tint);
			draw_text_rgba(data, pitch, icon.to.x + gap, (icon.from.y + icon.to.y) / 2 - usp_h() / 2, buf, dim ? 0xff707880u : th.amber);
		}
		// PING
		{
			rect r = L.ping;
			fill_rect_rgba(data, pitch, r, ping_armed ? th.ping_armed_fill : (hud_pressed(r) ? th.btn_pressed : th.btn_fill));
			line_rectangle_rgba(data, pitch, r, th.ping_edge);
			const int cx = (r.from.x + r.to.x) / 2, cy = (r.from.y + r.to.y) / 2;
			for (int ring = 0; ring < 2; ++ring) {
				const int rr = ring ? d3(10) : d3(3);
				for (int a = 0; a < 360; a += ring ? 6 : 20) {
					double t = a * 3.14159265 / 180.0;
					int px = cx + (int)std::lround(rr * std::cos(t)), py = cy + (int)std::lround(rr * std::sin(t));
					mark_box(data, pitch, px - d3(1), py - d3(1), px + d3(1) + 1, py + d3(1) + 1, th.ping_edge, (int)screen_height);
				}
			}
		}
	}

	// ---- the group rail ----------------------------------------------------------
	void draw_hud3_groups(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		const hud_theme_t th = hud_theme();
		load_wireframe();
		for (int i = 0; i < L.ngroups; ++i) {
			const int n = L.group_order[i];
			const rect b = L.group[n];
			int live = 0; unit_t* first = nullptr;
			for (auto uid : control_groups[n]) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) { if (!first) first = u; ++live; } }
			const bool current = live && group_is_current(n);
			fill_rect_rgba(data, pitch, b, hud_pressed(b) ? th.btn_pressed : current ? 0xff123a1fu : live ? 0xff000000u | (th.grp_fill & 0xffffffu) : th.btn_fill);
			line_rectangle_rgba(data, pitch, b, current ? 0xff4fd46au : live ? th.grp_border : th.off_line);
			if (current) line_rectangle_rgba(data, pitch, {{b.from.x + 1, b.from.y + 1}, {b.to.x - 1, b.to.y - 1}}, 0xff4fd46au);
			char nl[2] = { (char)('0' + n), 0 };
			draw_text_rgba_scaled(data, pitch, b.from.x + d3(3), b.from.y + d3(2), nl, current ? 0xffd6f5ddu : 0xff9098a0u, 100);
			if (first) {
				const int box = (b.to.y - b.from.y) - d3(8);
				blit_wireframe_type(data, pitch, first->unit_type, b.from.x + d3(14), b.from.y + d3(4), box, wire_palette(first, 100));
				char cnt[4]; snprintf(cnt, sizeof cnt, "%d", live > 99 ? 99 : live);
				draw_text_rgba(data, pitch, b.to.x - d3(4) - (int)strlen(cnt) * usp_adv(), b.to.y - usp_h() - d3(3), cnt, th.accent);
			}
		}
		if (L.plus.to.x > L.plus.from.x) {
			const rect b = L.plus;
			const bool has_sel = first_own_selected_unit() != nullptr;
			fill_rect_rgba(data, pitch, b, 0xff000000u | ((hud_pressed(b) ? th.btn_pressed : th.off_fill) & 0xffffffu));
			line_rectangle_rgba(data, pitch, b, th.off_line);
			const int cx = (b.from.x + b.to.x) / 2, cy = (b.from.y + b.to.y) / 2, s = d3(9), t = d3(2);
			const uint32_t c = has_sel ? 0xffc0c8d0u : 0xff5a626au;
			mark_box(data, pitch, cx - s, cy - t / 2, cx + s + 1, cy - t / 2 + t, c, (int)screen_height);
			mark_box(data, pitch, cx - t / 2, cy - s, cx - t / 2 + t, cy + s + 1, c, (int)screen_height);
		}
	}
	// "+": the selection becomes the first empty group.
	bool hud3_plus_tap() {
		if (!first_own_selected_unit()) return false;
		prune_control_groups();
		for (int n = 1; n <= 10; ++n) { const int g = n % 10; if (!hud3_group_live(g)) { assign_control_group(g); hud3_buzz(buzz_confirm); return true; } }
		return false;
	}

	// ---- SHIFT (design 4.10): tap = for the next command, double tap = locked,
	// hold = while held (the held finger is kept out of the two-finger gestures by
	// sdl2.cpp's "HUD fingers"). -----------------------------------------------------
	bool hud3_shift_once = false;     // turns itself off after the next order
	bool hud3_shift_locked = false;
	bool hud3_shift_held = false;     // a finger is holding the key
	std::chrono::steady_clock::time_point hud3_shift_last_tap{};
	void hud3_shift_tap() {
		auto now = std::chrono::steady_clock::now();
		const bool dbl = now - hud3_shift_last_tap < std::chrono::milliseconds(400);
		hud3_shift_last_tap = now;
		if (hud3_shift_locked) { hud3_shift_locked = false; hud3_shift_once = false; queue_mode = false; return; }
		if (dbl && hud3_shift_once) { hud3_shift_locked = true; hud3_shift_once = false; queue_mode = true; hud3_buzz(buzz_confirm); return; }
		if (hud3_shift_once) { hud3_shift_once = false; queue_mode = false; return; }
		hud3_shift_once = true; queue_mode = true;
	}
	// Called after an order goes out (order opcodes) -- the one-shot SHIFT ends.
	void hud3_order_sent() {
		if (hud_v3 && hud3_shift_once && !hud3_shift_held) { hud3_shift_once = false; queue_mode = false; }
	}
	void draw_hud3_shift(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		const hud_theme_t th = hud_theme();
		const rect r = L.shift;
		const bool on = queue_mode || hud3_shift_held;
		hud3_box(data, pitch, r, on, hud_pressed(r), on ? th.on_line : th.border);
		const uint32_t c = on ? th.on_text : th.accent;
		// an outlined arrow, the keyboard's Shift -- alone, in the middle of the key
		const int hw = d3(15), stem = d3(7), ah = d3(16), sh = d3(13);
		const int cx = (r.from.x + r.to.x) / 2, top = (r.from.y + r.to.y) / 2 - (ah + sh) / 2, t = d3(3);
		hud3_stroke(data, pitch, cx, top, cx - hw, top + ah, c, t);
		hud3_stroke(data, pitch, cx, top, cx + hw, top + ah, c, t);
		hud3_stroke(data, pitch, cx - hw, top + ah, cx - stem, top + ah, c, t);
		hud3_stroke(data, pitch, cx + hw, top + ah, cx + stem, top + ah, c, t);
		hud3_stroke(data, pitch, cx - stem, top + ah, cx - stem, top + ah + sh, c, t);
		hud3_stroke(data, pitch, cx + stem, top + ah, cx + stem, top + ah + sh, c, t);
		hud3_stroke(data, pitch, cx - stem, top + ah + sh, cx + stem, top + ah + sh, c, t);
		if (hud3_shift_locked) {   // a small padlock in the corner
			const int lx = r.to.x - d3(16), ly = r.from.y + d3(6);
			mark_box(data, pitch, lx, ly + d3(5), lx + d3(10), ly + d3(12), c, (int)screen_height);
			line_rectangle_rgba(data, pitch, {{lx + d3(2), ly}, {lx + d3(8), ly + d3(6)}}, c);
		}
	}

	// ---- the dock (3x3 grid or arc) ---------------------------------------------
	static bool hud3_instant(const card_button_t& b) {
		return b.action == cardact_stop || b.action == cardact_ability || b.action == cardact_cancel
		    || b.action == cardact_liftoff || b.action == cardact_morph || b.action == cardact_morph_building;
	}
	static bool hud3_submenu(const card_button_t& b) {
		return b.action == cardact_menu_build || b.action == cardact_menu_adv || b.action == cardact_select_larva;
	}
	// A lightning bolt (instant) or a small triangle (opens a menu) in a corner.
	void hud3_corner_mark(uint32_t* data, size_t pitch, rect r, bool instant, bool submenu) {
		const hud_theme_t th = hud_theme();
		if (instant) {
			const int x = r.to.x - d3(9), y = r.from.y + d3(3), t = d3(2) > 0 ? d3(2) : 1;
			hud3_stroke(data, pitch, x + d3(3), y, x, y + d3(5), th.amber, t);
			hud3_stroke(data, pitch, x, y + d3(5), x + d3(4), y + d3(5), th.amber, t);
			hud3_stroke(data, pitch, x + d3(4), y + d3(5), x + d3(1), y + d3(10), th.amber, t);
		}
		if (submenu) {
			const int x = r.to.x - d3(8), y = r.to.y - d3(10);
			for (int k = 0; k < d3(6); ++k) mark_box(data, pitch, x, y + k / 2, x + 1 + (k < d3(3) ? k : d3(6) - k), y + k / 2 + 1, th.amber, (int)screen_height);
		}
	}
	void draw_hud3_dock(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		const hud_theme_t th = hud_theme();
		load_cmdicons();
		if (hud3_arc_dock()) draw_hud3_arc_backdrop(data, pitch);
		for (int i = 0; i < 9; ++i) {
			const bool has = i < (int)buttons.size() && buttons[(size_t)i].action != cardact_none;
			rect r = hud3_dock_cell(i);
			if (!has) {
				// empty slots stay as faint outlines, so the grid keeps its shape
				if (!hud3_arc_dock()) { blend_rect_rgba(data, pitch, r, 0x000000u, 90); line_rectangle_rgba(data, pitch, r, 0xff000000u | (th.off_line & 0xffffffu)); }
				continue;
			}
			const card_button_t& b = buttons[(size_t)i];
			const bool armed = (b.action == cardact_attack && attack_armed) || (b.action == cardact_ability_target && ability_armed_order == b.arg);
			if (!hud3_arc_dock()) {
				fill_rect_rgba(data, pitch, r, armed ? th.armed_fill : (hud_pressed(r) ? th.btn_pressed : th.btn_fill));
				line_rectangle_rgba(data, pitch, r, armed ? th.armed_line : th.btn_line);
			}
			int icon = card_icon_frame(b.action, b.arg);
			rect ir = r;
			if (hud3_labels_everywhere && !hud3_arc_dock()) ir.to.y -= d3(12);
			if (icon >= 0 && cmdicons_ok) {
				int mgn = (b.action == cardact_build || b.action == cardact_morph_building || b.action == cardact_addon) ? 4 : 14;
				draw_cmdicon_fit(data, pitch, icon, ir, false, mgn, armed ? 0xffffffu : th.icon_tint);
			} else draw_card_label(data, pitch, r, b.label);
			if (hud3_labels_everywhere && !hud3_arc_dock()) hud3_text_c(data, pitch, r, r.to.y - d3(7), b.label, 0xffb8c0c8u, 100);
			hud3_corner_mark(data, pitch, r, hud3_instant(b), hud3_submenu(b));
			if (!b.available) {
				blend_rect_rgba(data, pitch, r, th.dim_rgb, 150);
				if (!hud3_arc_dock()) line_rectangle_rgba(data, pitch, r, th.dim_line);
			} else if (!hud3_affordable(b)) {
				// can be ordered, but not paid for yet: the cost shows in red
				char cost[24]; hud3_cost_text(b, cost, sizeof cost);
				if (cost[0]) draw_text_rgba_scaled(data, pitch, r.from.x + d3(3), r.to.y - hud3_text_h(100) - d3(2), cost, 0xffff5a50u, 100);
			}
		}
	}
	// Mineral / gas / energy cost of a card button, as "150", "50/25" or "75e".
	void hud3_cost_text(const card_button_t& b, char* out, size_t n) {
		out[0] = 0;
		int mi = 0, ga = 0, en = 0;
		switch (b.action) {
		case cardact_train: case cardact_build: case cardact_morph: case cardact_morph_building: case cardact_addon: {
			const unit_type_t* t = get_unit_type((UnitTypes)b.arg);
			if (t) { mi = t->mineral_cost; ga = t->gas_cost; }
			break;
		}
		case cardact_research: { const tech_type_t* t = get_tech_type((TechTypes)b.arg); if (t) { mi = t->mineral_cost; ga = t->gas_cost; } break; }
		case cardact_upgrade: {
			const upgrade_type_t* t = get_upgrade_type((UpgradeTypes)b.arg);
			if (t && game_local_player >= 0) { int lv = player_upgrade_level(game_local_player, (UpgradeTypes)b.arg); mi = t->mineral_cost_base + t->mineral_cost_factor * lv; ga = t->gas_cost_base + t->gas_cost_factor * lv; }
			break;
		}
		case cardact_ability_target: en = hud3_order_energy(b.arg); break;
		default: break;
		}
		if (en > 0) snprintf(out, n, "%de", en);
		else if (mi > 0 && ga > 0) snprintf(out, n, "%d/%d", mi, ga);
		else if (mi > 0) snprintf(out, n, "%d", mi);
		else if (ga > 0) snprintf(out, n, "%dg", ga);
	}
	// Energy a targeted spell costs (0 for orders that are not spells).
	int hud3_order_energy(int order_id) {
		const order_type_t* o = get_order_type((Orders)order_id);
		if (!o || o->tech_type == TechTypes::None) return 0;
		const tech_type_t* t = get_tech_type(o->tech_type);
		return t ? t->energy_cost : 0;
	}
	bool hud3_affordable(const card_button_t& b) {
		if (game_local_player < 0) return true;
		const int p = game_local_player;
		int mi = 0, ga = 0, en = 0;
		switch (b.action) {
		case cardact_train: case cardact_build: case cardact_morph: case cardact_morph_building: case cardact_addon: {
			const unit_type_t* t = get_unit_type((UnitTypes)b.arg); if (t) { mi = t->mineral_cost; ga = t->gas_cost; } break;
		}
		case cardact_research: { const tech_type_t* t = get_tech_type((TechTypes)b.arg); if (t) { mi = t->mineral_cost; ga = t->gas_cost; } break; }
		case cardact_upgrade: {
			const upgrade_type_t* t = get_upgrade_type((UpgradeTypes)b.arg);
			if (t) { int lv = player_upgrade_level(p, (UpgradeTypes)b.arg); mi = t->mineral_cost_base + t->mineral_cost_factor * lv; ga = t->gas_cost_base + t->gas_cost_factor * lv; }
			break;
		}
		case cardact_ability_target: en = hud3_order_energy(b.arg); break;
		default: break;
		}
		if (st.current_minerals[p] < mi || st.current_gas[p] < ga) return false;
		if (en > 0) {
			unit_t* u = focus_or_primary();
			if (u && (u->unit_type->flags & unit_type_t::flag_has_energy) && u->energy.integer_part() < en) return false;
		}
		return true;
	}
	// The arc dock's keys, drawn as annular sectors (design 4.5).
	// The key under every pixel of the quadrant is cached (the low 4 bits: key + 1,
	// 0 = none; bit 4: on a key's edge). It used to be computed three times per pixel
	// per frame with a sqrt and an atan2 each -- 17 fps on the S8 Ultra with the arc
	// dock on screen.
	a_vector<uint8_t> hud3_arc_mask; int hud3_arc_mask_r = 0, hud3_arc_mask_ppd = 0; bool hud3_arc_mask_left = false;
	void hud3_arc_build_mask() {
		const int R = d3(258);
		if (R == hud3_arc_mask_r && hud3_ppd_milli() == hud3_arc_mask_ppd && hud3_left == hud3_arc_mask_left && !hud3_arc_mask.empty()) return;
		hud3_arc_mask_r = R; hud3_arc_mask_ppd = hud3_ppd_milli(); hud3_arc_mask_left = hud3_left;
		const int D = R + 2;
		hud3_arc_mask.assign((size_t)D * D, 0);
		xy a = hud3_arc_axis();
		// mask (mx, my): mx = distance from the axis towards the screen centre, my = up
		auto key = [&](int mx, int my) { return hud3_arc_key_at(hud3_left ? a.x + mx : a.x - mx, a.y - my); };
		for (int my = 0; my < D; ++my) for (int mx = 0; mx < D; ++mx) {
			const int k = key(mx, my);
			if (k < 0) continue;
			const bool edge = key(mx + 1, my) != k || key(mx, my + 1) != k || key(mx - 1, my) != k || key(mx, my - 1) != k;
			hud3_arc_mask[(size_t)my * D + mx] = (uint8_t)((k + 1) | (edge ? 16 : 0));
		}
	}
	void draw_hud3_arc_backdrop(uint32_t* data, size_t pitch) {
		const hud_theme_t th = hud_theme();
		hud3_arc_build_mask();
		xy a = hud3_arc_axis();
		const int D = hud3_arc_mask_r + 2;
		static_vector<card_button_t, 9> buttons;
		card_buttons(buttons);
		uint32_t fill[9], line[9];
		for (int k = 0; k < 9; ++k) {
			const bool has = k < (int)buttons.size() && buttons[(size_t)k].action != cardact_none;
			const bool armed = has && ((buttons[(size_t)k].action == cardact_attack && attack_armed) || (buttons[(size_t)k].action == cardact_ability_target && ability_armed_order == buttons[(size_t)k].arg));
			fill[k] = has ? (armed ? th.armed_fill : th.btn_fill) : (th.panel_fill | 0xff000000u);
			line[k] = armed ? th.armed_line : th.btn_line;
		}
		for (int my = 0; my < D; ++my) {
			const int y = a.y - my;
			if (y < 0 || y >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)y * pitch;
			const uint8_t* m = &hud3_arc_mask[(size_t)my * D];
			for (int mx = 0; mx < D; ++mx) {
				const uint8_t v = m[mx];
				if (!v) continue;
				const int x = hud3_left ? a.x + mx : a.x - mx;
				if (x < 0 || x >= (int)screen_width) continue;
				const int k = (v & 15) - 1;
				row[x] = (v & 16) ? line[k] : fill[k];
			}
		}
	}

	// ---- the info card (design 4.11 and 4.11a) -------------------------------------------
	// 560 x 84 dp at the bottom centre for every selection (it does
	// not grow for buildings); ^ expands it to 168 dp (extras only), x clears the
	// selection. All rectangles are the design's (4.11a), in dp from the card row's
	// top-left; with the real font: 10 dp per character, 17 dp per line.
	//   one unit      name (race prefix dropped) + kills on one row, HP / shields /
	//                 energy rows under it, equipment icons right-aligned
	//   a building    the same with short bars; production: five 44 dp slots, the
	//                 caption, a segmented progress bar and the time left; tap a
	//                 queue slot = cancel it; a construction / egg slot is not tappable
	//   buildings     type tokens + one slot per building of the focused type
	//   a group       72 dp type tokens (wireframe + xN): tap = focus, double tap =
	//                 keep only that type, hold = drop that type
	//   cargo         up to five passengers in the slot positions; tap = unload
	struct hud3_token_t { rect r; const unit_type_t* type; };
	a_vector<hud3_token_t> hud3_tokens;
	a_vector<rect> hud3_prod_slots;   // cancellable production slots (build_queue order)
	struct hud3_bslot_t { rect r; unit_id uid; };
	a_vector<hud3_bslot_t> hud3_bldg_slots;   // one per building (multi-building selection): tap = select it
	// the bottom row's origin (the expanded card adds a row ABOVE it)
	xy hud3_card_row() { hud3_layout_t L = hud3_layout(); return xy(L.card_full.from.x, L.card_full.to.y - d3(84)); }
	rect hud3_cr(int x, int y, int w, int h) { xy o = hud3_card_row(); return {{o.x + d3(x), o.y + d3(y)}, {o.x + d3(x + w), o.y + d3(y + h)}}; }
	rect hud3_card_expand_btn() { return hud3_cr(524, 0, 36, 42); }   // touch areas; the glyphs are 26 dp
	rect hud3_card_close_btn() { return hud3_cr(524, 42, 36, 42); }
	void hud3_bar(uint32_t* data, size_t pitch, int x, int y, int w, int h, int cur, int max, uint32_t col) {
		fill_rect_rgba(data, pitch, {{x, y}, {x + w, y + h}}, 0xff1a2024u);
		if (max > 0 && cur > 0) { int fw = (int)((long long)w * (cur > max ? max : cur) / max); fill_rect_rgba(data, pitch, {{x, y}, {x + fw, y + h}}, col); }
		line_rectangle_rgba(data, pitch, {{x - 1, y - 1}, {x + w + 1, y + h + 1}}, 0xff000000u);
	}
	// "Terran Command Center" -> "Command Center" (17 characters fit beside the slots)
	a_string hud3_short_name(const unit_type_t* t, size_t maxc = 17) {
		load_unit_names();
		a_string nm = unit_name(t);
		for (const char* pre : {"Terran ", "Zerg ", "Protoss "}) { size_t n = strlen(pre); if (nm.size() > n && nm.compare(0, n, pre) == 0) { nm = nm.substr(n); break; } }
		if (nm.size() > maxc) nm = nm.substr(0, maxc);
		return nm;
	}
	// What a building / egg is busy with: icon, caption, frames left and total.
	struct hud3_work_t { bool busy = false; int icon = -1; const char* caption = ""; int rem = 0, total = 0; bool cancellable = false; int level = 0; };
	hud3_work_t hud3_work(unit_t* u) {
		hud3_work_t w;
		if (!u || u->owner != game_local_player) return w;
		const race_t pr = u->owner < 12 ? st.players[u->owner].race : unit_race(u);
		const bool bldg = ut_building(u);
		if (!u->build_queue.empty()) {
			const unit_type_t* act = u->build_queue[0];
			w.rem = u->current_build_unit ? u->current_build_unit->remaining_build_time : u->remaining_build_time;
			if (!bldg && !u->current_build_unit && w.rem == 0) return w;   // a worker still walking to its site
			w.busy = true; w.icon = (int)act->id; w.total = act->build_time;
			if (bldg && !u->current_build_unit) w.rem = w.total;            // queued this very frame: nothing done yet
			if (!bldg) w.caption = "Morphing";                       // an Egg / Cocoon: it IS the thing being made
			else if (ut_building(act)) w.caption = "Building";        // an addon going up
			else w.caption = pr == race_t::protoss ? "Warping" : pr == race_t::zerg ? "Morphing" : "Training";
			w.cancellable = bldg && u_completed(u);
			return w;
		}
		if (bldg && !u_completed(u)) {
			w.busy = true; w.icon = (int)u->unit_type->id; w.total = u->unit_type->build_time; w.rem = u->remaining_build_time;
			w.caption = pr == race_t::zerg ? "Evolving" : pr == race_t::protoss ? "Warping" : "Building";
			return w;
		}
		if (u->building.researching_type) {
			w.busy = true; w.icon = u->building.researching_type->icon; w.total = u->building.researching_type->research_time;
			w.rem = u->building.upgrade_research_time; w.caption = "Researching"; w.cancellable = true;
			return w;
		}
		if (u->building.upgrading_type) {
			const int lv = player_upgrade_level(u->owner, u->building.upgrading_type->id);
			w.busy = true; w.icon = u->building.upgrading_type->icon;
			w.total = u->building.upgrading_type->time_cost_base + u->building.upgrading_type->time_cost_factor * lv;
			w.rem = u->building.upgrade_research_time; w.caption = "Upgrading"; w.cancellable = true; w.level = lv + 1;
			return w;
		}
		return w;
	}
	// The segmented progress bar and the time left (design: 270,64 196x8; time right-aligned at 514).
	void hud3_progress(uint32_t* data, size_t pitch, const hud3_work_t& w, uint32_t accent) {
		rect b = hud3_cr(270, 64, 196, 8);
		for (int t = 1; t <= d3(2); ++t) line_rectangle_rgba(data, pitch, {{b.from.x - t, b.from.y - t}, {b.to.x + t, b.to.y + t}}, accent);
		fill_rect_rgba(data, pitch, b, 0xff0a0e11u);
		const int pct = w.total > 0 ? 100 - (int)((long long)w.rem * 100 / w.total) : 0;
		const int seg = d3(8), gap = d3(2) > 0 ? d3(2) : 1;
		const int fw = (b.to.x - b.from.x) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
		for (int x = 0; x + seg <= fw + seg / 2 && x < b.to.x - b.from.x; x += seg + gap)
			fill_rect_rgba(data, pitch, {{b.from.x + x, b.from.y}, {std::min(b.from.x + x + seg, b.to.x), b.to.y}}, 0xff40d060u);
		char tm[12]; snprintf(tm, sizeof tm, "%ds", (int)((long long)w.rem * 42 / 1000));
		rect tr = hud3_cr(474, 59, 40, 17);
		draw_text_rgba(data, pitch, tr.to.x - hud3_text_w(tm), tr.from.y, tm, 0xffd8dee2u);
	}
	void draw_hud3_card(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		hud3_tokens.clear();
		hud3_prod_slots.clear();
		hud3_bldg_slots.clear();
		selection_grid_hit.clear();
		equip_hit.clear();
		const hud_theme_t th = hud_theme();
		const rect c = L.card_full;
		int own = 0, own_bldg = 0; unit_t* first = nullptr;
		for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player) { ++own; if (ut_building(u)) ++own_bldg; if (!first) first = u; } }
		if (!first && current_selection.size() == 1) first = get_unit(current_selection.front());
		const bool wide = unlimited_sel_on() && wide_selection.size() > 12;
		hud2_frame(data, pitch, c);
		draw_hud_tab(data, pitch, c, 0);
		// portrait (the focused / first unit's; mission transmissions take it over)
		rect pr = hud3_cr(8, 8, 68, 68);
		fill_rect_rgba(data, pitch, pr, th.portrait_fill);
		draw_portrait_video(data, pitch, pr);
		line_rectangle_rgba(data, pitch, pr, th.inner);
		// ^ and x (26 dp glyphs in 36 x 42 dp touch areas)
		{
			const rect e = hud3_cr(530, 4, 26, 26), x = hud3_cr(530, 54, 26, 26);
			const int t = d3(2) > 0 ? d3(2) : 1, s = d3(6);
			const int ecx = (e.from.x + e.to.x) / 2, ecy = (e.from.y + e.to.y) / 2;
			if (hud3_info_expanded) { hud3_stroke(data, pitch, ecx - s, ecy - s / 2, ecx, ecy + s / 2, 0xff8a96a0u, t); hud3_stroke(data, pitch, ecx, ecy + s / 2, ecx + s, ecy - s / 2, 0xff8a96a0u, t); }
			else { hud3_stroke(data, pitch, ecx - s, ecy + s / 2, ecx, ecy - s / 2, 0xff8a96a0u, t); hud3_stroke(data, pitch, ecx, ecy - s / 2, ecx + s, ecy + s / 2, 0xff8a96a0u, t); }
			if (first) {
				const int xcx = (x.from.x + x.to.x) / 2, xcy = (x.from.y + x.to.y) / 2;
				hud3_stroke(data, pitch, xcx - s, xcy - s, xcx + s, xcy + s, 0xff8a96a0u, t);
				hud3_stroke(data, pitch, xcx - s, xcy + s, xcx + s, xcy - s, 0xff8a96a0u, t);
			}
		}
		if (!first) {
			rect r = hud3_cr(88, 8, 426, 68);
			hud3_text_c(data, pitch, r, (r.from.y + r.to.y) / 2, "Nothing selected", 0xff707a82u);
			return;
		}
		load_wireframe();
		load_cmdicons();
		const xy up(c.from.x, c.from.y);   // the expanded part's origin
		auto UR = [&](int x, int y, int w, int h) { return rect{{up.x + d3(x), up.y + d3(y)}, {up.x + d3(x + w), up.y + d3(y + h)}}; };
		auto slot_frame = [&](rect r, bool filled, bool active, uint32_t accent) {
			fill_rect_rgba(data, pitch, r, th.btn_fill);
			line_rectangle_rgba(data, pitch, r, filled ? accent : th.slot_free_line);
			if (active) line_rectangle_rgba(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.to.x - 1, r.to.y - 1}}, accent);
		};
		if (own > 1 || wide) {
			// ---- a group: type tokens (spellcasters first: the focus goes to them)
			struct tcount_t { const unit_type_t* t; int n; unit_t* any; };
			static_vector<tcount_t, 72> types;
			const a_vector<unit_id>& sel = wide ? wide_selection : current_selection;
			for (auto uid : sel) {
				unit_t* u = get_unit(uid); if (!u || u->owner != game_local_player) continue;
				bool found = false;
				for (auto& t : types) if (t.t == u->unit_type) { ++t.n; found = true; break; }
				if (!found && types.size() < types.capacity()) types.push_back({u->unit_type, 1, u});
			}
			std::stable_sort(types.begin(), types.end(), [](const tcount_t& a, const tcount_t& b) {
				const bool ca = (a.t->flags & unit_type_t::flag_has_energy) != 0, cb = (b.t->flags & unit_type_t::flag_has_energy) != 0;
				return ca && !cb;
			});
			unit_t* fu = get_unit(focused_unit_id);
			const unit_type_t* ftype = fu && std::find(sel.begin(), sel.end(), focused_unit_id) != sel.end() ? fu->unit_type : nullptr;
			if (!ftype) { unit_t* p = focus_or_primary(); if (p) ftype = p->unit_type; }
			const bool all_bldg = own_bldg == own && !wide;
			const int max_tok = all_bldg ? 2 : 4;
			int shown = 0;
			for (size_t i = 0; i < types.size() && shown < max_tok; ++i, ++shown) {
				const auto& t = types[i];
				rect r = hud3_cr(88 + 78 * shown, 8, 72, 68);
				const bool on = t.t == ftype;
				fill_rect_rgba(data, pitch, r, on ? 0xff123a1fu : (hud_pressed(r) ? th.btn_pressed : th.btn_fill));
				line_rectangle_rgba(data, pitch, r, on ? 0xff4fd46au : th.btn_line);
				blit_wireframe_type(data, pitch, t.t, r.from.x + d3(12), r.from.y + d3(2), d3(48), wire_palette(t.any, 100));
				char nb[8]; snprintf(nb, sizeof nb, "x%d", t.n);
				hud3_text_c(data, pitch, r, r.to.y - d3(10), nb, th.amber);
				hud3_tokens.push_back({r, t.t});
			}
			if ((int)types.size() > shown) {
				rect r = hud3_cr(88 + 78 * shown, 8, 72, 68);
				fill_rect_rgba(data, pitch, r, th.btn_fill);
				line_rectangle_rgba(data, pitch, r, th.btn_line);
				char nb[8]; snprintf(nb, sizeof nb, "+%d", (int)types.size() - shown);
				hud3_text_c(data, pitch, r, (r.from.y + r.to.y) / 2, nb, 0xffd0d8e0u);
			}
			if (all_bldg) {
				// ---- several buildings: one slot per building of the focused type
				int i = 0, busy = 0, total = 0;
				for (auto uid : current_selection) {
					unit_t* b = get_unit(uid);
					if (!b || b->owner != game_local_player || b->unit_type != ftype) continue;
					++total;
					const hud3_work_t w = hud3_work(b);
					if (w.busy) ++busy;
					if (i >= 5) continue;
					rect r = hud3_cr(270 + 50 * i, 8, 44, 44);
					slot_frame(r, w.busy, false, race_accent(b));
					if (w.busy && w.icon >= 0 && cmdicons_ok) {
						draw_cmdicon_fit(data, pitch, w.icon, r);
						const int pct = w.total > 0 ? 100 - (int)((long long)w.rem * 100 / w.total) : 0;
						hud3_bar(data, pitch, r.from.x + d3(3), r.to.y - d3(9), d3(38), d3(6), pct, 100, 0xff40d060u);
						if (b->build_queue.size() > 1) {
							char q[6]; snprintf(q, sizeof q, "+%d", (int)b->build_queue.size() - 1);
							draw_text_rgba(data, pitch, r.to.x - hud3_text_w(q) - d3(1), r.from.y, q, th.amber);
						}
					}
					hud3_bldg_slots.push_back({hud3_cr(267 + 50 * i, 4, 50, 52), uid});
					++i;
				}
				char line[32]; snprintf(line, sizeof line, "%d of %d busy", busy, total);
				rect lr = hud3_cr(270, 59, 244, 17);
				draw_text_rgba(data, pitch, lr.from.x, lr.from.y, line, 0xffa8b0b8u);
			} else {
				char total[24]; snprintf(total, sizeof total, "%d", (int)(wide ? wide_selection.size() : (size_t)own));
				rect tr = hud3_cr(474, 8, 40, 17);
				draw_text_rgba(data, pitch, tr.to.x - hud3_text_w(total), tr.from.y, total, th.amber);
				if (wide) {   // page dots for the 12-unit sub-cards
					const int pages = ((int)wide_selection.size() + 11) / 12;
					for (int p = 0; p < pages && p < 6; ++p) {
						rect dr = hud3_cr(514 - 10 * (pages < 6 ? pages : 6) + 10 * p, 62, 7, 7);
						fill_rect_rgba(data, pitch, dr, p == sel_page ? th.amber : 0xff4a525au);
					}
				}
			}
			if (hud3_info_expanded) {
				// the units themselves, 6 x 2 cells of 64 x 34 dp (the active sub-card)
				int i = 0;
				for (auto uid : current_selection) {
					unit_t* u = get_unit(uid); if (!u || u->owner != game_local_player) continue;
					if (i >= 12) break;
					rect r = UR(88 + 70 * (i % 6), 8 + 38 * (i / 6), 64, 34);
					fill_rect_rgba(data, pitch, r, th.btn_fill);
					line_rectangle_rgba(data, pitch, r, get_unit_id(u) == focused_unit_id ? th.amber : th.btn_line);
					blit_wireframe_unit(data, pitch, u, r.from.x + d3(17), r.from.y + d3(2), d3(30));
					selection_grid_hit.push_back({r, get_unit_id(u)});
					++i;
				}
			}
			return;
		}
		// ---- one unit or building
		unit_t* u = first;
		const bool bldg = ut_building(u);
		const bool mine = u->owner == game_local_player;
		{
			a_string name = hud3_short_name(u->unit_type);
			rect nr = hud3_cr(88, 8, 170, 17);
			draw_text_rgba(data, pitch, nr.from.x, nr.from.y, name.c_str(), 0xfff0f4f6u);
		}
		const hud3_work_t work = mine ? hud3_work(u) : hud3_work_t{};
		unit_t* transport = selected_transport_with_cargo();
		// train-capable: its card has a train / larva button (decides nothing visible when idle -- an idle
		// production building keeps its right side empty)
		// pools: HP, shields, energy on rows 25 / 42 / 59
		{
			const int barw = bldg ? 72 : 170, numx = bldg ? 168 : 266;
			int row = 0;
			auto pool = [&](int cur, int max, uint32_t col) {
				const int y = 25 + 17 * row++;
				rect b = hud3_cr(88, y + 5, barw, 7);
				hud3_bar(data, pitch, b.from.x, b.from.y, b.to.x - b.from.x, b.to.y - b.from.y, cur, max, col);
				char nb[24]; snprintf(nb, sizeof nb, "%d/%d", cur, max);
				rect n = hud3_cr(numx, y, 90, 17);
				draw_text_rgba(data, pitch, n.from.x, n.from.y, nb, col);
			};
			const int hpp = unit_hp_percent(u);
			pool((int)u->hp.integer_part(), (int)u->unit_type->hitpoints.integer_part(), hp_tint(hpp) | 0xff000000u);
			if (u->unit_type->has_shield) pool((int)u->shield_points.integer_part(), u->unit_type->shield_points, 0xff5cb0ffu);
			if (u->unit_type->flags & unit_type_t::flag_has_energy) pool((int)u->energy.integer_part(), (int)unit_max_energy(u).integer_part(), 0xffc884ffu);
			// a resource says what is left, on the next free row
			if (unit_is_mineral_field(u) || unit_is(u, UnitTypes::Resource_Vespene_Geyser) || unit_is(u, UnitTypes::Terran_Refinery)
			    || unit_is(u, UnitTypes::Protoss_Assimilator) || unit_is(u, UnitTypes::Zerg_Extractor)) {
				char line[40];
				const bool ore = unit_is_mineral_field(u);
				snprintf(line, sizeof line, "%s %d", ore ? "Minerals:" : "Gas:", (int)u->building.resource.resource_count);
				rect n = hud3_cr(88, 25 + 17 * (row < 2 ? row : 2), 170, 17);
				if (!work.busy || row < 2) draw_text_rgba(data, pitch, n.from.x, n.from.y, line, ore ? 0xff9ad0ffu : 0xff70e090u);
			}
		}
		// the caption row (59): what it is doing, the Nuclear Silo's state, the cargo count
		{
			const char* cap = work.busy ? work.caption : "";
			uint32_t capcol = 0xffd8dee2u;
			char cbuf[24];
			unit_t* silo = nullptr;
			if (unit_is(u, UnitTypes::Terran_Nuclear_Silo)) silo = u;
			if (silo && mine) {
				if (silo->building.silo.nuke && silo->building.silo.ready) { cap = "Nuke available"; capcol = 0xff30f030u; }
				else if (silo->building.silo.nuke || work.busy) { cap = "Arming Nuke"; capcol = th.amber; }
				else { cap = "Empty"; capcol = 0xffff4040u; }
			} else if (transport && !work.busy) {
				int n = 0, room = 0;
				for (unit_t* nu : loaded_units(transport)) { ++n; room += (int)nu->unit_type->space_required; }
				snprintf(cbuf, sizeof cbuf, "Cargo %d/%d", room, (int)transport->unit_type->space_provided);
				cap = cbuf; capcol = 0xffa8b0b8u;
				(void)n;
			}
			if (cap[0]) { rect cr = hud3_cr(88, 59, 170, 17); draw_text_rgba(data, pitch, cr.from.x, cr.from.y, cap, capcol); }
		}
		// the right side
		if (work.busy) {
			const uint32_t accent = race_accent(u);
			const bool queue = !u->build_queue.empty() && bldg && u_completed(u) && !ut_building(u->build_queue[0]);
			const int nslots = queue ? 5 : 1;
			for (int i = 0; i < nslots; ++i) {
				rect r = hud3_cr(270 + 50 * i, 8, 44, 44);
				const bool filled = i == 0 || i < (int)u->build_queue.size();
				slot_frame(r, filled, i == 0, accent);
				if (i == 0) {
					if (work.icon >= 0 && cmdicons_ok) draw_cmdicon_fit(data, pitch, work.icon, r);
					if (work.level > 0) {
						char lv[4]; snprintf(lv, sizeof lv, "%d", work.level);
						fill_rect_rgba(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.from.x + 1 + d3(14), r.from.y + 1 + d3(17)}}, 0xff101a20u);
						draw_text_rgba(data, pitch, r.from.x + d3(3), r.from.y + 1, lv, th.amber);
					}
				} else if (i < (int)u->build_queue.size() && cmdicons_ok) {
					draw_cmdicon_fit(data, pitch, (int)u->build_queue[(size_t)i]->id, r);
					blend_rect_rgba(data, pitch, {{r.from.x + 1, r.from.y + 1}, {r.to.x - 1, r.to.y - 1}}, th.dim_rgb, 110);   // waiting: dimmed
				}
				// only a training queue is cancelled slot by slot (a construction / egg slot does nothing)
				if (queue && i < (int)u->build_queue.size()) hud3_prod_slots.push_back(hud3_cr(267 + 50 * i, 4, 50, 52));
			}
			if (unit_is(u, UnitTypes::Terran_Nuclear_Silo) && u->building.silo.nuke && u->building.silo.ready) { /* armed: no bar */ }
			else hud3_progress(data, pitch, work, accent);
		} else if (transport && mine) {
			// cargo: five passengers in the slot positions, the fifth becomes "+N"
			int n = 0; for (unit_t* nu : loaded_units(transport)) { (void)nu; ++n; }
			int i = 0;
			for (unit_t* nu : loaded_units(transport)) {
				if (i >= 5) break;
				rect r = hud3_cr(270 + 50 * i, 8, 44, 44);
				slot_frame(r, true, false, th.btn_line);
				if (i == 4 && n > 5) {
					char more[6]; snprintf(more, sizeof more, "+%d", n - 4);
					hud3_text_c(data, pitch, r, (r.from.y + r.to.y) / 2, more, 0xffd0d8e0u);
				} else {
					blit_wireframe_unit(data, pitch, nu, r.from.x + d3(2), r.from.y + d3(2), d3(40));
					hud3_cargo_hit.push_back({hud3_cr(267 + 50 * i, 4, 50, 52), nu});
				}
				++i;
			}
		} else if (unit_is(u, UnitTypes::Terran_Nuclear_Silo) && mine && u->building.silo.nuke && u->building.silo.ready) {
			rect r = hud3_cr(270, 8, 44, 44);
			slot_frame(r, true, false, race_accent(u));
			if (cmdicons_ok) draw_cmdicon_fit(data, pitch, (int)UnitTypes::Terran_Nuclear_Missile, r);
		} else if (!bldg && u->owner < 12) {
			// kills on the name row, the equipment icons right-aligned at 514
			const weapon_type_t* gw = u->unit_type->ground_weapon; const weapon_type_t* aw = u->unit_type->air_weapon;
			const weapon_type_t* w = gw && gw->damage_amount > 0 ? gw : (aw && aw->damage_amount > 0 ? aw : nullptr);
			{
				char kl[24]; snprintf(kl, sizeof kl, "Kills: %d", (int)u->kill_count);
				rect kr = hud3_cr(270, 8, 90, 17);
				draw_text_rgba(data, pitch, kr.to.x - hud3_text_w(kl), kr.from.y, kl, 0xffa8b0b8u);
			}
			struct eq_t { UpgradeTypes up; int icon; const weapon_type_t* wpn; };
			static_vector<eq_t, 3> eqs;
			if (upgrade_icon(u->unit_type->armor_upgrade) > 0) eqs.push_back({u->unit_type->armor_upgrade, -1, nullptr});
			if (u->unit_type->has_shield) eqs.push_back({UpgradeTypes::Protoss_Plasma_Shields, -1, nullptr});
			if (w) eqs.push_back({w->damage_upgrade, (int)w->icon, w});
			const int n = (int)eqs.size();
			for (int i = 0; i < n; ++i) {
				const auto& e = eqs[(size_t)i];
				const int icon = e.icon >= 0 ? e.icon : upgrade_icon(e.up);
				if (icon <= 0) continue;
				rect r = hud3_cr(514 - 44 * n - 6 * (n - 1) + 50 * i, 8, 44, 44);
				fill_rect_rgba(data, pitch, r, th.btn_fill);
				if (cmdicons_ok) draw_cmdicon_fit(data, pitch, icon, r, equip_grey, 10);
				line_rectangle_rgba(data, pitch, r, th.btn_line);
				const int lvl = player_upgrade_level(u->owner, e.up);
				char lv[8]; snprintf(lv, sizeof lv, "%d", lvl);
				draw_text_rgba(data, pitch, r.to.x - hud3_text_w(lv) - d3(2), r.to.y - hud3_text_h(100) - d3(1), lv, lvl > 0 ? th.amber : 0xff98a0a8u);
				equip_hit.push_back({r, (int)e.up, e.wpn});
			}
		}
		if (hud3_info_expanded) {
			// extras: status lines on the left; the active item's name / the queue count; all the cargo
			int li = 0;
			auto status = [&](const char* s, uint32_t col) { if (li >= 4) return; rect r = UR(88, 8 + 17 * li++, 170, 17); draw_text_rgba(data, pitch, r.from.x, r.from.y, s, col); };
			if (transport && mine) {
				int i = 0;
				for (unit_t* nu : loaded_units(transport)) {
					if (i >= 8) break;
					rect r = UR(88 + 54 * i, 8, 48, 48);
					slot_frame(r, true, false, th.btn_line);
					blit_wireframe_unit(data, pitch, nu, r.from.x + d3(2), r.from.y + d3(2), d3(44));
					hud3_cargo_hit.push_back({r, nu});
					++i;
				}
			} else {
				if (ut_detector(u)) status("Detector", th.amber);
				if (u_hallucination(u)) status("Hallucination", 0xff8ad0ffu);
				if (u->parasite_flags) status("Parasite Detected", 0xffff8080u);
				if (u_disabled(u)) status("Disabled", 0xffa0a8b0u);
				const char* rank = unit_rank_name(u);
				if (rank && !bldg) status(rank, 0xffc8b878u);
				if (work.busy) {
					rect r1 = UR(270, 8, 244, 17), r2 = UR(270, 25, 244, 17);
					a_string what;
					if (!u->build_queue.empty()) what = hud3_short_name(u->build_queue[0], 24);
					else if (bldg && !u_completed(u)) what = hud3_short_name(u->unit_type, 24);
					if (!what.empty()) draw_text_rgba(data, pitch, r1.from.x, r1.from.y, what.c_str(), 0xfff0f4f6u);
					char l2[32] = "";
					if (!u->build_queue.empty() && bldg && u_completed(u)) snprintf(l2, sizeof l2, "Queue %d/5", (int)u->build_queue.size());
					else if (work.level > 0) snprintf(l2, sizeof l2, "Level %d", work.level);
					if (l2[0]) draw_text_rgba(data, pitch, r2.from.x, r2.from.y, l2, 0xffa8b0b8u);
				}
			}
		}
	}
	// Remaining-time progress (0..100) of what a building / egg is busy with, -1 none.
	int hud3_progress_pct(unit_t* u) {
		if (!u->build_queue.empty()) {
			int total = u->build_queue[0]->build_time;
			int rem = u->current_build_unit ? u->current_build_unit->remaining_build_time : u->remaining_build_time;
			if (!u->current_build_unit && rem == 0) return -1;   // a worker still walking to the site
			if (total > 0) return 100 - (int)((long long)rem * 100 / total);
		}
		if (!u_completed(u) && u->unit_type->build_time > 0) return 100 - (int)((long long)u->remaining_build_time * 100 / u->unit_type->build_time);
		if (u->building.researching_type && u->building.researching_type->research_time > 0)
			return 100 - (int)((long long)u->building.upgrade_research_time * 100 / u->building.researching_type->research_time);
		if (u->building.upgrading_type) {
			int lv = game_local_player >= 0 ? player_upgrade_level(game_local_player, (UpgradeTypes)u->building.upgrading_type->id) : 0;
			int total = u->building.upgrading_type->time_cost_base + u->building.upgrading_type->time_cost_factor * lv;
			if (total > 0) return 100 - (int)((long long)u->building.upgrade_research_time * 100 / total);
		}
		return -1;
	}

	// ===========================================================================
	// The command wheel (design 4.6 - 4.9)
	// ===========================================================================
	// W1 "ground": hold on the field with own units selected -- the commands that
	// need a point or a target, plus instant ones, on fixed axes: up ATTACK, left
	// MOVE, right PATROL, down HOLD (a worker: down BUILD, its HOLD moves to
	// down-right); the diagonals carry the unit's abilities in card order
	// (up-left 1, up-right 2, down-left 3, down-right 4; down-right is the one the
	// right hand covers, so left-handed swaps it with down-left). Releasing over a
	// slot is card_activate + game_click at the held point -- the same path as
	// "card, then tap". The outer band queues it (Shift).
	// W2 "unit": hold on an own unit or building -- its instant commands and its
	// production; the previous selection comes back after ordering.
	// Second level: BUILD / ADVANCED / LARVA open a ring in the same place.
	enum { wv_none = 0, wv_ping, wv_all_type, wv_to_group };
	struct hud3_slot_t { int card = -1; int virt = wv_none; };
	bool wheel_open = false;
	int wheel_kind = 0;              // 1 ground, 2 unit
	int wheel_level = 0;             // 0 commands, 1 a second ring (build list, larvae)
	int wheel_mode = 0;              // 0 finger held, 1 tap mode (latch / ring), 2 pen
	bool wheel_held = false;         // the opening finger / pen is still down
	xy wheel_c{0, 0}, wheel_pt{0, 0};  // centre (moved in from the edges) and the real point
	int wheel_n = 8;
	hud3_slot_t wheel_slot[9];
	int wheel_hover = -1;            // slot under the finger, -1 none
	bool wheel_hover_queue = false;  // in the outer band
	a_vector<unit_id> wheel_saved_sel;
	bool wheel_restore = false;      // give the saved selection back when done
	std::chrono::steady_clock::time_point wheel_hover_since{}, wheel_repeat_at{};
	int wheel_repeats = 0;
	bool hud3_force_queue = false;   // the outer band: order_queued() is true for this one order
	// The wheel was opened ON an own unit the selection can act on (a bunker or a
	// transport to enter, a gas building to mine, something an SCV can repair or
	// finish): the hub is then the DEFAULT order -- hold and let go, exactly the HUD
	// v2 hold (play-test: "units can no longer enter bunkers" -- a tap selects the
	// bunker, and the hold used to open the bunker's own wheel).
	bool wheel_hub_default = false;
	const char* wheel_hub_label = "";
	// What the selection would do to `t` by default, or nullptr if nothing special.
	const char* hud3_default_target_label(unit_t* t) {
		if (!t || t->owner != game_local_player) return nullptr;
		bool mobile = false, worker = false, scv = false;
		for (auto uid : current_selection) {
			unit_t* u = get_unit(uid);
			if (!u || u->owner != game_local_player || u == t) continue;
			if (!ut_building(u)) mobile = true;
			if (unit_is(u, UnitTypes::Terran_SCV) || unit_is(u, UnitTypes::Protoss_Probe) || unit_is(u, UnitTypes::Zerg_Drone)) worker = true;
			if (unit_is(u, UnitTypes::Terran_SCV)) scv = true;
		}
		if (!mobile || current_selection_is_selected(t)) return nullptr;
		if (t->unit_type->space_provided > 0 && u_completed(t)) return "ENTER";
		if (worker && u_completed(t) && (unit_is(t, UnitTypes::Terran_Refinery) || unit_is(t, UnitTypes::Protoss_Assimilator) || unit_is(t, UnitTypes::Zerg_Extractor))) return "GATHER";
		if (scv && unit_race(t) == race_t::terran) {
			if (ut_building(t) && !u_completed(t)) return "BUILD";
			const bool mech = ut_building(t) || ut_flag(t, unit_type_t::flag_mechanical);
			if (mech && t->hp < t->unit_type->hitpoints) return "REPAIR";
		}
		return nullptr;
	}

	// geometry in dp (design 4.6): hub r 44, slots 48-132, queue band 136-164
	static constexpr int wr_hub = 44, wr_in = 48, wr_out = 132, wr_band0 = 136, wr_band1 = 164;
	// What is at a point: -2 outside, -1 hub, 0..n-1 slot; *band = in the queue band.
	int wheel_at(int x, int y, bool* band = nullptr) {
		if (band) *band = false;
		const double dx = x - wheel_c.x, dy = y - wheel_c.y;
		const double r = std::sqrt(dx * dx + dy * dy) * 1000.0 / hud3_ppd_milli();
		if (r < wr_hub) return -1;
		if (r > wr_band1 + 12) return -2;
		double ang = std::atan2(dx, -dy) * 180.0 / 3.14159265358979;   // 0 = up, clockwise
		if (ang < 0) ang += 360.0;
		const double step = 360.0 / wheel_n;
		int k = (int)((ang + step / 2) / step) % wheel_n;
		if (r >= wr_band0 && band) *band = true;
		return k;
	}

	// Card index of the first button matching, or -1.
	template<typename F> int card_find(const static_vector<card_button_t, 9>& b, F&& f) {
		for (size_t i = 0; i < b.size(); ++i) if (b[i].action != cardact_none && f(b[i])) return (int)i;
		return -1;
	}
	// Fill the slots for the current wheel kind / level from card_buttons().
	void wheel_build_slots() {
		for (auto& s : wheel_slot) s = hud3_slot_t{};
		static_vector<card_button_t, 9> b;
		card_buttons(b);
		auto is = [&](const card_button_t& x, int act, int arg) { return x.action == act && x.arg == arg; };
		const int i_move = card_find(b, [&](const card_button_t& x) { return is(x, cardact_ability_target, (int)Orders::Move); });
		const int i_stop = card_find(b, [&](const card_button_t& x) { return x.action == cardact_stop; });
		const int i_attack = card_find(b, [&](const card_button_t& x) { return x.action == cardact_attack; });
		const int i_hold = card_find(b, [&](const card_button_t& x) { return is(x, cardact_ability, 43); });
		const int i_patrol = card_find(b, [&](const card_button_t& x) { return is(x, cardact_ability_target, (int)Orders::Patrol); });
		const int i_build = card_find(b, [&](const card_button_t& x) { return x.action == cardact_menu_build; });
		const int i_adv = card_find(b, [&](const card_button_t& x) { return x.action == cardact_menu_adv; });
		const int i_repair = card_find(b, [&](const card_button_t& x) { return is(x, cardact_ability_target, (int)Orders::Repair); });
		const int i_return = card_find(b, [&](const card_button_t& x) { return is(x, cardact_ability, 30); });
		auto reserved = [&](const card_button_t& x) {
			return x.action == cardact_cancel || x.action == cardact_place_cancel || x.action == cardact_liftoff
			    || x.action == cardact_land || x.action == cardact_menu_back;
		};
		const int SE = hud3_left ? 5 : 3, SW = hud3_left ? 3 : 5;   // the hand covers down-right
		if (wheel_level == 1) {
			// a second ring: every entry in card order from the top, clockwise
			int k = 0;
			for (size_t i = 0; i < b.size(); ++i) {
				if (b[i].action == cardact_none || b[i].action == cardact_menu_back) continue;
				if (k < 9) wheel_slot[k++].card = (int)i;
			}
			wheel_n = k > 8 ? 9 : 8;
			return;
		}
		wheel_n = 8;
		if (wheel_kind == 1) {
			if (i_stop < 0 && i_move < 0) {
				// nothing that moves is selected: the building's targeted commands
				// (Comsat SCAN...) or, with nothing selected at all, PING
				int k = 0;
				for (size_t i = 0; i < b.size() && k < 8; ++i)
					if (b[i].action == cardact_ability_target) wheel_slot[k++].card = (int)i;
				if (!first_own_selected_unit()) wheel_slot[0].virt = wv_ping;
				return;
			}
			const bool worker = i_build >= 0;
			wheel_slot[0].card = i_attack;
			wheel_slot[6].card = i_move;
			wheel_slot[2].card = i_patrol;
			static_vector<int, 9> extra;
			for (size_t i = 0; i < b.size(); ++i) {
				const int ii = (int)i;
				if (b[i].action == cardact_none || reserved(b[i])) continue;
				if (ii == i_move || ii == i_stop || ii == i_attack || ii == i_hold || ii == i_patrol || ii == i_build || ii == i_adv || ii == i_repair || ii == i_return) continue;
				if (b[i].action == cardact_research || b[i].action == cardact_upgrade || b[i].action == cardact_train) continue;
				extra.push_back(ii);
			}
			if (worker) {
				wheel_slot[4].card = i_build;
				wheel_slot[SW].card = i_adv;
				wheel_slot[SE].card = i_hold;
				wheel_slot[1].card = i_return;
				wheel_slot[7].card = i_repair >= 0 ? i_repair : (extra.empty() ? -1 : extra[0]);
			} else {
				wheel_slot[4].card = i_hold;
				const int diag[4] = {7, 1, SW, SE};
				for (size_t k = 0; k < extra.size() && k < 4; ++k) wheel_slot[diag[k]].card = extra[k];
			}
			return;
		}
		// W2: the unit / building under the finger
		const bool mobile = i_stop >= 0;
		if (mobile) {
			wheel_slot[6].card = i_stop;
			wheel_slot[2].card = i_hold;
			wheel_slot[4].virt = wv_all_type;
			wheel_slot[SW].virt = wv_to_group;
			const int order[4] = {0, 1, 7, SE};
			int k = 0;
			for (size_t i = 0; i < b.size() && k < 4; ++i) {
				if (b[i].action == cardact_ability && b[i].arg != 43) wheel_slot[order[k++]].card = (int)i;
				else if (b[i].action == cardact_morph) wheel_slot[order[k++]].card = (int)i;
			}
			return;
		}
		const int i_cancel = card_find(b, [&](const card_button_t& x) { return x.action == cardact_cancel; });
		const int i_lift = card_find(b, [&](const card_button_t& x) { return x.action == cardact_liftoff || x.action == cardact_land; });
		if (i_cancel >= 0) wheel_slot[4].card = i_cancel;
		if (i_lift >= 0) wheel_slot[SW].card = i_lift;
		const int fill[7] = {0, 1, 2, 3, 6, 7, 5};
		int k = 0;
		for (size_t i = 0; i < b.size(); ++i) {
			if (b[i].action == cardact_none || reserved(b[i])) continue;
			while (k < 7 && (wheel_slot[fill[k]].card >= 0 || wheel_slot[fill[k]].virt)) ++k;
			if (k >= 7) break;
			wheel_slot[fill[k++]].card = (int)i;
		}
	}
	int wheel_slot_count() { int n = 0; for (int k = 0; k < wheel_n; ++k) if (wheel_slot[k].card >= 0 || wheel_slot[k].virt) ++n; return n; }
	// A wheel with a single free command is not worth opening: a building with
	// nothing to target keeps the v2 hold (its rally), like before.
	bool wheel_single_free() {
		if (wheel_slot_count() != 1) return false;
		static_vector<card_button_t, 9> b; card_buttons(b);
		for (int k = 0; k < wheel_n; ++k) {
			if (wheel_slot[k].virt) return false;
			if (wheel_slot[k].card >= 0 && (size_t)wheel_slot[k].card < b.size()) {
				char c[24]; hud3_cost_text(b[(size_t)wheel_slot[k].card], c, sizeof c);
				return c[0] == 0;
			}
		}
		return false;
	}

	// Open the wheel for a hold at (x, y). Returns true when the hold was used.
	bool hud3_open_wheel_at(int x, int y, int mode) {
		if (!hud_v3 || !game_input) return false;
		wheel_saved_sel.clear(); wheel_restore = false;
		// on an own unit or building: W2
		xy wp = screen_to_world(x, y);
		unit_t* under = select_get_unit_at(wp);
		wheel_hub_default = false;
		const char* dflt = under ? hud3_default_target_label(under) : nullptr;
		if (dflt) {
			// the selection's wheel, aimed at this unit; letting go in the hub = the default order
			wheel_kind = 1;
			wheel_hub_default = true;
			wheel_hub_label = dflt;
		} else if (under && under->owner == game_local_player && unit_can_be_selected(under)) {
			if (!current_selection_is_selected(under)) {
				wheel_saved_sel = current_selection;
				wheel_restore = true;
				current_selection_clear();
				current_selection_add(under);
			}
			focused_unit_id = get_unit_id(under);
			wheel_kind = 2;
		} else {
			wheel_kind = 1;
		}
		wheel_level = 0;
		card_menu = 0;
		wheel_build_slots();
		if (wheel_slot_count() == 0 || (wheel_kind == 1 && wheel_single_free())) {
			// nothing worth a wheel: the plain v2 hold (default order / rally)
			if (wheel_restore) { current_selection = wheel_saved_sel; wheel_restore = false; }
			if (wheel_kind == 1) issue_default_order_at(x, y);
			return true;
		}
		wheel_open = true;
		wheel_mode = mode;
		wheel_held = mode != 1;
		wheel_pt = xy(x, y);
		// keep the whole wheel (with the band and the label above it) on screen
		const int R = d3(wr_band1 + 4), top = d3(wr_band1 + 40);
		int cx = x, cy = y;
		if (cx < R) cx = R;
		if (cx > (int)screen_width - R) cx = (int)screen_width - R;
		if (cy < top) cy = top;
		if (cy > (int)screen_height - R) cy = (int)screen_height - R;
		wheel_c = xy(cx, cy);
		wheel_hover = -1; wheel_hover_queue = false;
		wheel_repeats = 0;
		hud3_buzz(buzz_tick);
		return true;
	}
	void hud3_close_wheel(bool keep_selection) {
		if (!wheel_open) return;
		wheel_open = false;
		wheel_held = false;
		if (card_menu != 0 && !placing_type) card_menu = 0;
		if (wheel_restore && !keep_selection) current_selection = wheel_saved_sel;
		wheel_restore = false;
		wheel_hover = -1;
	}
	// The finger moved while the wheel is up.
	void hud3_wheel_motion(int x, int y) {
		bool band = false;
		int k = wheel_at(x, y, &band);
		if (k >= 0 && !(wheel_slot[k].card >= 0 || wheel_slot[k].virt)) k = -2;
		if (k != wheel_hover || band != wheel_hover_queue) {
			if (k >= 0) hud3_buzz(buzz_tick);
			wheel_hover = k; wheel_hover_queue = band;
			wheel_hover_since = std::chrono::steady_clock::now();
			wheel_repeats = 0;
		}
	}
	// Release (or a tap in tap mode) at (x, y): do what the slot says.
	void hud3_wheel_release(int x, int y) {
		const int repeats = wheel_repeats;
		hud3_wheel_motion(x, y);
		const int k = wheel_hover;
		const bool queue = wheel_hover_queue;
		wheel_held = false;
		if (k == -1 && wheel_level == 0 && wheel_hub_default) {
			// the hub over an own bunker / transport / gas building / repair target
			const xy pt = wheel_pt;
			hud3_close_wheel(true);
			issue_default_order_at(pt.x, pt.y);
			hud3_buzz(buzz_confirm);
			return;
		}
		if (k == -1 && wheel_level == 1) {   // the hub of a second ring = back
			wheel_level = 0;
			card_menu = 0;
			wheel_build_slots();
			wheel_mode = 1;
			return;
		}
		if (k < 0) {
			// let go in the hub or outside: nothing (design 4.6). Only the pen's
			// "button + touch" tap leaves the wheel up, to be tapped.
			if (k == -1 && wheel_mode == 2) { wheel_mode = 1; return; }
			hud3_close_wheel(false);
			return;
		}
		if (repeats > 0 && wheel_hover == k) { hud3_close_wheel(false); return; }   // a held production slot already ordered
		hud3_wheel_activate(k, queue);
	}
	void hud3_wheel_activate(int k, bool queue) {
		const hud3_slot_t s = wheel_slot[k];
		if (s.virt == wv_ping) {
			xy pp = screen_to_world(wheel_pt.x, wheel_pt.y);
			game_action_writer w;
			w.u8(88); w.u16((uint16_t)pp.x); w.u16((uint16_t)pp.y);
			submit_game_action(w);
			hud3_buzz(buzz_confirm);
			hud3_close_wheel(false);
			return;
		}
		if (s.virt == wv_all_type) {
			unit_t* u = first_own_selected_unit();
			if (u) select_all_of_type_on_screen(u->unit_type);
			wheel_restore = false;
			hud3_close_wheel(true);
			return;
		}
		if (s.virt == wv_to_group) { hud3_plus_tap(); wheel_restore = false; hud3_close_wheel(true); return; }
		static_vector<card_button_t, 9> b;
		card_buttons(b);
		if (s.card < 0 || (size_t)s.card >= b.size()) { hud3_close_wheel(false); return; }
		const card_button_t bt = b[(size_t)s.card];
		if (!bt.available) { hud3_buzz(buzz_error); wheel_mode = 1; return; }   // stays open: pick another
		if (hud3_submenu(bt)) {
			// BUILD / ADVANCED / LARVA: a second ring in the same place
			if (bt.action == cardact_select_larva && !wheel_restore) { wheel_saved_sel = current_selection; wheel_restore = true; }
			card_activate(s.card);
			wheel_level = 1;
			wheel_build_slots();
			wheel_mode = 1;   // the ring waits for a tap
			wheel_hover = -1;
			hud3_buzz(buzz_tick);
			return;
		}
		hud3_force_queue = queue;
		const bool targeted = bt.action == cardact_attack || bt.action == cardact_ability_target;
		bool keep = false;
		if (bt.action == cardact_build) {
			// the shadow appears where the wheel was opened; tick / cross confirm it
			card_activate(s.card);
			if (placing_type) game_click(wheel_pt.x, wheel_pt.y);
			keep = true;
		} else if (targeted) {
			attack_armed = false; ability_armed_order = -1;
			card_activate(s.card);
			game_click(wheel_pt.x, wheel_pt.y);
			keep = true;
		} else {
			card_activate(s.card);
			// a unit's own instant command keeps it selected; production (W2 on a
			// building) gives the player's selection back
			keep = wheel_kind == 2 && (bt.action == cardact_ability || bt.action == cardact_stop || bt.action == cardact_liftoff);
		}
		hud3_force_queue = false;
		hud3_buzz(buzz_confirm);
		if (wheel_kind == 1) keep = true;
		hud3_close_wheel(keep);
	}
	// Called once per frame: a held production slot orders one more every 300 ms
	// (after 600 ms), up to the five-slot queue.
	void hud3_tick() {
		if (!hud_v3) return;
		if (wheel_open && wheel_held && wheel_kind == 2 && wheel_level == 0 && wheel_hover >= 0) {
			const hud3_slot_t s = wheel_slot[wheel_hover];
			static_vector<card_button_t, 9> b;
			card_buttons(b);
			if (s.card >= 0 && (size_t)s.card < b.size() && b[(size_t)s.card].action == cardact_train) {
				auto now = std::chrono::steady_clock::now();
				if (now - wheel_hover_since > std::chrono::milliseconds(600) && now >= wheel_repeat_at && wheel_repeats < 5) {
					card_activate(s.card);
					++wheel_repeats;
					wheel_repeat_at = now + std::chrono::milliseconds(300);
					hud3_buzz(buzz_tick);
				}
			}
		}
		// the wheel's units died: close without an order
		if (wheel_open && wheel_level == 0 && !first_own_selected_unit() && !wheel_slot[0].virt) hud3_close_wheel(false);
		hud3_stick_scroll();
	}
	void select_all_of_type_on_screen(const unit_type_t* t) {
		rect view{screen_pos, screen_pos + xy((int)view_width, playfield_view_height())};
		current_selection_clear();
		for (unit_t* u : find_units(view)) {
			if (u->owner != game_local_player || u->unit_type != t || !unit_can_be_selected(u)) continue;
			current_selection_add(u);
		}
	}

	// ---- drawing the wheel ---------------------------------------------------------
	// Sector masks are cached per (size, slot count): one byte per pixel, so the
	// per-frame cost is a lookup, not an atan2, even at the FULL HD tier.
	a_vector<uint8_t> wheel_mask; int wheel_mask_r = 0, wheel_mask_n = 0, wheel_mask_ppd = 0;
	// 0 outside, 1 hub, 2 hub ring, 10+k slot k, 40+k slot k edge, 30 band, 31 band tick, 32 band edge
	void wheel_build_mask() {
		const int R = d3(wr_band1) + 1;
		if (R == wheel_mask_r && wheel_n == wheel_mask_n && hud3_ppd_milli() == wheel_mask_ppd && !wheel_mask.empty()) return;
		wheel_mask_r = R; wheel_mask_n = wheel_n; wheel_mask_ppd = hud3_ppd_milli();
		const int D = 2 * R + 1;
		wheel_mask.assign((size_t)D * D, 0);
		const double ppd = hud3_ppd_milli() / 1000.0;
		const double step = 360.0 / wheel_n, gap = 1.6;
		const double edge_dp = 1.2 / ppd;           // ~1 buffer pixel, in dp
		for (int y = 0; y < D; ++y) for (int x = 0; x < D; ++x) {
			const double dx = x - R, dy = y - R;
			const double r = std::sqrt(dx * dx + dy * dy) / ppd;
			uint8_t v = 0;
			double ang = std::atan2(dx, -dy) * 180.0 / 3.14159265358979; if (ang < 0) ang += 360.0;
			if (r < wr_hub - 2) v = 1;
			else if (r < wr_hub) v = 2;
			else if (r >= wr_in && r <= wr_out) {
				const double a = std::fmod(ang + step / 2, 360.0);
				const int k = (int)(a / step) % wheel_n;
				const double in_sector = a - k * step;
				const double edge_deg = edge_dp / (r > 1 ? r : 1) * 180.0 / 3.14159265358979;
				if (in_sector < gap / 2 || in_sector > step - gap / 2) v = 0;
				else if (r < wr_in + edge_dp || r > wr_out - edge_dp || in_sector < gap / 2 + edge_deg || in_sector > step - gap / 2 - edge_deg) v = (uint8_t)(40 + k);
				else v = (uint8_t)(10 + k);
			} else if (r >= wr_band0 && r <= wr_band1) {
				if (r < wr_band0 + edge_dp || r > wr_band1 - edge_dp) v = 32;
				else v = ((int)(ang / 4.0) % 2) ? 31 : 30;
			}
			wheel_mask[(size_t)y * D + x] = v;
		}
	}
	void wheel_slot_label(int k, char* out, size_t n, char* cost, size_t cn) {
		out[0] = 0; cost[0] = 0;
		const hud3_slot_t s = wheel_slot[k];
		if (s.virt == wv_ping) { snprintf(out, n, "%s", "PING"); return; }
		if (s.virt == wv_all_type) { snprintf(out, n, "%s", "ALL THESE"); return; }
		if (s.virt == wv_to_group) { snprintf(out, n, "%s", "TO GROUP"); return; }
		static_vector<card_button_t, 9> b; card_buttons(b);
		if (s.card < 0 || (size_t)s.card >= b.size()) return;
		const char* l = b[(size_t)s.card].label;
		snprintf(out, n, "%s", l ? l : "");
		size_t L = strlen(out); while (L && out[L - 1] == '!') out[--L] = 0;   // the "armed" marker
		hud3_cost_text(b[(size_t)s.card], cost, cn);
	}
	void draw_hud3_wheel(uint32_t* data, size_t pitch) {
		if (!wheel_open) return;
		wheel_build_mask();
		const hud_theme_t th = hud_theme();
		static_vector<card_button_t, 9> b;
		card_buttons(b);
		const int R = wheel_mask_r, D = 2 * R + 1;
		uint32_t fill[9], line[9];
		for (int k = 0; k < 9; ++k) {
			const hud3_slot_t s = k < wheel_n ? wheel_slot[k] : hud3_slot_t{};
			const bool has = s.card >= 0 || s.virt;
			const bool avail = s.virt || (s.card >= 0 && (size_t)s.card < b.size() && b[(size_t)s.card].available);
			const bool hov = k == wheel_hover && !wheel_hover_queue;
			fill[k] = !has ? 0xff0a0d10u : hov ? 0xff1f6b34u : avail ? (th.btn_fill | 0xff000000u) : 0xff101416u;
			line[k] = !has ? 0xff1a2024u : hov ? 0xff4fd46au : th.btn_line;
		}
		const bool band_hot = wheel_hover >= 0 && wheel_hover_queue;
		for (int y = 0; y < D; ++y) {
			const int py = wheel_c.y - R + y;
			if (py < 0 || py >= (int)screen_height) continue;
			uint32_t* row = data + (size_t)py * pitch;
			const uint8_t* m = &wheel_mask[(size_t)y * D];
			for (int x = 0; x < D; ++x) {
				const uint8_t v = m[x];
				if (!v) continue;
				const int px = wheel_c.x - R + x;
				if (px < 0 || px >= (int)screen_width) continue;
				uint32_t c;
				if (v == 1) c = 0xff05080au;
				else if (v == 2) c = 0xff000000u | (th.border & 0xffffffu);          // the hub ring in the race colour
				else if (v >= 10 && v < 19) c = fill[v - 10];
				else if (v >= 40 && v < 49) c = line[v - 40];
				else if (v == 30) c = band_hot ? 0xff2a3a20u : 0xff14181au;
				else if (v == 31) c = band_hot ? 0xff3c5a2cu : 0xff20262au;
				else c = band_hot ? 0xff4fd46au : 0xff3a4248u;
				row[px] = c;
			}
		}
		load_cmdicons();
		const double step = 360.0 / wheel_n;
		for (int k = 0; k < wheel_n; ++k) {
			const hud3_slot_t s = wheel_slot[k];
			if (s.card < 0 && !s.virt) continue;
			const double a = k * step * 3.14159265358979 / 180.0;
			const int mr = d3((wr_in + wr_out) / 2);
			const int cx = wheel_c.x + (int)std::lround(mr * std::sin(a)), cy = wheel_c.y - (int)std::lround(mr * std::cos(a));
			const int is = d3(30);
			rect ir = {{cx - is / 2, cy - is / 2 - d3(8)}, {cx + is / 2, cy + is / 2 - d3(8)}};
			const bool hov = k == wheel_hover && !wheel_hover_queue;
			if (s.virt == wv_ping) {
				for (int rr : {d3(4), d3(10)}) for (int g = 0; g < 360; g += 8) {
					double t = g * 3.14159265 / 180.0;
					int px = (ir.from.x + ir.to.x) / 2 + (int)(rr * std::cos(t)), py = (ir.from.y + ir.to.y) / 2 + (int)(rr * std::sin(t));
					mark_box(data, pitch, px - 1, py - 1, px + 1, py + 1, th.ping_edge, (int)screen_height);
				}
			} else if (s.card >= 0 && (size_t)s.card < b.size()) {
				const int icon = card_icon_frame(b[(size_t)s.card].action, b[(size_t)s.card].arg);
				if (icon >= 0 && cmdicons_ok) draw_cmdicon_fit(data, pitch, icon, ir, false, 6, hov ? 0xffffffu : th.icon_tint);
				hud3_corner_mark(data, pitch, {{ir.to.x - d3(2), ir.from.y}, {ir.to.x + d3(10), ir.from.y + d3(12)}}, hud3_instant(b[(size_t)s.card]), hud3_submenu(b[(size_t)s.card]));
			}
			char lbl[24], cost[24];
			wheel_slot_label(k, lbl, sizeof lbl, cost, sizeof cost);
			const bool avail = s.virt || (s.card >= 0 && (size_t)s.card < b.size() && b[(size_t)s.card].available);
			hud3_text_c(data, pitch, {{cx - d3(40), 0}, {cx + d3(40), 0}}, cy + d3(17), lbl, !avail ? 0xff5a626au : hov ? 0xffffffffu : 0xffd8dee2u, 100);
		}
		// the hub: the slot's name and cost (or BACK on a second ring)
		{
			char lbl[24] = "", cost[24] = "";
			if (wheel_hover >= 0) wheel_slot_label(wheel_hover, lbl, sizeof lbl, cost, sizeof cost);
			else if (wheel_level == 1) snprintf(lbl, sizeof lbl, "%s", "BACK");
			else if (wheel_hub_default) snprintf(lbl, sizeof lbl, "%s", wheel_hub_label);
			rect hr = {{wheel_c.x - d3(40), 0}, {wheel_c.x + d3(40), 0}};
			if (lbl[0]) hud3_text_c(data, pitch, hr, wheel_c.y - (cost[0] ? d3(7) : 0), lbl, 0xffffffffu, strlen(lbl) > 8 ? 100 : 100);
			if (cost[0]) {
				const bool ok = wheel_hover < 0 || wheel_slot[wheel_hover].card < 0 || hud3_affordable(b[(size_t)wheel_slot[wheel_hover].card]);
				hud3_text_c(data, pitch, hr, wheel_c.y + d3(9), cost, ok ? th.amber : 0xffff5a50u, 100);
			}
			// the echo above the wheel (the hand covers the lower half)
			if (wheel_hover >= 0 && lbl[0]) {
				char echo[40]; snprintf(echo, sizeof echo, "%s%s", lbl, wheel_hover_queue ? " +Q" : "");
				const int ew = hud3_text_w(echo, 100) + d3(20), eh = d3(26);
				rect er = {{wheel_c.x - ew / 2, wheel_c.y - d3(wr_band1) - d3(34)}, {wheel_c.x + ew / 2, wheel_c.y - d3(wr_band1) - d3(34) + eh}};
				fill_rect_rgba(data, pitch, er, 0xff102a18u);
				line_rectangle_rgba(data, pitch, er, 0xff4fd46au);
				hud3_text_c(data, pitch, er, (er.from.y + er.to.y) / 2, echo, 0xffffffffu, 100);
			}
			// "+ QUEUE" on the band
			hud3_text_c(data, pitch, {{wheel_c.x - d3(40), 0}, {wheel_c.x + d3(40), 0}}, wheel_c.y - d3((wr_band0 + wr_band1) / 2), "+ QUEUE", band_hot ? 0xffd6f5ddu : th.amber, 100);
		}
		// the real point, when the wheel had to move in from an edge
		if (std::abs(wheel_c.x - wheel_pt.x) + std::abs(wheel_c.y - wheel_pt.y) > d3(6)) {
			draw_line2(data, pitch, wheel_c.x, wheel_c.y, wheel_pt.x, wheel_pt.y, 0xf2c53du, true);
			mark_rect(data, pitch, {{wheel_pt.x - d3(7), wheel_pt.y - d3(7)}, {wheel_pt.x + d3(7), wheel_pt.y + d3(7)}}, 0xf2c53du);
		}
	}

	// ---- the growing ring under a held finger (from 80 ms until it opens) ----------
	bool hud3_press_active = false;
	xy hud3_press_xy{0, 0};
	std::chrono::steady_clock::time_point hud3_press_t{};
	void draw_hud3_press_ring(uint32_t* data, size_t pitch) {
		if (!hud3_press_active || wheel_open) return;
		const long long el = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - hud3_press_t).count();
		if (el < 80 || el > hud3_wheel_ms + 200) return;
		const double f = std::min(1.0, (double)(el - 80) / std::max(1, hud3_wheel_ms - 80));
		const int r = d3(20 + (int)(f * 40));
		const uint32_t c = 0xff000000u | (hud_theme().border & 0xffffffu);
		for (int a = 0; a < (int)(360 * f); a += 3) {
			double t = (a - 90) * 3.14159265 / 180.0;
			int px = hud3_press_xy.x + (int)std::lround(r * std::cos(t)), py = hud3_press_xy.y + (int)std::lround(r * std::sin(t));
			mark_box(data, pitch, px - d3(2), py - d3(2), px + d3(2), py + d3(2), c, (int)screen_height);
		}
	}

	// ---- targeting bar and placement tick / cross (design 4.6, 4.9) ---------------------
	rect hud3_bar_rect() {
		if (!hud_v3 || !(attack_armed || ability_armed_order >= 0 || placing_type)) return {};
		const int w = d3(560), h = d3(34);   // "BUILD ENGINEERING BAY - place it, then tick" + the cross
		const int y = d3(8) + usp_h() + d3(12);
		return {{(int)screen_width / 2 - w / 2, y}, {(int)screen_width / 2 + w / 2, y + h}};
	}
	rect hud3_bar_x() { rect r = hud3_bar_rect(); if (r.to.x <= r.from.x) return {}; return {{r.to.x - (r.to.y - r.from.y), r.from.y}, {r.to.x, r.to.y}}; }
	bool hud3_bar_hit(int x, int y) { return in3(hud3_bar_rect(), x, y); }
	void hud3_cancel_targeting() {
		attack_armed = false; ability_armed_order = -1;
		if (placing_type) { placing_type = nullptr; placing_order = nullptr; placing_pos = {-1, -1}; placing_land = false; placing_self = false; }
	}
	void draw_hud3_bar(uint32_t* data, size_t pitch) {
		rect r = hud3_bar_rect();
		if (r.to.x <= r.from.x) return;
		const hud_theme_t th = hud_theme();
		fill_rect_rgba(data, pitch, r, th.armed_fill);
		line_rectangle_rgba(data, pitch, r, th.armed_line);
		char txt[64];
		if (placing_type) {
			load_unit_names();
			a_string nm = unit_name(placing_type);
			size_t sp = nm.find(' ');
			a_string sh = sp != a_string::npos ? nm.substr(sp + 1) : nm;
			for (auto& ch : sh) ch = (char)std::toupper((unsigned char)ch);
			snprintf(txt, sizeof txt, "BUILD %s - place it, then tick", sh.c_str());
		} else if (attack_armed) snprintf(txt, sizeof txt, "%s", "ATTACK - tap a target");
		else {
			static_vector<card_button_t, 9> b; card_buttons(b);
			const char* l = "TARGET";
			for (auto& x : b) if (x.action == cardact_ability_target && x.arg == ability_armed_order) l = x.label;
			char clean[24]; snprintf(clean, sizeof clean, "%s", l);
			size_t L = strlen(clean); while (L && clean[L - 1] == '!') clean[--L] = 0;
			snprintf(txt, sizeof txt, "%s - tap a target", clean);
		}
		rect tx = {{r.from.x, r.from.y}, {r.to.x - (r.to.y - r.from.y), r.to.y}};
		hud3_text_c(data, pitch, tx, (r.from.y + r.to.y) / 2, txt, 0xfff2f2f2u, 100);
		rect x = hud3_bar_x();
		line_rectangle_rgba(data, pitch, x, th.armed_line);
		const int cx = (x.from.x + x.to.x) / 2, cy = (x.from.y + x.to.y) / 2, s = d3(7), t = d3(2) > 0 ? d3(2) : 1;
		hud3_stroke(data, pitch, cx - s, cy - s, cx + s, cy + s, 0xffff7068u, t);
		hud3_stroke(data, pitch, cx - s, cy + s, cx + s, cy - s, 0xffff7068u, t);
	}
	// The tick / cross beside the placement shadow: which 1 tick, 2 cross.
	rect hud3_place_rect(int which) {
		if (!hud_v3 || !placing_type || placing_pos.x < 0) return {};
		xy half = xy(placing_type->placement_size.x / 2, placing_type->placement_size.y / 2);
		auto w2s = [&](xy w) {
			return xy((int)((fp16::integer(w.x - screen_pos.x) * view_scale).integer_part()),
			          (int)((fp16::integer(w.y - screen_pos.y) * view_scale).integer_part()));
		};
		xy a = w2s(placing_pos - half), b = w2s(placing_pos + half);
		const int s = d3(56), g = d3(10), cy = (a.y + b.y) / 2;
		rect r = which == 1 ? rect{{b.x + g, cy - s / 2}, {b.x + g + s, cy + s / 2}} : rect{{a.x - g - s, cy - s / 2}, {a.x - g, cy + s / 2}};
		if (r.from.x < 0) { int d = -r.from.x; r.from.x += d; r.to.x += d; }
		if (r.to.x > (int)screen_width) { int d = r.to.x - (int)screen_width; r.from.x -= d; r.to.x -= d; }
		return r;
	}
	int hud3_place_hit(int x, int y) {
		if (in3(hud3_place_rect(1), x, y)) return 1;
		if (in3(hud3_place_rect(2), x, y)) return 2;
		return 0;
	}
	bool hud3_place_ok() {
		if (!placing_type || placing_pos.x < 0) return false;
		unit_t* builder = first_own_selected_unit();
		return builder && can_place_building(builder, game_local_player, placing_type, placing_pos, true, true);
	}
	xy hud3_place_screen() {
		return xy((int)((fp16::integer(placing_pos.x - screen_pos.x) * view_scale).integer_part()),
		          (int)((fp16::integer(placing_pos.y - screen_pos.y) * view_scale).integer_part()));
	}
	void draw_hud3_place(uint32_t* data, size_t pitch) {
		rect ok = hud3_place_rect(1), no = hud3_place_rect(2);
		if (ok.to.x <= ok.from.x) return;
		const bool can = hud3_place_ok();
		fill_rect_rgba(data, pitch, ok, can ? 0xff1f6b34u : 0xff1a2420u);
		line_rectangle_rgba(data, pitch, ok, can ? 0xff4fd46au : 0xff3a4a40u);
		const int t = d3(4) > 0 ? d3(4) : 1;
		{
			const int cx = (ok.from.x + ok.to.x) / 2, cy = (ok.from.y + ok.to.y) / 2, s = d3(13);
			const uint32_t c = can ? 0xffffffffu : 0xff5a6a60u;
			hud3_stroke(data, pitch, cx - s, cy, cx - s / 3, cy + s * 2 / 3, c, t);
			hud3_stroke(data, pitch, cx - s / 3, cy + s * 2 / 3, cx + s, cy - s * 2 / 3, c, t);
		}
		fill_rect_rgba(data, pitch, no, 0xff2a1113u);
		line_rectangle_rgba(data, pitch, no, 0xffe0564eu);
		{
			const int cx = (no.from.x + no.to.x) / 2, cy = (no.from.y + no.to.y) / 2, s = d3(11);
			hud3_stroke(data, pitch, cx - s, cy - s, cx + s, cy + s, 0xffff7068u, t);
			hud3_stroke(data, pitch, cx - s, cy + s, cx + s, cy - s, 0xffff7068u, t);
		}
	}

	// ---- latch, camera stick, dock drag ------------------------------------------------
	bool hud3_latch_armed = false, hud3_latch_locked = false;
	std::chrono::steady_clock::time_point hud3_latch_last{};
	void draw_hud3_latch(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		if (L.latch.to.x <= L.latch.from.x) return;
		const hud_theme_t th = hud_theme();
		const bool on = hud3_latch_armed || hud3_latch_locked;
		hud3_box(data, pitch, L.latch, on, hud_pressed(L.latch), on ? th.on_line : th.border);
		const int cx = (L.latch.from.x + L.latch.to.x) / 2, cy = L.latch.from.y + d3(17);
		for (int g = 0; g < 360; g += 6) {
			double t = g * 3.14159265 / 180.0;
			int px = cx + (int)(d3(10) * std::cos(t)), py = cy + (int)(d3(10) * std::sin(t));
			mark_box(data, pitch, px - 1, py - 1, px + 1, py + 1, on ? th.on_text : th.accent, (int)screen_height);
		}
		mark_box(data, pitch, cx - d3(2), cy - d3(2), cx + d3(2), cy + d3(2), on ? th.on_text : th.accent, (int)screen_height);
		hud3_text_c(data, pitch, L.latch, L.latch.to.y - d3(9), hud3_latch_locked ? "WHEEL*" : "WHEEL", on ? th.on_text : th.accent, 100);
	}
	void hud3_latch_tap() {
		auto now = std::chrono::steady_clock::now();
		const bool dbl = now - hud3_latch_last < std::chrono::milliseconds(400);
		hud3_latch_last = now;
		if (hud3_latch_locked) { hud3_latch_locked = false; hud3_latch_armed = false; return; }
		if (dbl && hud3_latch_armed) { hud3_latch_locked = true; hud3_latch_armed = false; hud3_buzz(buzz_confirm); return; }
		hud3_latch_armed = !hud3_latch_armed;
	}
	// camera stick: the knob's deflection is the scroll speed (design 4.10)
	bool hud3_stick_active = false;
	xy hud3_stick_d{0, 0};
	void draw_hud3_stick(uint32_t* data, size_t pitch, const hud3_layout_t& L) {
		if (L.stick.to.x <= L.stick.from.x) return;
		const hud_theme_t th = hud_theme();
		const int cx = (L.stick.from.x + L.stick.to.x) / 2, cy = (L.stick.from.y + L.stick.to.y) / 2, r = (L.stick.to.x - L.stick.from.x) / 2;
		for (int g = 0; g < 360; g += 3) {
			double t = g * 3.14159265 / 180.0;
			int px = cx + (int)(r * std::cos(t)), py = cy + (int)(r * std::sin(t));
			mark_box(data, pitch, px - 1, py - 1, px + 1, py + 1, th.border, (int)screen_height);
		}
		const int kx = cx + hud3_stick_d.x, ky = cy + hud3_stick_d.y, kr = d3(16);
		for (int y = -kr; y <= kr; ++y) {
			int w = (int)std::sqrt((double)(kr * kr - y * y));
			mark_box(data, pitch, kx - w, ky + y, kx + w + 1, ky + y + 1, hud3_stick_active ? th.on_line : th.btn_line, (int)screen_height);
		}
	}
	void hud3_stick_set(int x, int y) {
		hud3_layout_t L = hud3_layout();
		const int cx = (L.stick.from.x + L.stick.to.x) / 2, cy = (L.stick.from.y + L.stick.to.y) / 2, r = (L.stick.to.x - L.stick.from.x) / 2;
		int dx = x - cx, dy = y - cy;
		const double len = std::sqrt((double)dx * dx + (double)dy * dy);
		if (len > r && len > 0) { dx = (int)(dx * r / len); dy = (int)(dy * r / len); }
		hud3_stick_d = xy(dx, dy);
	}
	void hud3_stick_scroll() {
		if (!hud3_stick_active || game_local_player < 0) return;
		hud3_layout_t L = hud3_layout();
		const int r = (L.stick.to.x - L.stick.from.x) / 2;
		if (r <= 0) return;
		const int sp = 20 * camera_speed / 100;   // world px per frame at full deflection
		screen_pos.x += hud3_stick_d.x * sp / r;
		screen_pos.y += hud3_stick_d.y * sp / r;
	}
	int hud3_drag_card = -1;            // a dock key being dragged to the field
	xy hud3_drag_xy{-1, -1};
	bool hud3_drag_out = false;         // the finger has left the dock
	xy hud3_drag_aim() { return xy(hud3_drag_xy.x, hud3_drag_xy.y - d3(70)); }   // ~12 mm above the finger
	void draw_hud3_drag(uint32_t* data, size_t pitch) {
		if (hud3_drag_card < 0 || !hud3_drag_out) return;
		const xy a = hud3_drag_aim();
		const int s = d3(14);
		draw_line2(data, pitch, a.x - s, a.y, a.x - d3(4), a.y, 0xf2c53du);
		draw_line2(data, pitch, a.x + d3(4), a.y, a.x + s, a.y, 0xf2c53du);
		draw_line2(data, pitch, a.x, a.y - s, a.x, a.y - d3(4), 0xf2c53du);
		draw_line2(data, pitch, a.x, a.y + d3(4), a.x, a.y + s, 0xf2c53du);
	}
	// The dock key under a point: the grid through card_hit_test (its cells come
	// from hud2_card_cell, which is the dock in v3), the arc through its own test.
	int hud3_dock_hit(int x, int y) {
		if (hud3_arc_dock()) {
			int k = hud3_arc_key_at(x, y);
			if (k < 0) return -1;
			static_vector<card_button_t, 9> b; card_buttons(b);
			return k < (int)b.size() && b[(size_t)k].action != cardact_none ? k : -1;
		}
		return card_hit_test(x, y);
	}
	bool hud3_on_dock(int x, int y) {
		if (hud3_arc_dock()) return hud3_arc_key_at(x, y) >= 0;
		return in3(hud3_layout().dock, x, y);
	}

	// ---- the first-game hint (design 4.17) ------------------------------------------------
	rect hud3_hint_panel() { const int w = d3(700), h = d3(320); return {{(int)screen_width / 2 - w / 2, (int)screen_height / 2 - h / 2}, {(int)screen_width / 2 + w / 2, (int)screen_height / 2 + h / 2}}; }
	rect hud3_hint_ok() { rect p = hud3_hint_panel(); return {{p.to.x - d3(16) - d3(200), p.to.y - d3(16) - d3(48)}, {p.to.x - d3(16), p.to.y - d3(16)}}; }
	void draw_hud3_hint(uint32_t* data, size_t pitch) {
		if (!hud3_hint_pending) return;
		dim_rect_rgba(data, pitch, {{0, 0}, {(int)screen_width, (int)screen_height}}, 115);
		rect p = hud3_hint_panel();
		hud2_frame(data, pitch, p);
		draw_hud_tab(data, pitch, p, 0);
		const hud_theme_t th = hud_theme();
		draw_text_rgba(data, pitch, p.from.x + d3(20), p.from.y + d3(18), L("TOUCH HUD"), th.menu_title, 3);
		const char* lines[] = {
			L("Hold the ground: the command wheel opens under your finger."),
			L("Slide to a command and lift. The outer band queues it."),
			L("Hold your own unit or building: its commands and production."),
			L("SHIFT: tap for one order, double tap to lock, or hold it."),
			L("S-Pen button and touch: the wheel at once."),
			L("Every command is also on the dock, bottom right.")};
		int y = p.from.y + d3(66);
		for (const char* l : lines) { draw_text_rgba_scaled(data, pitch, p.from.x + d3(20), y, l, 0xffd0d8e0u, 100); y += hud3_text_h(100) + d3(9); }
		rect ok = hud3_hint_ok();
		fill_rect_rgba(data, pitch, ok, th.on_fill | 0xff000000u);
		line_rectangle_rgba(data, pitch, ok, th.on_line);
		hud3_text_c(data, pitch, ok, (ok.from.y + ok.to.y) / 2, L("Got it"), th.on_text);
	}
	void hud3_hint_dismiss() {
		hud3_hint_pending = false;
		if (!hud3_hint_flag_path.empty()) if (FILE* f = fopen(hud3_hint_flag_path.c_str(), "wb")) { fputs("1", f); fclose(f); }
	}

	// ===========================================================================
	// The frame and the input
	// ===========================================================================
	a_vector<std::pair<rect, unit_t*>> hud3_cargo_hit;   // passengers on the expanded card
	void draw_hud3(uint32_t* data, size_t pitch) {
		hud_scale_text = true;
		diag_in_hud = true;
		struct guard_t { bool* f; bool* g; ~guard_t() { *f = false; *g = false; } } _guard{&hud_scale_text, &diag_in_hud};
		const hud3_layout_t L = hud3_layout();
		hud3_cargo_hit.clear();
		// bottom right: the dock
		draw_hud3_dock(data, pitch, L);
		// bottom left: minimap, IDLE / ARMY / PING, the group rail, SHIFT
		hud2_frame(data, pitch, L.minimap);
		draw_minimap_scaled(data, pitch);
		draw_hud_tab(data, pitch, L.minimap, 2);
		draw_ping_marker(data, pitch);
		draw_under_attack_alert(data, pitch);
		draw_hud3_utils(data, pitch, L);
		draw_hud3_groups(data, pitch, L);
		draw_hud3_shift(data, pitch, L);
		draw_auto_worker_button(data, pitch);
		draw_hud3_latch(data, pitch, L);
		draw_hud3_stick(data, pitch, L);
		// bottom centre: the info card
		draw_hud3_card(data, pitch, L);
		// top row
		draw_hud3_top(data, pitch, L);
		draw_game_timer(data, pitch);
		if (hud_show_apm) draw_apm(data, pitch);
		draw_hud2_resources(data, pitch);
		draw_zoom_buttons(data, pitch);
		// targeting, placement, the wheel
		draw_hud3_bar(data, pitch);
		draw_hud3_place(data, pitch);
		draw_hud3_drag(data, pitch);
		draw_hud3_press_ring(data, pitch);
		draw_group_menu(data, pitch);
		draw_hud3_wheel(data, pitch);
		// overlays shared with v2
		draw_trigger_hud(data, pitch);
		draw_chat(data, pitch);
		draw_card_tooltip(data, pitch);
		draw_equip_tooltip(data, pitch);
		draw_hud3_hint(data, pitch);
		// DIAG: the corner groups, the card and the dock tile without overlapping
		if (bwgame::diag::enabled()) {
			struct { const char* id; rect r; } parts[5] = {
				{"minimap", L.minimap}, {"shift", L.shift}, {"card", L.card_full}, {"dock", L.dock}, {"menu", L.menu}};
			int pair = 0;
			for (int i = 0; i < 5; ++i) for (int j = i + 1; j < 5; ++j, ++pair) {
				const rect& a = parts[i].r; const rect& b = parts[j].r;
				int ox0 = std::max(a.from.x, b.from.x), oy0 = std::max(a.from.y, b.from.y);
				int ox1 = std::min(a.to.x, b.to.x), oy1 = std::min(a.to.y, b.to.y);
				if (ox1 > ox0 && oy1 > oy0 && diag_once(13, 100u + (uint32_t)pair))
					bwgame::diag::emit((long)st.current_frame, "hud", bwgame::diag::Sev::warn,
						"HUD_OVERLAP", "two HUD v3 parts that should not touch intersect",
						std::string("\"a\":\"") + parts[i].id + "\",\"b\":\"" + parts[j].id + "\"");
			}
		}
		draw_pause_menu(data, pitch);
	}

	// Pointer input for v3, before the shared handlers. Returns true when the event
	// was used here; false lets the HUD v2 / field code see it (the shared
	// controls -- minimap, IDLE / ARMY / PING, groups, MENU, the grid dock -- work
	// there through the same geometry getters).
	bool hud3_info_hit(int x, int y) { return in3(hud3_layout().card_full, x, y); }
	void hud3_focus_type(const unit_type_t* t) {
		for (auto uid : current_selection) { unit_t* u = get_unit(uid); if (u && u->unit_type == t) { focused_unit_id = uid; break; } }
		card_menu = 0; ability_armed_order = -1; attack_armed = false;
	}
	void hud3_keep_type(const unit_type_t* t, bool keep) {
		a_vector<unit_t*> picked;
		const a_vector<unit_id> sel = unlimited_sel_on() && !wide_selection.empty() ? wide_selection : current_selection;
		for (auto uid : sel) { unit_t* u = get_unit(uid); if (u && u->owner == game_local_player && ((u->unit_type == t) == keep)) picked.push_back(u); }
		if (picked.empty()) return;
		current_selection_clear();
		for (unit_t* u : picked) current_selection_add(u);
	}
	bool hud3_pen_down() { return wnd && (wnd.get_key_state(225) || wnd.get_key_state(229)); }
	bool hud3_pen_gesture = false;   // the current touch opened the wheel with the pen button
	std::chrono::steady_clock::time_point hud3_shift_hold_t{};
	bool hud3_shift_was_on = false;

	bool hud3_input(const native_window::event_t& e) {
		typedef native_window::event_t ev;
		if (!hud_v3 || !game_input || pause_screen != 0 || briefing_active || lobby_mode || chat_active) return false;
		const int x = e.mouse_x, y = e.mouse_y;
		if (hud3_hint_pending) {
			if (e.type == ev::type_mouse_button_up && e.button == 1 && in3(hud3_hint_ok(), x, y)) hud3_hint_dismiss();
			return e.type == ev::type_mouse_button_down || e.type == ev::type_mouse_button_up || e.type == ev::type_long_press
			    || e.type == ev::type_long_release || e.type == ev::type_mouse_motion;
		}
		const hud3_layout_t L = hud3_layout();
		switch (e.type) {
		case ev::type_touch_down:
			hud3_pen_gesture = false;
			if (wheel_open) return false;
			if (hud3_pen_wheel && hud3_pen_down() && !hud3_rect_at(x, y, true)) {
				// S-Pen side button + touch: the wheel at once (design 4.13)
				if (hud3_open_wheel_at(x, y, 2)) hud3_pen_gesture = true;
				return false;
			}
			hud3_press_active = !hud3_rect_at(x, y, true) && !placing_type;
			hud3_press_xy = xy(x, y);
			hud3_press_t = std::chrono::steady_clock::now();
			return false;
		case ev::type_hold_down:
			if (e.button == 0) {   // SHIFT held by a "HUD finger"
				hud3_shift_held = true;
				hud3_shift_was_on = queue_mode;
				hud3_shift_hold_t = std::chrono::steady_clock::now();
				queue_mode = true;
				hud_press_pos = xy(x, y);
			} else if (e.button == 1) { hud3_stick_active = true; hud3_stick_set(x, y); }
			return true;
		case ev::type_hold_motion:
			if (e.button == 1 && hud3_stick_active) hud3_stick_set(x, y);
			return true;
		case ev::type_hold_up:
			if (e.button == 0 && hud3_shift_held) {
				hud3_shift_held = false;
				hud_press_pos = xy(-1, -1);
				const bool short_tap = std::chrono::steady_clock::now() - hud3_shift_hold_t < std::chrono::milliseconds(300);
				queue_mode = hud3_shift_was_on;
				if (short_tap) hud3_shift_tap();
				else if (!hud3_shift_locked && !hud3_shift_once) queue_mode = false;
			} else if (e.button == 1) { hud3_stick_active = false; hud3_stick_d = xy(0, 0); }
			return true;
		case ev::type_mouse_button_down:
			if (e.button != 1) { hud3_press_active = false; return false; }   // a two-finger pan began
			hud3_press_active = false;
			if (wheel_open) return true;
			if (hud3_bar_hit(x, y) || hud3_place_hit(x, y)) return true;
			if (in3(L.chat, x, y) || in3(L.plus, x, y) || in3(L.latch, x, y) || hud3_info_hit(x, y)) return true;
			if (in3(L.stick, x, y)) { hud3_stick_active = true; hud3_stick_set(x, y); return true; }
			if (in3(L.shift, x, y)) return true;
			{
				const int k = hud3_dock_hit(x, y);
				if (k >= 0) {
					hud3_drag_card = k; hud3_drag_xy = xy(x, y); hud3_drag_out = false;
					card_hover_index = k;
					return hud3_arc_dock();   // the grid also goes through the v2 card code
				}
				if (hud3_on_dock(x, y)) return true;   // an empty dock slot: nothing behind it reacts
			}
			if ((hud3_latch_armed || hud3_latch_locked) && !hud3_rect_at(x, y, true)) return true;
			return false;
		case ev::type_mouse_motion:
			if (wheel_open && (e.button_state & 1)) { hud3_wheel_motion(x, y); return true; }
			if (wheel_open && hud3_pen_gesture) { hud3_wheel_motion(x, y); return true; }
			if (hud3_stick_active && (e.button_state & 1)) { hud3_stick_set(x, y); return true; }
			if (hud3_drag_card >= 0 && (e.button_state & 1)) {
				hud3_drag_xy = xy(x, y);
				hud3_drag_out = !hud3_on_dock(x, y);
				if (hud3_drag_out) card_hover_index = -1;
				return true;
			}
			if (e.button_state & 1) hud3_press_active = false;
			if (wheel_open) { hud3_wheel_motion(x, y); return false; }   // pen hover over the wheel
			return false;
		case ev::type_mouse_button_up: {
			if (e.button != 1) return false;
			hud3_press_active = false;
			if (wheel_open) {
				if (wheel_mode == 1) {
					const int k = wheel_at(x, y);
					if (k == -2) hud3_close_wheel(false);
					else hud3_wheel_release(x, y);
				} else hud3_wheel_release(x, y);
				hud_press_pos = xy(-1, -1);
				card_hover_index = -1;
				return true;
			}
			if (hud3_stick_active) { hud3_stick_active = false; hud3_stick_d = xy(0, 0); return true; }
			if (hud3_drag_card >= 0) {
				const int c = hud3_drag_card;
				const bool out = hud3_drag_out;
				const xy aim = hud3_drag_aim();
				hud3_drag_card = -1; hud3_drag_out = false;
				if (out) {
					// dragged from the dock to the field: the command at the aim point
					static_vector<card_button_t, 9> b; card_buttons(b);
					if (c < (int)b.size() && b[(size_t)c].available) {
						const bool targeted = b[(size_t)c].action == cardact_attack || b[(size_t)c].action == cardact_ability_target;
						attack_armed = false; ability_armed_order = -1;
						card_activate(c);
						if (targeted || placing_type) game_click(aim.x, aim.y);
						hud3_buzz(buzz_confirm);
					}
					hud_press_pos = xy(-1, -1);
					card_hover_index = -1;
					return true;
				}
				if (hud3_arc_dock()) {
					if (hud3_dock_hit(x, y) == c) card_activate(c);
					hud_press_pos = xy(-1, -1);
					card_hover_index = -1;
					return true;
				}
				return false;   // a tap on a grid cell: the v2 card code activates it
			}
			if (hud3_bar_hit(x, y)) { if (in3(hud3_bar_x(), x, y)) hud3_cancel_targeting(); return true; }
			if (const int ph = hud3_place_hit(x, y)) {
				if (ph == 2) hud3_cancel_targeting();
				else if (hud3_place_ok()) { const xy s = hud3_place_screen(); place_building_at(s.x, s.y); hud3_buzz(buzz_confirm); }
				else hud3_buzz(buzz_error);
				return true;
			}
			if (in3(L.chat, x, y)) { open_chat(); return true; }
			if (in3(L.plus, x, y)) { hud3_plus_tap(); return true; }
			if (in3(L.latch, x, y)) { hud3_latch_tap(); return true; }
			if (in3(L.shift, x, y)) { hud3_shift_tap(); return true; }
			if (hud3_info_hit(x, y)) {
				if (in3(hud3_card_expand_btn(), x, y)) { hud3_info_expanded = !hud3_info_expanded; return true; }
				if (in3(hud3_card_close_btn(), x, y)) { current_selection_clear(); wide_selection.clear(); sel_page = 0; return true; }
				for (auto& t : hud3_tokens) if (in3(t.r, x, y)) {
					if (e.clicks >= 2) hud3_keep_type(t.type, true); else hud3_focus_type(t.type);
					play_unit_what(first_own_selected_unit());
					return true;
				}
				for (size_t i = 0; i < hud3_prod_slots.size(); ++i) if (in3(hud3_prod_slots[i], x, y)) {
					if (submit_select_action()) { game_action_writer w; w.u8(32); w.u16((uint16_t)i); submit_game_action(w); }
					return true;
				}
				for (auto& cg : hud3_cargo_hit) if (in3(cg.first, x, y)) { unload_single(cg.second); return true; }
				for (auto& bs : hud3_bldg_slots) if (in3(bs.r, x, y)) {
					if (unit_t* bu = get_unit(bs.uid)) { current_selection_clear(); current_selection_add(bu); play_unit_what(bu); }
					return true;
				}
				if (unit_t* gu = selection_grid_unit_at(x, y)) { focused_unit_id = get_unit_id(gu); card_menu = 0; ability_armed_order = -1; play_unit_what(gu); return true; }
				if (unlimited_sel_on() && wide_selection.size() > 12) {   // the page dots
					const int pages = ((int)wide_selection.size() + 11) / 12;
					sel_page = (sel_page + 1) % (pages > 0 ? pages : 1);
					rebuild_active_page();
				}
				return true;
			}
			{
				int gn;
				if (e.clicks >= 2 && group_button_hit(x, y, gn)) {
					// double tap on a group: recall it and look at it
					recall_control_group(gn);
					if (unit_t* u = first_own_selected_unit()) center_camera_on(u->sprite->position);
					return true;
				}
			}
			if ((hud3_latch_armed || hud3_latch_locked) && !hud3_rect_at(x, y, true)) {
				hud3_latch_armed = false;
				hud3_open_wheel_at(x, y, 1);
				return true;
			}
			return false;
		}
		case ev::type_long_press:
			hud3_press_active = false;
			if (wheel_open) return true;
			if (hud3_drag_card >= 0) { card_hover_index = hud3_drag_card; return true; }   // hold on a dock key: its tooltip
			if (hud3_info_hit(x, y)) {
				for (auto& t : hud3_tokens) if (in3(t.r, x, y)) { hud3_keep_type(t.type, false); return true; }
				return false;   // the equipment tooltip and the like: the shared hold handler
			}
			if (hud3_rect_at(x, y, true)) return false;   // groups, minimap, tooltips: the shared handlers
			if (placing_type || attack_armed || ability_armed_order >= 0) return false;
			return hud3_open_wheel_at(x, y, 0);
		case ev::type_long_motion:
			if (wheel_open) { hud3_wheel_motion(x, y); return true; }
			if (hud3_drag_card >= 0) { hud3_drag_xy = xy(x, y); hud3_drag_out = !hud3_on_dock(x, y); return true; }
			return false;
		case ev::type_long_release:
			if (wheel_open) { hud3_wheel_release(x, y); hud_press_pos = xy(-1, -1); return true; }
			if (hud3_drag_card >= 0) {
				// held on a dock key (its tooltip), then let go: a drag out still commands
				native_window::event_t up = e; up.type = ev::type_mouse_button_up; up.button = 1;
				return hud3_input(up);
			}
			return false;
		case ev::type_touch_cancel:
			hud3_press_active = false;
			hud3_drag_card = -1; hud3_drag_out = false;
			if (wheel_open) hud3_close_wheel(false);
			return false;
		case ev::type_key_down:
			if (e.scancode == 270 || e.sym == 27) {
				// hardware Back: close the wheel / drop the targeting first (design 4.13)
				if (wheel_open) { hud3_close_wheel(false); return true; }
				if (attack_armed || ability_armed_order >= 0 || placing_type) { hud3_cancel_targeting(); return true; }
			}
			return false;
		default:
			return false;
		}
	}
	// The S-Pen in the air (design 4.13, 14.4): the side button pressed while the pen
	// hovers opens the wheel under it; moving highlights; letting the button go
	// picks; touching the glass or leaving the hover range cancels.
	bool hud3_air_btn = false, hud3_air_wheel = false;
	void hud3_pen_air(bool in_range, int x, int y, bool button) {
		if (!hud_v3 || !hud3_pen_wheel || !game_input || pause_screen != 0 || chat_active || hud3_hint_pending) { hud3_air_btn = button; return; }
		if (!in_range) {
			if (wheel_open && hud3_air_wheel) hud3_close_wheel(false);
			hud3_air_wheel = false; hud3_air_btn = false;
			return;
		}
		if (button && !hud3_air_btn) {
			// pressed in the air
			if (!wheel_open) {
				if (!hud3_rect_at(x, y, true) && !placing_type && hud3_open_wheel_at(x, y, 0) && wheel_open) hud3_air_wheel = true;
			} else if (hud3_air_wheel) hud3_wheel_motion(x, y);   // a second ring: press again, release picks
		} else if (button && wheel_open && hud3_air_wheel) {
			hud3_wheel_motion(x, y);
		} else if (!button && hud3_air_btn && wheel_open && hud3_air_wheel) {
			hud3_wheel_release(x, y);
			if (!wheel_open) hud3_air_wheel = false;
		}
		hud3_air_btn = button;
	}

	// The rectangles sdl2.cpp keeps out of the gesture state machine: a finger that
	// lands on SHIFT (0) or the camera stick (1) is a "HUD finger" -- held while the
	// other hand works on the field, never part of a two-finger pan.
	void hud3_publish_hold_rects() {
		if (!wnd) return;
		if (!hud_v3 || !game_input || pause_screen != 0) { wnd.set_hold_rects(nullptr, 0); return; }
		hud3_layout_t L = hud3_layout();
		int r[8] = {L.shift.from.x, L.shift.from.y, L.shift.to.x, L.shift.to.y, L.stick.from.x, L.stick.from.y, L.stick.to.x, L.stick.to.y};
		wnd.set_hold_rects(r, L.stick.to.x > L.stick.from.x ? 2 : 1);
	}
