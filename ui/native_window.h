#ifndef NATIVE_WINDOW_H
#define NATIVE_WINDOW_H

#include <memory>

namespace native_window {
	struct window_impl;
	
	struct event_t {
		enum {
			type_none,
			type_quit,
			type_key_down,
			type_key_up,
			type_resize,
			type_mouse_button_down,
			type_mouse_button_up,
			type_mouse_motion,
			// Touch long-press (Android): a contextual order in game mode
			// (like right-click in the original), ignored by the replay UI.
			type_long_press,
			// Pinch zoom (Android): button = +1 (zoom in) / -1 (zoom out);
			// mouse_x/mouse_y = pinch center in render pixels.
			type_zoom,
			// UTF-8 text from the IME / keyboard (chat input); payload in .text.
			type_text_input,
			// Touch (Android): the finger that produced type_long_press moved /
			// lifted. Lets a held control offer choices to slide to (the control-
			// group menu). mouse_x/mouse_y = the finger, in render pixels.
			type_long_motion,
			type_long_release,
			// Touch (Android): the one-finger gesture in progress (a drag-select, a
			// minimap / timeline drag, a press on a HUD button) is ABANDONED -- a
			// second finger landed and the gesture became a two-finger pan. It must
			// end without any effect: no selection, no click, no order.
			type_touch_cancel,
			// Touch (Android, HUD v3): the first finger of a gesture landed (sent
			// before anything is decided -- the wheel's "growing ring" starts here).
			type_touch_down,
			// Touch (Android, HUD v3): a "HUD finger" -- one that landed on a hold
			// rectangle (set_hold_rects) -- went down / moved / lifted. It is kept out
			// of the tap / drag / two-finger state machine, so a thumb can hold SHIFT
			// while the other hand works on the field. button = the rectangle's index.
			type_hold_down,
			type_hold_motion,
			type_hold_up
		};
		int type = type_none;
		int sym = -1;
		int scancode = -1;
		// UTF-8 committed text for type_text_input (SDL caps this at 32 bytes).
		char text[32] = {};
		int width = -1;
		int height = -1;
		int button = -1;
		int mouse_x = -1;
		int mouse_y = -1;
		int button_state = 0;
		int mouse_xrel = -1;
		int mouse_yrel = -1;
		int clicks = -1;
	};

	// Divisor between the window pixel size and the render/present size
	// (1 = native, 2 = half). Takes effect for windows created afterwards.
	void set_render_scale(int scale);
	// Touch: how long a still finger is held before type_long_press (ms).
	void set_long_press_ms(int ms);

	// Returns (and clears) whether the OS signalled low memory since the
	// last call; the caller should free what it can.
	bool consume_low_memory();

	struct window {
		std::unique_ptr<window_impl> impl;
		window();
		~window();
		window(window&&);
		void destroy();
		bool create(const char* title, int x, int y, int width, int height);
		// Actual render/present size (window pixels / render scale); on
		// Android the window ignores the requested size and goes fullscreen,
		// so query this after create() instead of assuming.
		void get_render_size(int* width, int* height);
		void get_cursor_pos(int* x, int* y);
		// A render-space rect where a single-finger press starts a left-button
		// drag immediately (no tap/long-press) - used so the minimap viewport
		// can be slid with finger/S-Pen. Set to an empty rect to disable.
		void set_immediate_drag_rect(int x0, int y0, int x1, int y1);
		// Render-space rectangles (x0 y0 x1 y1, n of them) whose fingers become
		// "HUD fingers" (type_hold_*). n = 0 clears them.
		void set_hold_rects(const int* xyxy, int n);
		bool peek_message(event_t& e);
		// Chat input: raise/hide the soft keyboard and toggle SDL text events.
		void start_text_input();
		void stop_text_input();
		bool show_cursor(bool show);
		bool get_key_state(int scancode);
		bool get_mouse_button_state(int button);
		void update_surface();
		void set_bloom(int strength); // open1161HD Faze 7: GPU bloom (0 = off)
		// SP shader pipeline: GLES2 present stage (off = classic). effect: -1 AUTO
		// (sharpen on Adreno, identity elsewhere), 0 identity, 1 sharpen. strength 0..1.
		void set_post_gl(bool on, int effect = -1, float strength = 0.35f, float bloom = 0.f, float grade = 0.f);
		explicit operator bool() const;
	};
}

#endif

