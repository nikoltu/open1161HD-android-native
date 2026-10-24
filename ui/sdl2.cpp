#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"

#include "common.h"
#include "SDL.h"
#ifndef OPENBW_NO_SDL_IMAGE
#include "SDL_image.h"
#endif
#ifndef OPENBW_NO_SDL_MIXER
#include "SDL_mixer.h"
#endif

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <csignal>
#ifdef __ANDROID__
#include <deque>
#include <vector>
#include <jni.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#define OB_POST_GL 1          // SP shader pipeline available (GLES2 present stage)
#endif

using bwgame::ui::log;
using bwgame::ui::fatal_error;

namespace native_window {

#ifdef __ANDROID__
int default_render_scale = 2;
#else
int default_render_scale = 1;
#endif

void set_render_scale(int scale) {
	if (scale < 1) scale = 1;
	if (scale > 4) scale = 4;
	default_render_scale = scale;
}

// HUD v3: the hold time before a long press (the command wheel's delay setting).
static unsigned g_long_press_ms = 400;
void set_long_press_ms(int ms) {
	if (ms < 150) ms = 150;
	if (ms > 1000) ms = 1000;
	g_long_press_ms = (unsigned)ms;
}

// Set from the SDL thread when SDL_APP_LOWMEMORY arrives; the main loop
// consumes it to shrink its memory (dropping cached seek states).
bool low_memory_flag = false;

bool consume_low_memory() {
	bool r = low_memory_flag;
	low_memory_flag = false;
	return r;
}

bool sdl_initialized = false;
void sdl_init() {
	if (!sdl_initialized) {
#ifndef OPENBW_NO_SDL_IMAGE
		IMG_Init(IMG_INIT_PNG);
#endif
		auto original_handler = signal(SIGINT, SIG_DFL);
		if (SDL_Init(SDL_INIT_VIDEO) == 0) {
			sdl_initialized = true;
		} else {
			log("SDL_Init failed: %s\n", SDL_GetError());
		}
		signal(SIGINT, original_handler);
	}
}


struct window_impl {

	SDL_Window* window = nullptr;

	// Presentation goes through an SDL_Renderer: the UI draws into an
	// offscreen RGBA surface at render_w x render_h (the window size divided
	// by render_scale), update_surface() streams it into a texture and the
	// GPU upscales to the full window with vsync. On Android BW-era art at
	// native tablet resolution is unreadably small, hence the divisor.
	SDL_Renderer* renderer = nullptr;
	SDL_Texture* texture = nullptr;
	int tex_w = 0;
	int tex_h = 0;
	// open1161HD Faze 7: optional GPU bloom (SDL_Renderer render-target; no raw
	// GLES/shaders). A quarter-res bilinear downscale of the frame is additively
	// blended back, so the brightest (emissive) areas bloom softly. bloom_strength
	// 0 = OFF (default) -> the present path is byte-for-byte the classic RenderCopy.
	SDL_Texture* bloom_tex = nullptr;
	int bloom_w = 0, bloom_h = 0;
	int bloom_strength = 0;   // 0..255 additive alpha; 0 disables
	// open1161HD Faze 7+: bright-pass threshold. A real SC:R bloom glows only the
	// bright areas (weapon flashes, psi, shields), not the whole frame. After the
	// downscale we subtract this constant from the bloom buffer with a custom
	// REV_SUBTRACT blend, crushing everything below it to black so only the excess
	// over the threshold blooms. 0 -> old uniform-haze behaviour.
	int bloom_thresh = 120;   // 0..255 luma cutoff
	SDL_BlendMode bloom_subtract = SDL_BLENDMODE_INVALID; // built lazily; INVALID if unsupported
	// SP shader pipeline (Phase 1): a GLES2 post-process present stage. Binds the
	// streaming texture via SDL_GL_BindTexture, draws a full-screen triangle through a
	// fragment shader, presents to FBO 0. Phase 1 uses an IDENTITY shader (+ V-flip) so
	// the output is byte-identical to RenderCopy -- it exists to prove the GL bridge.
	// Any failure -> post_gl_ok=false and the classic RenderCopy path runs (never black).
	bool post_gl_enabled = false;   // user toggle (default OFF)
	bool post_gl_ok = false;        // compiled + ready this session
	bool post_gl_tried = false;     // init attempted (don't retry every frame)
	// SP Phase 2: post-process effect selection. 0 = identity pass-through (Phase 1),
	// 1 = sharpen/crispen. -1 = AUTO: pick per GPU at init (sharpen on Adreno, identity
	// on others) so it ships on by default only on the faster tablet (S8U), off on Mali
	// (S7+) until the on-device perf A/B. post_strength scales the effect (0..1).
	int post_effect = -1;
	float post_strength = 0.35f;
	float post_grade = 0.f;         // SE color-grade strength 0..1 (0 = neutral no-op)
#ifdef OB_POST_GL
	// Identity present program (Phase 1): flips V, .bgra-swizzles the ARGB8888 source.
	GLuint post_prog = 0, post_vbo = 0;
	GLint post_a_pos = -1, post_u_tex = -1, post_u_grade = -1;
	// Sharpen program (reads the SDL source, no flip, .bgr swizzle) -> intermediate FBO.
	GLuint post_prog_sharp = 0;
	GLint post_sharp_pos = -1, post_sharp_tex = -1, post_sharp_texel = -1, post_sharp_amt = -1;
	// Final blit program (identity, flips V, no swizzle) for our own RGBA FBO textures.
	GLuint post_prog_blit = 0;
	GLint post_blit_pos = -1, post_blit_tex = -1, post_blit_grade = -1;
	// Intermediate render target (ping-pong scaffold; Phase 2 uses one, Phase 3 chains).
	GLuint post_fbo = 0, post_fbo_tex = 0;
	int post_fbo_w = 0, post_fbo_h = 0;
	bool post_is_adreno = false;    // GPU family probed at init (drives AUTO)
	bool post_sharp_ready = false;  // sharpen + blit programs built OK
	// SP Phase 3: multi-pass GPU bloom (bright-pass -> separable blur -> additive composite),
	// composed after the base/sharpen pass through the FBO chain -- a real GLES bloom that
	// replaces the SDL_ComposeCustomBlendMode approximation when the GL present stage runs.
	float post_bloom = 0.f;         // bloom strength 0..1 (0 = off)
	GLuint post_prog_bright = 0, post_prog_blur = 0, post_prog_comp = 0;
	GLint post_bright_pos = -1, post_bright_tex = -1, post_bright_thr = -1;
	GLint post_blur_pos = -1, post_blur_tex = -1, post_blur_dir = -1;
	GLint post_comp_pos = -1, post_comp_base = -1, post_comp_bloom = -1, post_comp_amt = -1, post_comp_grade = -1;
	GLuint post_bloomfbo[2] = {0, 0}, post_bloomtex[2] = {0, 0}; // half-res ping-pong
	int post_bloom_w = 0, post_bloom_h = 0;
	bool post_bloom_ready = false;  // bloom programs built OK
#endif
	SDL_Surface* present_source = nullptr;
	int render_scale;
	int render_w = 0;
	int render_h = 0;

	window_impl() : render_scale(default_render_scale) {
		sdl_init();
	}
	~window_impl() {
		destroy();
	}

	void destroy() {
#ifdef OB_POST_GL
		// Tear down GL objects while the renderer's context is still current, and reset
		// the lazy-init flags so a reused window_impl re-creates them against the next
		// context instead of glUseProgram'ing a stale handle with post_gl_ok still true.
		post_gl_teardown();
		post_gl_tried = false;
#endif
		if (texture) {
			SDL_DestroyTexture(texture);
			texture = nullptr;
		}
		if (renderer) {
			SDL_DestroyRenderer(renderer);
			renderer = nullptr;
		}
		if (window) {
			SDL_DestroyWindow(window);
			window = nullptr;
		}
	}

	void update_render_size() {
		int w = 0;
		int h = 0;
		if (renderer) SDL_GetRendererOutputSize(renderer, &w, &h);
		else if (window) SDL_GetWindowSize(window, &w, &h);
		if (w < 1 || h < 1) return;
		render_w = w / render_scale < 1 ? 1 : w / render_scale;
		render_h = h / render_scale < 1 ? 1 : h / render_scale;
		if (renderer) SDL_RenderSetLogicalSize(renderer, render_w, render_h);
	}

	void invalidate_texture() {
		if (texture) {
			SDL_DestroyTexture(texture);
			texture = nullptr;
		}
		if (bloom_tex) {
			SDL_DestroyTexture(bloom_tex);
			bloom_tex = nullptr; bloom_w = bloom_h = 0;
		}
	}

	bool create(const char* title, int x, int y, int width, int height) {
		if (window) fatal_error("window already created");
		Uint32 flags = 0;
		flags |= SDL_WINDOW_RESIZABLE;
		window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
		if (!window) log("SDL_CreateWindow failed: %s\n", SDL_GetError());
		if (window) {
			renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
			if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
			if (!renderer) {
				log("SDL_CreateRenderer failed: %s\n", SDL_GetError());
				SDL_DestroyWindow(window);
				window = nullptr;
				return false;
			}
			update_render_size();
			// No SDL_StartTextInput(): the game is touch/gesture driven and
			// text input pops up the Android soft keyboard over the screen.
		}
		return window != nullptr;
	}

	void get_cursor_pos(int* x, int* y) {
		int wx = 0;
		int wy = 0;
		SDL_GetMouseState(&wx, &wy);
		if (renderer) {
			float lx = 0.0f;
			float ly = 0.0f;
			SDL_RenderWindowToLogical(renderer, wx, wy, &lx, &ly);
			*x = (int)lx;
			*y = (int)ly;
		} else {
			*x = wx;
			*y = wy;
		}
	}
	
	std::array<bool, 512> key_state {};
	std::array<bool, 6> mouse_button_state {};

#ifdef __ANDROID__
	// Touch gesture state machine (deferred commit, VCMI-style). Nothing is
	// sent to the UI until the gesture is disambiguated:
	//   tap                -> left click (select); double tap = double click
	//   drag > threshold   -> left-button drag from the ORIGINAL touch point
	//                         (drag-select box, minimap/slider dragging)
	//   hold ~600 ms still -> type_long_press (contextual order in melee)
	//   two fingers        -> right-button drag (camera pan), centroid-based
	// Gesture discrimination thresholds are in physical window pixels so
	// they don't change with the render scale; event positions are emitted
	// in logical render coordinates.
	std::deque<event_t> pending_events;
	struct finger_state {
		SDL_FingerID id;
		int px; // physical window px
		int py;
		int rx; // logical render px
		int ry;
	};
	std::vector<finger_state> fingers;

	enum class touch_state_t { idle, down_short, dragging, long_fired, panning };
	touch_state_t touch_state = touch_state_t::idle;

	static constexpr int touch_drag_threshold_px = 24;   // physical px
	static constexpr int touch_double_tap_dist_px = 96;  // physical px
	static constexpr Uint32 touch_double_tap_ms = 400;
	// #12: hold time to promote a stationary press into a long-press (= order /
	// build-place / tooltip). Was 600ms, which felt sluggish. Lowered to 400ms:
	// noticeably snappier while still above the ~300ms band where a deliberate but
	// slow *stationary* tap (finger down, pause, up) would be misread as an order.
	// Drag-select does not collide -- any real drag trips touch_drag_threshold_px
	// (24px) well before this fires. Android's own long-press default is ~500ms,
	// so 400 sits in a familiar range. Tune here if holds still feel off.
	// HUD v3: adjustable (set_long_press_ms, the wheel delay 200-600 ms); HUD v2
	// keeps 400.
	Uint32 touch_long_press_ms() const { return (Uint32)g_long_press_ms; }

	int touch_down_px = 0;
	int touch_down_py = 0;
	int touch_down_rx = 0;
	int touch_down_ry = 0;
	Uint32 touch_down_ticks = 0;
	Uint32 last_tap_ticks = 0;
	int last_tap_px = -10000;
	int last_tap_py = -10000;

	// Render-space rect where a single-finger press begins a left-drag at once
	// (minimap viewport sliding). Empty (x1 < x0) disables it.
	int idr_x0 = 0, idr_y0 = 0, idr_x1 = -1, idr_y1 = -1;
	bool in_immediate_drag_rect(int rx, int ry) {
		return idr_x1 > idr_x0 && rx >= idr_x0 && rx < idr_x1 && ry >= idr_y0 && ry < idr_y1;
	}

	// HUD v3 "HUD fingers": a finger that lands in one of these rectangles is
	// reported on its own (type_hold_*) and never joins the gesture state machine.
	int hold_r[16] = {};
	int hold_n = 0;
	struct hold_finger_t { SDL_FingerID id; int which; };
	std::vector<hold_finger_t> hold_fingers;
	int hold_rect_at(int rx, int ry) {
		for (int i = 0; i < hold_n; ++i)
			if (rx >= hold_r[i * 4] && rx < hold_r[i * 4 + 2] && ry >= hold_r[i * 4 + 1] && ry < hold_r[i * 4 + 3]) return i;
		return -1;
	}
	hold_finger_t* find_hold(SDL_FingerID id) {
		for (auto& h : hold_fingers) if (h.id == id) return &h;
		return nullptr;
	}

	finger_state* find_finger(SDL_FingerID id) {
		for (auto& v : fingers) {
			if (v.id == id) return &v;
		}
		return nullptr;
	}

	void push_mouse_event(int type, int button, int x, int y, int button_state, int clicks = 1) {
		event_t e;
		e.type = type;
		e.button = button;
		e.mouse_x = x;
		e.mouse_y = y;
		e.mouse_xrel = 0;
		e.mouse_yrel = 0;
		e.button_state = button_state;
		e.clicks = clicks;
		if (button >= 0 && (size_t)button < mouse_button_state.size()) {
			mouse_button_state[button] = type == event_t::type_mouse_button_down;
		}
		pending_events.push_back(e);
	}

	void pan_centroid(int* rx, int* ry) {
		int n = 0;
		int sx = 0;
		int sy = 0;
		for (auto& v : fingers) {
			sx += v.rx;
			sy += v.ry;
			++n;
			if (n == 2) break;
		}
		if (n == 0) {
			*rx = 0;
			*ry = 0;
			return;
		}
		*rx = sx / n;
		*ry = sy / n;
	}

	// Physical-pixel distance between the first two fingers (pinch gesture).
	int pinch_ref_dist = 0;
	// Physical centroid of the two fingers when the pinch baseline was taken,
	// and when the two-finger gesture started - used to tell a deliberate
	// pinch (separation changes) from a two-finger scroll (whole hand moves,
	// separation ~constant), and to delay zoom until the fingers settle.
	int pinch_ref_cx = 0, pinch_ref_cy = 0;
	Uint32 pinch_start_ticks = 0;
	int finger_dist() {
		if (fingers.size() < 2) return 0;
		int dx = fingers[0].px - fingers[1].px;
		int dy = fingers[0].py - fingers[1].py;
		int d = (int)std::sqrt((double)(dx * dx + dy * dy));
		return d;
	}
	// Physical-pixel centroid of the first two fingers (for pan/pinch discrimination).
	void pan_centroid_px(int* x, int* y) {
		int n = 0, sx = 0, sy = 0;
		for (auto& v : fingers) { sx += v.px; sy += v.py; if (++n == 2) break; }
		if (n == 0) { *x = 0; *y = 0; return; }
		*x = sx / n; *y = sy / n;
	}
	void push_zoom(int delta) {
		event_t e;
		e.type = event_t::type_zoom;
		e.button = delta;
		pan_centroid(&e.mouse_x, &e.mouse_y);
		pending_events.push_back(e);
	}

	// Called each peek_message iteration: long presses fire on time, not on
	// an incoming event (the finger is holding still).
	bool poll_long_press(event_t& e) {
		if (touch_state != touch_state_t::down_short) return false;
		if (SDL_GetTicks() - touch_down_ticks < touch_long_press_ms()) return false;
		touch_state = touch_state_t::long_fired;
		const finger_state& f = fingers.empty() ? finger_state{0, touch_down_px, touch_down_py, touch_down_rx, touch_down_ry} : fingers[0];
		e = event_t();
		e.type = event_t::type_long_press;
		e.mouse_x = f.rx;
		e.mouse_y = f.ry;
		return true;
	}

	void handle_finger_event(const SDL_Event& sdl_e) {
		int rw = render_w;
		int rh = render_h;
		int pw = 0;
		int ph = 0;
		if (window) SDL_GetWindowSize(window, &pw, &ph);
		if (rw < 1 || rh < 1) {
			rw = pw;
			rh = ph;
		}
		int rx = (int)(sdl_e.tfinger.x * rw);
		int ry = (int)(sdl_e.tfinger.y * rh);
		int px = (int)(sdl_e.tfinger.x * pw);
		int py = (int)(sdl_e.tfinger.y * ph);
		SDL_FingerID id = sdl_e.tfinger.fingerId;

		if (hold_finger_t* h = find_hold(id)) {
			// a HUD finger: its own events only
			if (sdl_e.type == SDL_FINGERMOTION) push_mouse_event(event_t::type_hold_motion, h->which, rx, ry, 0);
			else if (sdl_e.type == SDL_FINGERUP) {
				const int which = h->which;
				for (auto i = hold_fingers.begin(); i != hold_fingers.end(); ++i) if (i->id == id) { hold_fingers.erase(i); break; }
				push_mouse_event(event_t::type_hold_up, which, rx, ry, 0);
			}
			return;
		}
		if (sdl_e.type == SDL_FINGERDOWN && !find_finger(id)) {
			const int which = hold_rect_at(rx, ry);
			if (which >= 0) {
				hold_fingers.push_back({id, which});
				push_mouse_event(event_t::type_hold_down, which, rx, ry, 0);
				return;
			}
		}
		if (sdl_e.type == SDL_FINGERDOWN) {
			if (!find_finger(id)) fingers.push_back({id, px, py, rx, ry});
			if (fingers.size() == 1) push_mouse_event(event_t::type_touch_down, -1, rx, ry, 0);
			if (fingers.size() == 1) {
				touch_down_px = px;
				touch_down_py = py;
				touch_down_rx = rx;
				touch_down_ry = ry;
				touch_down_ticks = SDL_GetTicks();
				if (in_immediate_drag_rect(rx, ry)) {
					// Minimap: slide the viewport straight away (no tap /
					// long-press wait), so a press-drag pans continuously.
					touch_state = touch_state_t::dragging;
					push_mouse_event(event_t::type_mouse_button_down, 1, rx, ry, 1);
				} else {
					touch_state = touch_state_t::down_short;
				}
			} else if (fingers.size() == 2 && touch_state != touch_state_t::panning) {
				if (touch_state == touch_state_t::dragging) {
					// The first finger had already started a drag (it moved before the
					// second one landed -- normal for a quick two-finger swipe). This
					// used to FINISH the drag with a button-up, and the UI then acted
					// on it: a thin box counted as a tap and, with tap-to-command, sent
					// the selected units to where the first finger was; a wider box
					// replaced the selection. A two-finger pan must do neither, so the
					// drag is cancelled instead.
					push_mouse_event(event_t::type_touch_cancel, -1, fingers[0].rx, fingers[0].ry, 0);
					if (mouse_button_state.size() > 1) mouse_button_state[1] = false;
				} else if (touch_state == touch_state_t::long_fired) {
					// The first finger had rested long enough to fire a long press (HUD
					// v3: the command wheel is open under it, HUD v2: a group's hold
					// menu) before the second one landed. A two-finger pan drops that
					// too -- the wheel must not stay up, nor pick a slot, while panning.
					push_mouse_event(event_t::type_touch_cancel, -1, fingers[0].rx, fingers[0].ry, 0);
				}
				touch_state = touch_state_t::panning;
				pinch_ref_dist = finger_dist();
				pan_centroid_px(&pinch_ref_cx, &pinch_ref_cy);
				pinch_start_ticks = SDL_GetTicks();
				int cx;
				int cy;
				pan_centroid(&cx, &cy);
				push_mouse_event(event_t::type_mouse_button_down, 3, cx, cy, 4);
			}
		} else if (sdl_e.type == SDL_FINGERMOTION) {
			finger_state* f = find_finger(id);
			if (f) {
				f->px = px;
				f->py = py;
				f->rx = rx;
				f->ry = ry;
			}
			switch (touch_state) {
			case touch_state_t::down_short:
				if (std::abs(px - touch_down_px) > touch_drag_threshold_px || std::abs(py - touch_down_py) > touch_drag_threshold_px) {
					// Commit the left button at the ORIGINAL touch point so
					// the selection box anchors where the finger first
					// landed, then catch up to the current position.
					touch_state = touch_state_t::dragging;
					push_mouse_event(event_t::type_mouse_button_down, 1, touch_down_rx, touch_down_ry, 1);
					push_mouse_event(event_t::type_mouse_motion, -1, rx, ry, 1);
				}
				break;
			case touch_state_t::dragging:
				push_mouse_event(event_t::type_mouse_motion, -1, rx, ry, 1);
				break;
			case touch_state_t::long_fired:
				// the held finger slides: a held control may offer choices
				if (fingers.size() == 1) push_mouse_event(event_t::type_long_motion, -1, rx, ry, 0);
				break;
			case touch_state_t::panning: {
				int cx;
				int cy;
				pan_centroid(&cx, &cy);
				push_mouse_event(event_t::type_mouse_motion, -1, cx, cy, 4);
				// Pinch vs two-finger scroll. A scroll moves the whole hand
				// (centroid) while keeping the fingers ~the same distance apart;
				// a pinch changes that separation. Only zoom when the separation
				// changes by a large, deliberate amount AND that change dominates
				// how far the hand panned - so a scroll never zooms. Also wait a
				// short settle time so the jitter of the second finger landing
				// isn't read as a pinch.
				int d = finger_dist();
				int ccx, ccy;
				pan_centroid_px(&ccx, &ccy);
				if (pinch_ref_dist > 40 && d > 40 && SDL_GetTicks() - pinch_start_ticks > 90) {
					int sep = d - pinch_ref_dist;                 // signed separation change
					int add = sep < 0 ? -sep : sep;
					int pan_moved = std::abs(ccx - pinch_ref_cx) + std::abs(ccy - pinch_ref_cy);
					if (add * 100 >= pinch_ref_dist * 40 && add * 2 >= pan_moved) {
						push_zoom(sep > 0 ? 1 : -1);
						pinch_ref_dist = d;         // require another large change for the next step
						pinch_ref_cx = ccx;
						pinch_ref_cy = ccy;
						pinch_start_ticks = SDL_GetTicks();
					}
				}
				break;
			}
			default:
				break;
			}
		} else if (sdl_e.type == SDL_FINGERUP) {
			for (auto i = fingers.begin(); i != fingers.end(); ++i) {
				if (i->id == id) {
					fingers.erase(i);
					break;
				}
			}
			switch (touch_state) {
			case touch_state_t::down_short:
				if (fingers.empty()) {
					// Short tap: emit a full click. Double-tap detection uses
					// the release position/time of the previous tap.
					Uint32 now = SDL_GetTicks();
					int clicks = 1;
					if (now - last_tap_ticks < touch_double_tap_ms && std::abs(px - last_tap_px) < touch_double_tap_dist_px && std::abs(py - last_tap_py) < touch_double_tap_dist_px) clicks = 2;
					last_tap_ticks = now;
					last_tap_px = px;
					last_tap_py = py;
					push_mouse_event(event_t::type_mouse_button_down, 1, touch_down_rx, touch_down_ry, 1, clicks);
					push_mouse_event(event_t::type_mouse_button_up, 1, touch_down_rx, touch_down_ry, 0, clicks);
					touch_state = touch_state_t::idle;
				}
				break;
			case touch_state_t::dragging:
				if (fingers.empty()) {
					push_mouse_event(event_t::type_mouse_button_up, 1, rx, ry, 0);
					touch_state = touch_state_t::idle;
				}
				break;
			case touch_state_t::panning:
				if (fingers.size() < 2) {
					push_mouse_event(event_t::type_mouse_button_up, 3, rx, ry, 0);
					// Require lift-and-retouch before the next gesture:
					// predictable, and a leftover finger can't misfire.
					touch_state = touch_state_t::idle;
				}
				break;
			case touch_state_t::long_fired:
				if (fingers.empty()) {
					push_mouse_event(event_t::type_long_release, -1, rx, ry, 0);
					touch_state = touch_state_t::idle;
				}
				break;
			default:
				break;
			}
			if (fingers.empty()) touch_state = touch_state_t::idle;
		}
	}
#endif

	bool peek_message(event_t& e) {
		SDL_Event sdl_e;
#ifdef __ANDROID__
		if (!pending_events.empty()) {
			e = pending_events.front();
			pending_events.pop_front();
			return true;
		}
		if (poll_long_press(e)) return true;
#endif
		while (SDL_PollEvent(&sdl_e)) {
			switch (sdl_e.type) {
#ifdef __ANDROID__
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_FINGERMOTION:
				handle_finger_event(sdl_e);
				if (!pending_events.empty()) {
					e = pending_events.front();
					pending_events.pop_front();
					return true;
				}
				break;
#endif
			case SDL_MOUSEMOTION:
				e.type = event_t::type_mouse_motion;
				e.button_state = sdl_e.motion.state;
				e.mouse_x = sdl_e.motion.x;
				e.mouse_y = sdl_e.motion.y;
				e.mouse_xrel = sdl_e.motion.xrel;
				e.mouse_yrel = sdl_e.motion.yrel;
				return true;
			case SDL_MOUSEBUTTONDOWN:
				e.type = event_t::type_mouse_button_down;
				e.button = sdl_e.button.button;
				e.mouse_x = sdl_e.button.x;
				e.mouse_y = sdl_e.button.y;
				e.clicks = sdl_e.button.clicks;
				if ((size_t)e.button < mouse_button_state.size()) mouse_button_state[e.button] = true;
				return true;
			case SDL_MOUSEBUTTONUP:
				e.type = event_t::type_mouse_button_up;
				e.button = sdl_e.button.button;
				e.mouse_x = sdl_e.button.x;
				e.mouse_y = sdl_e.button.y;
				e.clicks = sdl_e.button.clicks;
				if ((size_t)e.button < mouse_button_state.size()) mouse_button_state[e.button] = false;
				return true;
			case SDL_KEYDOWN:
				e.type = event_t::type_key_down;
				e.sym = sdl_e.key.keysym.sym;
				e.scancode = sdl_e.key.keysym.scancode;
				if ((size_t)e.scancode < key_state.size()) key_state[e.scancode] = true;
				return true;
			case SDL_KEYUP:
				e.type = event_t::type_key_up;
				e.sym = sdl_e.key.keysym.sym;
				e.scancode = sdl_e.key.keysym.scancode;
				if ((size_t)e.scancode < key_state.size()) key_state[e.scancode] = false;
				return true;
			case SDL_TEXTINPUT:
				e.type = event_t::type_text_input;
				// SDL's text buffer is 32 bytes incl. NUL; copy verbatim.
				std::memcpy(e.text, sdl_e.text.text, sizeof(e.text));
				e.text[sizeof(e.text) - 1] = 0;
				return true;
			case SDL_QUIT:
			case SDL_APP_TERMINATING:
				e.type = event_t::type_quit;
				return true;
			case SDL_APP_LOWMEMORY:
				low_memory_flag = true;
				break;
			case SDL_APP_DIDENTERFOREGROUND:
			case SDL_RENDER_TARGETS_RESET:
			case SDL_RENDER_DEVICE_RESET:
				// Coming back from the background (or a GPU context loss)
				// invalidates the texture and possibly the EGL surface.
				// Report a synthetic resize so the UI recreates its surfaces.
				invalidate_texture();
#ifdef OB_POST_GL
				// SP: all GL objects are lost with the context -- zero the handles (do NOT
				// glDelete; the names are invalid now) so post_gl_init rebuilds them lazily.
				post_prog = 0; post_vbo = 0; post_prog_sharp = 0; post_prog_blit = 0;
				post_fbo = 0; post_fbo_tex = 0; post_fbo_w = 0; post_fbo_h = 0;
				post_prog_bright = 0; post_prog_blur = 0; post_prog_comp = 0;
				post_bloomfbo[0] = 0; post_bloomfbo[1] = 0; post_bloomtex[0] = 0; post_bloomtex[1] = 0;
				post_bloom_w = 0; post_bloom_h = 0; post_bloom_ready = false;
				post_gl_ok = false; post_gl_tried = false;
#endif
				update_render_size();
				if (render_w >= 1 && render_h >= 1) {
					e.type = event_t::type_resize;
					e.width = render_w;
					e.height = render_h;
					return true;
				}
				break;
			case SDL_WINDOWEVENT:
				if (sdl_e.window.windowID == SDL_GetWindowID(window)) {
					if (sdl_e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || sdl_e.window.event == SDL_WINDOWEVENT_RESIZED) {
						update_render_size();
						e.type = event_t::type_resize;
						e.width = render_w >= 1 ? render_w : sdl_e.window.data1;
						e.height = render_h >= 1 ? render_h : sdl_e.window.data2;
						return true;
					}
					if (sdl_e.window.event == SDL_WINDOWEVENT_RESTORED || sdl_e.window.event == SDL_WINDOWEVENT_EXPOSED) {
						invalidate_texture();
						update_render_size();
						if (render_w >= 1 && render_h >= 1) {
							e.type = event_t::type_resize;
							e.width = render_w;
							e.height = render_h;
							return true;
						}
					}
				}
				break;
			}
		}
		return false;
	}

	bool show_cursor(bool show) {
		return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE) ? true : false;
	}
	
	bool get_key_state(int scancode) {
		return key_state.at(scancode) ? true : false;
	}
	
	bool get_mouse_button_state(int button) {
		return mouse_button_state.at(button) ? true : false;
	}
	
#ifdef OB_POST_GL
	static GLuint post_compile(GLenum type, const char* src) {
		GLuint s = glCreateShader(type);
		glShaderSource(s, 1, &src, nullptr);
		glCompileShader(s);
		GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) { char l[512]; glGetShaderInfoLog(s, sizeof l, nullptr, l); log("post_gl shader compile: %s\n", l); glDeleteShader(s); return 0; }
		return s;
	}
	// Link a vertex+fragment source pair; returns 0 on failure (logged).
	GLuint post_link(const char* vs, const char* fs) {
		GLuint v = post_compile(GL_VERTEX_SHADER, vs), f = post_compile(GL_FRAGMENT_SHADER, fs);
		if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
		GLuint p = glCreateProgram();
		glAttachShader(p, v); glAttachShader(p, f);
		glLinkProgram(p);
		glDeleteShader(v); glDeleteShader(f);
		GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) { char l[512]; glGetProgramInfoLog(p, sizeof l, nullptr, l); log("post_gl link: %s\n", l); glDeleteProgram(p); return 0; }
		return p;
	}
	// Allocate one RGBA8 render target (fbo+tex) at w x h. Returns false on incomplete FBO.
	bool make_fbo(GLuint& fbo, GLuint& tex, int w, int h) {
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		return st == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
	}
	void post_fbo_teardown() {
		if (post_fbo) { glDeleteFramebuffers(1, &post_fbo); post_fbo = 0; }
		if (post_fbo_tex) { glDeleteTextures(1, &post_fbo_tex); post_fbo_tex = 0; }
		post_fbo_w = post_fbo_h = 0;
	}
	void post_bloom_fbo_teardown() {
		for (int i = 0; i < 2; ++i) {
			if (post_bloomfbo[i]) { glDeleteFramebuffers(1, &post_bloomfbo[i]); post_bloomfbo[i] = 0; }
			if (post_bloomtex[i]) { glDeleteTextures(1, &post_bloomtex[i]); post_bloomtex[i] = 0; }
		}
		post_bloom_w = post_bloom_h = 0;
	}
	void post_gl_teardown() {
		if (post_vbo) { glDeleteBuffers(1, &post_vbo); post_vbo = 0; }
		if (post_prog) { glDeleteProgram(post_prog); post_prog = 0; }
		if (post_prog_sharp) { glDeleteProgram(post_prog_sharp); post_prog_sharp = 0; }
		if (post_prog_blit) { glDeleteProgram(post_prog_blit); post_prog_blit = 0; }
		if (post_prog_bright) { glDeleteProgram(post_prog_bright); post_prog_bright = 0; }
		if (post_prog_blur) { glDeleteProgram(post_prog_blur); post_prog_blur = 0; }
		if (post_prog_comp) { glDeleteProgram(post_prog_comp); post_prog_comp = 0; }
		post_fbo_teardown();
		post_bloom_fbo_teardown();
		post_gl_ok = false;
	}
	// Lazily (re)allocate the intermediate RGBA8 render target at w x h. Returns false on
	// failure (incomplete FBO / GL error) -> caller falls back to the identity path.
	bool post_ensure_fbo(int w, int h) {
		if (post_fbo && post_fbo_w == w && post_fbo_h == h) return true;
		post_fbo_teardown();
		while (glGetError() != GL_NO_ERROR) {}   // drain stale errors so the check below judges only our setup
		if (!make_fbo(post_fbo, post_fbo_tex, w, h)) { post_fbo_teardown(); return false; }
		post_fbo_w = w; post_fbo_h = h;
		return true;
	}
	// Lazily (re)allocate the two half-res bloom ping-pong buffers (bright-pass + blur).
	bool post_ensure_bloom_fbos(int w, int h) {
		int bw = w / 2 < 1 ? 1 : w / 2, bh = h / 2 < 1 ? 1 : h / 2;
		if (post_bloomfbo[0] && post_bloom_w == bw && post_bloom_h == bh) return true;
		post_bloom_fbo_teardown();
		while (glGetError() != GL_NO_ERROR) {}
		if (!make_fbo(post_bloomfbo[0], post_bloomtex[0], bw, bh) ||
		    !make_fbo(post_bloomfbo[1], post_bloomtex[1], bw, bh)) { post_bloom_fbo_teardown(); return false; }
		post_bloom_w = bw; post_bloom_h = bh;
		return true;
	}
	void post_gl_init() {
		post_gl_tried = true;
		// Guard: only the opengles2 SDL backend gives us a current GLES2 context, so
		// raw glCreateShader/SDL_GL_BindTexture are valid. On a software/opengles(v1)/
		// D3D fallback (emulator, some devices) these calls are UB -> stay on the classic
		// present path. SDL_CreateRenderer(-1) can pick a non-GLES2 backend.
		SDL_RendererInfo ri;
		if (SDL_GetRendererInfo(renderer, &ri) != 0 || !ri.name || SDL_strcmp(ri.name, "opengles2") != 0) {
#ifdef __ANDROID__
			__android_log_print(ANDROID_LOG_INFO, "OpenBW", "post_gl: renderer '%s' is not opengles2 -> fallback", ri.name ? ri.name : "?");
#endif
			return;
		}
		// Full-screen-triangle vertex shaders. UVs from aPos; the "flip" variant flips V so a
		// top-down source presents right-side-up on GL's bottom-up FBO. Intermediate passes
		// (writing to our own FBO textures) use "noflip" so the flip is applied exactly once,
		// at the final present-to-screen pass.
		const char* vs_flip =
			"attribute vec2 aPos;\nvarying vec2 vUv;\n"
			"void main(){ vUv = vec2(aPos.x*0.5+0.5, 1.0-(aPos.y*0.5+0.5)); gl_Position = vec4(aPos,0.0,1.0); }\n";
		const char* vs_noflip =
			"attribute vec2 aPos;\nvarying vec2 vUv;\n"
			"void main(){ vUv = vec2(aPos.x*0.5+0.5, aPos.y*0.5+0.5); gl_Position = vec4(aPos,0.0,1.0); }\n";
		// The present surface is SDL_PIXELFORMAT_ARGB8888 (BGRA byte order in memory on
		// little-endian; arm64 is always LE). SDL's own GLES2 renderer swizzles that format
		// in its shaders; our raw texture2D() does not, so R and B arrive swapped. Shaders
		// that sample the *SDL source* undo it with .bgra/.bgr; shaders that sample our own
		// RGBA FBO textures do not. NB: the swizzle hard-codes the LE memory order.
		// SE color-grade: a subtle cinematic grade (contrast + saturation + slight warmth),
		// blended neutral->graded by uGrade (0 = exact no-op). Injected into every final
		// present shader so it composes with identity / sharpen / bloom uniformly.
		#define OB_GRADE_FN \
			"uniform float uGrade;\n" \
			"vec3 grd(vec3 c){ vec3 g=(c-0.5)*1.12+0.5; float l=dot(g,vec3(0.299,0.587,0.114));" \
			" g=mix(vec3(l),g,1.15); g+=vec3(0.02,0.0,-0.02); return clamp(mix(c,g,uGrade),0.0,1.0);}\n"
		const char* fs_ident_bgra =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\n" OB_GRADE_FN
			"void main(){ vec4 s = texture2D(uTex, vUv).bgra; gl_FragColor = vec4(grd(s.rgb), s.a); }\n";
		const char* fs_ident_plain =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\n" OB_GRADE_FN
			"void main(){ gl_FragColor = vec4(grd(texture2D(uTex, vUv).rgb), 1.0); }\n";
		// Sharpen/crispen: Laplacian unsharp mask. sharp = c + amt*(4c - N - S - E - W).
		// Reads the SDL source (.bgr to get RGB), samples 1-texel neighbours via uTexel.
		const char* fs_sharp =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\n"
			"uniform vec2 uTexel;\nuniform float uAmt;\n"
			"void main(){\n"
			"  vec3 c = texture2D(uTex, vUv).bgr;\n"
			"  vec3 l = texture2D(uTex, vUv - vec2(uTexel.x, 0.0)).bgr;\n"
			"  vec3 r = texture2D(uTex, vUv + vec2(uTexel.x, 0.0)).bgr;\n"
			"  vec3 u = texture2D(uTex, vUv - vec2(0.0, uTexel.y)).bgr;\n"
			"  vec3 d = texture2D(uTex, vUv + vec2(0.0, uTexel.y)).bgr;\n"
			"  vec3 s = c + uAmt * (4.0*c - l - r - u - d);\n"
			"  gl_FragColor = vec4(clamp(s, 0.0, 1.0), 1.0);\n"
			"}\n";
		post_prog = post_link(vs_flip, fs_ident_bgra);
		if (!post_prog) return;
		post_a_pos = glGetAttribLocation(post_prog, "aPos");
		post_u_tex = glGetUniformLocation(post_prog, "uTex");
		post_u_grade = glGetUniformLocation(post_prog, "uGrade");
		// Effect programs (sharpen + final blit). If either fails to build we simply keep
		// effect 0 (identity) rather than aborting the whole stage.
		post_prog_sharp = post_link(vs_noflip, fs_sharp);
		if (post_prog_sharp) {
			post_sharp_pos = glGetAttribLocation(post_prog_sharp, "aPos");
			post_sharp_tex = glGetUniformLocation(post_prog_sharp, "uTex");
			post_sharp_texel = glGetUniformLocation(post_prog_sharp, "uTexel");
			post_sharp_amt = glGetUniformLocation(post_prog_sharp, "uAmt");
		}
		post_prog_blit = post_link(vs_flip, fs_ident_plain);
		if (post_prog_blit) {
			post_blit_pos = glGetAttribLocation(post_prog_blit, "aPos");
			post_blit_tex = glGetUniformLocation(post_prog_blit, "uTex");
			post_blit_grade = glGetUniformLocation(post_prog_blit, "uGrade");
		}
		// SP Phase 3 bloom shaders. All read our own RGBA FBO textures (no swizzle).
		// bright-pass: keep the luma above uThr, scaled to [0,1]. Blur: 5-tap linear-sampled
		// Gaussian along uDir (uses GL_LINEAR bilinear taps, ~9-tap quality). comp: add the
		// blurred bright buffer back over the base and present (flip).
		const char* fs_bright =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\nuniform float uThr;\n"
			"void main(){\n"
			"  vec3 c = texture2D(uTex, vUv).rgb;\n"
			"  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
			"  float k = max(l - uThr, 0.0) / max(1.0 - uThr, 0.001);\n"
			"  gl_FragColor = vec4(c * k, 1.0);\n"
			"}\n";
		const char* fs_blur =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\nuniform vec2 uDir;\n"
			"void main(){\n"
			"  vec3 s = texture2D(uTex, vUv).rgb * 0.2270270270;\n"
			"  s += texture2D(uTex, vUv + uDir * 1.3846153846).rgb * 0.3162162162;\n"
			"  s += texture2D(uTex, vUv - uDir * 1.3846153846).rgb * 0.3162162162;\n"
			"  s += texture2D(uTex, vUv + uDir * 3.2307692308).rgb * 0.0702702703;\n"
			"  s += texture2D(uTex, vUv - uDir * 3.2307692308).rgb * 0.0702702703;\n"
			"  gl_FragColor = vec4(s, 1.0);\n"
			"}\n";
		const char* fs_comp =
			"precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uBase;\nuniform sampler2D uBloom;\nuniform float uAmt;\n" OB_GRADE_FN
			"void main(){\n"
			"  vec3 b = texture2D(uBase, vUv).rgb;\n"
			"  vec3 g = texture2D(uBloom, vUv).rgb;\n"
			"  gl_FragColor = vec4(grd(min(b + g * uAmt, 1.0)), 1.0);\n"
			"}\n";
		post_prog_bright = post_link(vs_noflip, fs_bright);
		if (post_prog_bright) {
			post_bright_pos = glGetAttribLocation(post_prog_bright, "aPos");
			post_bright_tex = glGetUniformLocation(post_prog_bright, "uTex");
			post_bright_thr = glGetUniformLocation(post_prog_bright, "uThr");
		}
		post_prog_blur = post_link(vs_noflip, fs_blur);
		if (post_prog_blur) {
			post_blur_pos = glGetAttribLocation(post_prog_blur, "aPos");
			post_blur_tex = glGetUniformLocation(post_prog_blur, "uTex");
			post_blur_dir = glGetUniformLocation(post_prog_blur, "uDir");
		}
		post_prog_comp = post_link(vs_flip, fs_comp);
		if (post_prog_comp) {
			post_comp_pos = glGetAttribLocation(post_prog_comp, "aPos");
			post_comp_base = glGetUniformLocation(post_prog_comp, "uBase");
			post_comp_bloom = glGetUniformLocation(post_prog_comp, "uBloom");
			post_comp_amt = glGetUniformLocation(post_prog_comp, "uAmt");
			post_comp_grade = glGetUniformLocation(post_prog_comp, "uGrade");
		}
		static const float tri[6] = { -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f };
		glGenBuffers(1, &post_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, post_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		if (glGetError() != GL_NO_ERROR || post_a_pos < 0) { post_gl_teardown();
#ifdef __ANDROID__
			__android_log_print(ANDROID_LOG_ERROR, "OpenBW", "post_gl init: GL error / no attrib");
#endif
			return; }
		// Probe the GPU family (drives the AUTO default) and whether the effect programs
		// are usable. The concrete effect is resolved per frame in resolve_effect() so a
		// runtime settings change takes effect without a GL rebuild.
		const char* rend = (const char*)glGetString(GL_RENDERER);
		post_is_adreno = rend && SDL_strstr(rend, "Adreno") != nullptr;
		post_sharp_ready = post_prog_sharp && post_prog_blit && post_sharp_pos >= 0 && post_blit_pos >= 0;
		// Bloom needs all three programs + the sharpen program (reused, uAmt=0, for the base
		// pass into the FBO) + the blit path's FBO scaffold.
		post_bloom_ready = post_prog_bright && post_prog_blur && post_prog_comp && post_prog_sharp
			&& post_bright_pos >= 0 && post_blur_pos >= 0 && post_comp_pos >= 0 && post_sharp_pos >= 0;
		post_gl_ok = true;
#ifdef __ANDROID__
		__android_log_print(ANDROID_LOG_INFO, "OpenBW", "post_gl: ready (effect=%d strength=%.2f, GL_RENDERER=%s)", resolve_effect(), post_strength, rend ? rend : "?");
#endif
	}
	// Resolve the concrete effect for this frame. AUTO (post_effect<0) -> sharpen only on
	// Adreno; an explicit effect wins but degrades to identity if sharpen isn't available.
	int resolve_effect() {
		if (!post_sharp_ready) return 0;
		if (post_effect < 0) return post_is_adreno ? 1 : 0;
		return post_effect == 1 ? 1 : 0;
	}
	// Draw the full-screen triangle with `prog` (attrib `aPosLoc`) into the currently bound
	// framebuffer/viewport. Texture unit 0 must already hold the input; sampler is `texLoc`.
	void post_draw_tri(GLuint prog, GLint aPosLoc, GLint texLoc) {
		glUseProgram(prog);
		if (texLoc >= 0) glUniform1i(texLoc, 0);
		glBindBuffer(GL_ARRAY_BUFFER, post_vbo);
		glEnableVertexAttribArray((GLuint)aPosLoc);
		glVertexAttribPointer((GLuint)aPosLoc, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glDisableVertexAttribArray((GLuint)aPosLoc);
	}
	// Present `tex` through the selected effect to the default framebuffer. Returns true if
	// it ran; false -> caller falls back to SDL_RenderCopy.
	bool post_gl_present(SDL_Texture* tex) {
		if (!post_gl_ok || !tex) return false;
		int eff = resolve_effect();
		bool want_bloom = post_bloom > 0.004f && post_bloom_ready;
		bool want_grade = post_grade > 0.004f;  // SE color-grade (applied in the final shader)
		// AUTO that resolves to identity (e.g. Mali/S7+) with no bloom or grade buys nothing
		// but a blit -> classic present. An explicit effect / bloom / grade runs the GL path.
		if (eff == 0 && post_effect < 0 && !want_bloom && !want_grade) return false;
		SDL_RenderFlush(renderer);   // commit SDL's queued clear/copies before our GL block
		float tw = 1, th = 1;
		glActiveTexture(GL_TEXTURE0);
		if (SDL_GL_BindTexture(tex, &tw, &th) != 0) return false;
		int ow = 0, oh = 0; SDL_GetRendererOutputSize(renderer, &ow, &oh);
		glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);

		// SP Phase 3 bloom chain: base (SDL source -> texA via the sharpen prog, uAmt=0 when
		// not sharpening) -> bright-pass into a half-res buffer -> separable blur (H then V,
		// ping-pong) -> additive composite of texA + blurred bloom -> screen (flip). Falls
		// through to the sharpen/identity paths if the FBOs can't be allocated this frame.
		if (want_bloom && post_ensure_fbo(ow, oh) && post_ensure_bloom_fbos(ow, oh)) {
			// Base -> texA
			glBindFramebuffer(GL_FRAMEBUFFER, post_fbo);
			glViewport(0, 0, ow, oh);
			glUseProgram(post_prog_sharp);
			glUniform2f(post_sharp_texel, tw / (float)tex_w, th / (float)tex_h);
			glUniform1f(post_sharp_amt, eff == 1 ? post_strength : 0.f);
			post_draw_tri(post_prog_sharp, post_sharp_pos, post_sharp_tex);
			// Bright-pass texA -> bloom[0] (half-res)
			glBindFramebuffer(GL_FRAMEBUFFER, post_bloomfbo[0]);
			glViewport(0, 0, post_bloom_w, post_bloom_h);
			glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, post_fbo_tex);
			glUseProgram(post_prog_bright);
			glUniform1f(post_bright_thr, 0.72f);
			post_draw_tri(post_prog_bright, post_bright_pos, post_bright_tex);
			// Blur H bloom[0] -> bloom[1]
			glBindFramebuffer(GL_FRAMEBUFFER, post_bloomfbo[1]);
			glViewport(0, 0, post_bloom_w, post_bloom_h);
			glBindTexture(GL_TEXTURE_2D, post_bloomtex[0]);
			glUseProgram(post_prog_blur);
			glUniform2f(post_blur_dir, 1.f / (float)post_bloom_w, 0.f);
			post_draw_tri(post_prog_blur, post_blur_pos, post_blur_tex);
			// Blur V bloom[1] -> bloom[0]
			glBindFramebuffer(GL_FRAMEBUFFER, post_bloomfbo[0]);
			glViewport(0, 0, post_bloom_w, post_bloom_h);
			glBindTexture(GL_TEXTURE_2D, post_bloomtex[1]);
			glUniform2f(post_blur_dir, 0.f, 1.f / (float)post_bloom_h);
			post_draw_tri(post_prog_blur, post_blur_pos, post_blur_tex);
			// Composite texA + bloom[0] -> screen (flip)
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, ow, oh);
			glUseProgram(post_prog_comp);
			glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, post_bloomtex[0]);
			if (post_comp_bloom >= 0) glUniform1i(post_comp_bloom, 1);
			glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, post_fbo_tex);
			if (post_comp_base >= 0) glUniform1i(post_comp_base, 0);
			glUniform1f(post_comp_amt, post_bloom);
			if (post_comp_grade >= 0) glUniform1f(post_comp_grade, post_grade);
			glBindBuffer(GL_ARRAY_BUFFER, post_vbo);
			glEnableVertexAttribArray((GLuint)post_comp_pos);
			glVertexAttribPointer((GLuint)post_comp_pos, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glDisableVertexAttribArray((GLuint)post_comp_pos);
			glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
			glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
		} else if (eff == 1 && post_ensure_fbo(ow, oh)) {
			// Effect 1 (sharpen): pass A shades the SDL source into the intermediate FBO (no
			// flip), pass B blits that RGBA target to screen (flip).
			if (post_bloomfbo[0]) post_bloom_fbo_teardown();  // release bloom buffers if not blooming
			glBindFramebuffer(GL_FRAMEBUFFER, post_fbo);
			glViewport(0, 0, ow, oh);
			glUseProgram(post_prog_sharp);
			glUniform2f(post_sharp_texel, tw / (float)tex_w, th / (float)tex_h);
			glUniform1f(post_sharp_amt, post_strength);
			post_draw_tri(post_prog_sharp, post_sharp_pos, post_sharp_tex);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, ow, oh);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, post_fbo_tex);
			glUseProgram(post_prog_blit);
			if (post_blit_grade >= 0) glUniform1f(post_blit_grade, post_grade);
			post_draw_tri(post_prog_blit, post_blit_pos, post_blit_tex);
			glBindTexture(GL_TEXTURE_2D, 0);
		} else {
			// Effect 0 (identity): single pass, SDL source -> screen (flip + .bgra).
			if (post_fbo) post_fbo_teardown();
			if (post_bloomfbo[0]) post_bloom_fbo_teardown();
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, ow, oh);
			glUseProgram(post_prog);
			if (post_u_grade >= 0) glUniform1f(post_u_grade, post_grade);
			post_draw_tri(post_prog, post_a_pos, post_u_tex);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
		SDL_GL_UnbindTexture(tex);   // let SDL re-establish its own GL state next batch
		return true;
	}
#endif

	void update_surface() {
		if (!renderer || !present_source) return;
		if (!texture || tex_w != present_source->w || tex_h != present_source->h) {
			invalidate_texture();
			// Must match the present-surface channel order (ARGB8888, see
			// create_rgba_surface): SDL_UpdateTexture is a raw copy, no convert.
			texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, present_source->w, present_source->h);
			if (!texture) {
				log("SDL_CreateTexture failed: %s\n", SDL_GetError());
				return;
			}
			tex_w = present_source->w;
			tex_h = present_source->h;
		}
		SDL_UpdateTexture(texture, nullptr, present_source->pixels, present_source->pitch);
		SDL_RenderClear(renderer);
#ifdef OB_POST_GL
		// SP: present the base frame through the GLES2 post-process stage (identity in Phase
		// 1, sharpen in Phase 2). Takes precedence over the SDL bloom block for now (composing
		// bloom through the shader chain is Phase 3). Any failure falls through to the classic
		// bloom/RenderCopy path below.
		if (post_gl_enabled) {
			if (!post_gl_tried) post_gl_init();
			if (post_gl_present(texture)) { SDL_RenderPresent(renderer); return; }
		} else if (post_fbo || post_prog) {
			// Pipeline was turned off after having run -> free the GL objects (context is
			// current here) rather than holding the full-screen FBO resident until destroy.
			post_gl_teardown();
			post_gl_tried = false;
		}
#endif
		if (bloom_strength > 0) {
			// GPU bloom: downscale the frame into a quarter-res render target
			// (bilinear = blur), draw the base frame, then additively blend the
			// blurred target back on top so bright/emissive areas glow.
			int bw = tex_w / 4, bh = tex_h / 4; if (bw < 1) bw = 1; if (bh < 1) bh = 1;
			if (!bloom_tex || bloom_w != bw || bloom_h != bh) {
				if (bloom_tex) SDL_DestroyTexture(bloom_tex);
				bloom_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, bw, bh);
				bloom_w = bw; bloom_h = bh;
				if (bloom_tex) SDL_SetTextureBlendMode(bloom_tex, SDL_BLENDMODE_ADD);
			}
			if (bloom_tex) {
				SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
				SDL_SetRenderTarget(renderer, bloom_tex);
				SDL_RenderCopy(renderer, texture, nullptr, nullptr); // downscale -> blur
				// Bright-pass: subtract the threshold from the downscaled buffer so
				// only pixels brighter than it survive to bloom (real SC:R glow, not
				// a whole-frame haze). Uses a custom REV_SUBTRACT blend (dst-src);
				// built once, and if the SDL build can't compose it we simply skip
				// the threshold and keep the old uniform bloom.
				if (bloom_thresh > 0) {
					if (bloom_subtract == SDL_BLENDMODE_INVALID) {
						bloom_subtract = SDL_ComposeCustomBlendMode(
							SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
							SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT);
					}
					if (bloom_subtract != SDL_BLENDMODE_INVALID) {
						int t = bloom_thresh > 255 ? 255 : bloom_thresh;
						SDL_SetRenderDrawBlendMode(renderer, bloom_subtract);
						SDL_SetRenderDrawColor(renderer, (Uint8)t, (Uint8)t, (Uint8)t, 0);
						SDL_RenderFillRect(renderer, nullptr); // dst -= threshold
						SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
					}
				}
				SDL_SetRenderTarget(renderer, nullptr);
				SDL_RenderCopy(renderer, texture, nullptr, nullptr); // base frame
				SDL_SetTextureAlphaMod(bloom_tex, (Uint8)(bloom_strength > 255 ? 255 : bloom_strength));
				SDL_RenderCopy(renderer, bloom_tex, nullptr, nullptr); // additive glow
			} else {
				SDL_RenderCopy(renderer, texture, nullptr, nullptr);
			}
		} else {
			SDL_RenderCopy(renderer, texture, nullptr, nullptr);
		}
		SDL_RenderPresent(renderer);
	}
	
	explicit operator bool() const {
		return window != nullptr;
	}

};

window::window() {
	impl = std::make_unique<window_impl>();
}

window::~window() {
}

window::window(window&& n) {
	impl = std::move(n.impl);
}

void window::destroy() {
	impl->destroy();
}

bool window::create(const char* title, int x, int y, int width, int height) {
	return impl->create(title, x, y, width, height);
}

void window::get_render_size(int* width, int* height) {
	impl->update_render_size();
	*width = impl->render_w >= 1 ? impl->render_w : 640;
	*height = impl->render_h >= 1 ? impl->render_h : 480;
}

void window::get_cursor_pos(int* x, int* y) {
	return impl->get_cursor_pos(x, y);
}

void window::set_immediate_drag_rect(int x0, int y0, int x1, int y1) {
	impl->idr_x0 = x0;
	impl->idr_y0 = y0;
	impl->idr_x1 = x1;
	impl->idr_y1 = y1;
}

void window::set_hold_rects(const int* xyxy, int n) {
#ifdef __ANDROID__
	if (n < 0) n = 0;
	if (n > 4) n = 4;
	for (int i = 0; i < n * 4; ++i) impl->hold_r[i] = xyxy[i];
	impl->hold_n = n;
#else
	(void)xyxy; (void)n;
#endif
}

bool window::peek_message(event_t& e) {
	return impl->peek_message(e);
}

#ifdef __ANDROID__
// v0.82.017: SDL_StartTextInput() did not raise the soft keyboard on the test
// tablets, so chat could be opened but never typed into. Ask Android's
// InputMethodManager directly through the activity, which does not depend on
// SDL's internal text-input state.
static void android_soft_keyboard(bool show) {
	JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	if (!env) return;
	jobject activity = (jobject)SDL_AndroidGetActivity();
	if (!activity) return;
	jclass cls = env->GetObjectClass(activity);
	if (cls) {
		jmethodID mid = env->GetMethodID(cls, "showSoftKeyboard", "(Z)V");
		if (mid) env->CallVoidMethod(activity, mid, (jboolean)show);
		if (env->ExceptionCheck()) env->ExceptionClear();
		env->DeleteLocalRef(cls);
	}
	env->DeleteLocalRef(activity);
}
#endif

void window::start_text_input() {
	// Raises the Android soft keyboard and begins delivering SDL_TEXTINPUT.
	// The old guard made a failure permanent: if SDL already believed text
	// input was active, the call was skipped and nothing ever appeared. Stop
	// first so the request always reaches the platform.
	SDL_StopTextInput();
	SDL_StartTextInput();
#ifdef __ANDROID__
	android_soft_keyboard(true);
#endif
}

void window::stop_text_input() {
	if (SDL_IsTextInputActive()) SDL_StopTextInput();
#ifdef __ANDROID__
	android_soft_keyboard(false);
#endif
}

bool window::show_cursor(bool show) {
	return impl->show_cursor(show);
}

bool window::get_key_state(int scancode) {
	return impl->get_key_state(scancode);
}

bool window::get_mouse_button_state(int button) {
	return impl->get_mouse_button_state(button);
}

void window::update_surface() {
	return impl->update_surface();
}

void window::set_bloom(int strength) {
	if (impl) impl->bloom_strength = strength;
}

void window::set_post_gl(bool on, int effect, float strength, float bloom, float grade) {
	if (impl) { impl->post_gl_enabled = on; impl->post_effect = effect; impl->post_strength = strength; impl->post_bloom = bloom; impl->post_grade = grade; }
}

window::operator bool() const {
	return (bool)*impl;
}

}

namespace native_window_drawing {

struct palette_impl : palette {
	SDL_Palette* pal = nullptr;
	palette_impl() {
		pal = SDL_AllocPalette(256);
	}
	virtual ~palette_impl() override {
		SDL_FreePalette(pal);
	}
	virtual void set_colors(color colors[256]) override {
		std::array<SDL_Color, 256> col{};
		for (size_t i = 0; i < 256; ++i) {
			col[i].r = colors[i].r;
			col[i].g = colors[i].g;
			col[i].b = colors[i].b;
			//col[i].a = colors[i].a;
		}
		if (SDL_SetPaletteColors(pal, col.data(), 0, 256)) fatal_error("SDL_SetPaletteColors failed: %s", SDL_GetError());
	}
};

struct sdl_surface: surface {
	SDL_Surface* surf = nullptr;
	void set(SDL_Surface* surf) {
		this->surf = surf;
		w = surf->w;
		h = surf->h;
		pitch = surf->pitch;
	}

	virtual ~sdl_surface() override {
		if (surf) SDL_FreeSurface(surf);
	}
	virtual void set_palette(palette* pal) override {
		if (SDL_SetSurfacePalette(surf, ((palette_impl*)pal)->pal)) fatal_error("SDL_SetSurfacePalette failed: %s", SDL_GetError());
	}
	virtual void* lock() override {
		if (SDL_LockSurface(surf)) fatal_error("SDL_LockSurface failed: %s", SDL_GetError());
		return surf->pixels;
	}
	virtual void unlock() override {
		SDL_UnlockSurface(surf);
	}
	virtual void blit(surface* dst, int x, int y) override {
		auto* s = ((sdl_surface*)dst)->surf;
		if (x == 0 && y == 0) {
			SDL_BlitSurface(surf, nullptr, s, nullptr);
		} else {
			SDL_Rect r{ x, y, s->w, s->h };
			SDL_BlitSurface(surf, nullptr, s, &r);
		}
	}
	virtual void blit_scaled(surface* dst, int x, int y, int w, int h) override {
		auto* s = ((sdl_surface*)dst)->surf;
		SDL_Rect r{x, y, w, h};
		SDL_BlitScaled(surf, nullptr, s, &r);
	}
	virtual void fill(int r, int g, int b, int a) override {
		SDL_FillRect(surf, nullptr, SDL_MapRGBA(surf->format, r, g, b, a));
	}
	virtual void set_alpha(int a) override {
		SDL_SetSurfaceAlphaMod(surf, a);
	}
	virtual void set_blend_mode(blend_mode blend) override {
		if (blend == blend_mode::none) SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_NONE);
		else if (blend == blend_mode::alpha) SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
		else if (blend == blend_mode::add) SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_ADD);
		else if (blend == blend_mode::mod) SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_MOD);
	}
};

std::unique_ptr<surface> create_rgba_surface(int width, int height) {
	//SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
	// ARGB8888 packed layout (matches the texture format in update_surface).
	// The whole RGBA HUD layer (draw_text_rgba, fill_rect_rgba, and the
	// game_palette/cmdicon_rgb/console_pal blits) writes colors as 0xAARRGGBB
	// (red in bits 16-23), so the present surface must use the same channel
	// order or red and blue come out swapped (cyan renders as gold, etc.).
	// The world is unaffected: SDL fills it via an indexed->surface blit that
	// respects whatever format the surface declares.
	Uint32 rmask = 0x00ff0000;
	Uint32 gmask = 0x0000ff00;
	Uint32 bmask = 0x000000ff;
	Uint32 amask = 0xff000000;
    SDL_Surface* surf = SDL_CreateRGBSurface(0, width, height, 32, rmask, gmask, bmask, amask);
	if (!surf) fatal_error("SDL_CreateRGBSurfaceWithFormat failed: %s", SDL_GetError());
	auto r = std::make_unique<sdl_surface>();
	r->set(surf);
	return std::unique_ptr<surface>(r.release());
}

std::unique_ptr<surface> get_window_surface(native_window::window* wnd, int width, int height) {
	// Not the actual window framebuffer: an offscreen surface at the
	// caller's render size. window::update_surface() streams it to the
	// screen through the renderer (GPU upscale + vsync). The size must be
	// the caller's, not queried here: the window can be mid-layout and a
	// size mismatch with the caller's drawing dimensions overflows the
	// buffer. The caller owns the surface; the window keeps a borrowed
	// pointer to it as the present source, valid until the next
	// get_window_surface() call replaces it.
	auto* impl = wnd->impl.get();
	auto r = create_rgba_surface(width, height);
	impl->present_source = ((sdl_surface*)r.get())->surf;
	if (impl->renderer) SDL_RenderSetLogicalSize(impl->renderer, width, height);
	impl->render_w = width;
	impl->render_h = height;
	return r;
}

std::unique_ptr<surface> convert_to_8_bit_indexed(surface* s) {
	auto* surf = SDL_ConvertSurfaceFormat(((sdl_surface*)s)->surf, SDL_PIXELFORMAT_INDEX8, 0);
	if (!surf) fatal_error("SDL_ConvertSurfaceFormat failed: %s", SDL_GetError());
	auto r = std::make_unique<sdl_surface>();
	r->set(surf);
	return std::unique_ptr<surface>(r.release());
}

palette* new_palette() {
	return new palette_impl();
}
void delete_palette(palette* pal) {
	delete pal;
}

std::unique_ptr<surface> load_image(const char* filename) {
#ifndef OPENBW_NO_SDL_IMAGE
	auto* surf = IMG_Load(filename);
	if (!surf) fatal_error("IMG_Load(%s) failed: %s", filename, IMG_GetError());
	auto r = std::make_unique<sdl_surface>();
	r->set(surf);
	return std::unique_ptr<surface>(r.release());
#else
	return nullptr;
#endif
}

std::unique_ptr<surface> load_image(const void* data, size_t size) {
#ifndef OPENBW_NO_SDL_IMAGE
	auto* surf = IMG_Load_RW(SDL_RWFromConstMem(data, (int)size), 1);
	if (!surf) fatal_error("IMG_Load_RW(mem) failed: %s", IMG_GetError());
	auto r = std::make_unique<sdl_surface>();
	r->set(surf);
	return std::unique_ptr<surface>(r.release());
#else
	return nullptr;
#endif
}

}

namespace native_sound {

bool initialized = false;

int frequency = 0;
int channels = 64;

#ifndef OPENBW_NO_SDL_MIXER

void init() {
	if (initialized) return;
	initialized = true;
	Mix_Init(0);
	int freq = frequency;
	if (freq == 0) {
//#ifdef EMSCRIPTEN
#if 0
		freq = EM_ASM_INT_V({
			var context;
			try {
				context = new AudioContext();
			} catch (e) {
				context = new webkitAudioContext();
			}
			return context.sampleRate;
		});
#else
		freq = MIX_DEFAULT_FREQUENCY;
#endif
	}
	Mix_OpenAudio(freq, MIX_DEFAULT_FORMAT, 2, 1024);
	Mix_AllocateChannels(channels);
}

struct sdl_sound: sound {
	Mix_Chunk* c = nullptr;
	virtual ~sdl_sound() override {
		if (c) Mix_FreeChunk(c);
	}
};

void play(int channel, sound* arg_s, int volume, int pan) {
	if (!initialized) init();
	sdl_sound* s = (sdl_sound*)arg_s;
	if (!s || !s->c) return;
	int c = Mix_PlayChannel(channel, s->c, 0);
	if (c != -1) {
		Mix_Volume(c, volume);
		int left = 254;
		int right = 254;
		if (pan < 0) right += pan;
		else left -= pan;
		Mix_SetPanning(c, (Uint8)left, (Uint8)right);
	}
}

bool is_playing(int channel) {
	return Mix_Playing(channel) != 0;
}

void stop(int channel) {
	Mix_HaltChannel(channel);
}

void set_volume(int channel, int volume) {
	Mix_Volume(channel, volume);
}

std::unique_ptr<sound> load_wav(const void* data, size_t size) {
	if (!initialized) init();
	Mix_Chunk* c = Mix_LoadWAV_RW(SDL_RWFromConstMem(data, (int)size), 1);
	if (!c) return {};
	auto r = std::make_unique<sdl_sound>();
	r->c = c;
	return std::unique_ptr<sound>(r.release());
}

// --- Music (streamed via the Mix_Music API, independent of SFX channels) ---
// The RWops streams from music_data, so the buffer must outlive the music
// object; both are freed together on the next play/stop.
static Mix_Music* current_music = nullptr;
static std::vector<char> music_data;

void stop_music() {
	if (current_music) {
		Mix_HaltMusic();
		Mix_FreeMusic(current_music);
		current_music = nullptr;
	}
	music_data.clear();
	music_data.shrink_to_fit();
}

bool play_music(const void* data, size_t size, int volume) {
	if (!initialized) init();
	stop_music();
	music_data.assign((const char*)data, (const char*)data + size);
	Mix_Music* m = Mix_LoadMUS_RW(SDL_RWFromConstMem(music_data.data(), (int)music_data.size()), 1);
	if (!m) {
		music_data.clear();
		return false;
	}
	current_music = m;
	Mix_VolumeMusic(volume);
	Mix_PlayMusic(m, 1);
	return true;
}

bool music_playing() {
	return current_music && Mix_PlayingMusic() != 0;
}

void set_music_volume(int volume) {
	Mix_VolumeMusic(volume);
}

#else

void init() {
}

struct sdl_sound: sound {
	virtual ~sdl_sound() override {}
};

void play(int channel, sound* arg_s, int volume, int pan) {
}

bool is_playing(int channel) {
	return false;
}

void stop(int channel) {
}

void set_volume(int channel, int volume) {
}

std::unique_ptr<sound> load_wav(const void* data, size_t size) {
	return nullptr;
}

bool play_music(const void* data, size_t size, int volume) {
	return false;
}

bool music_playing() {
	return false;
}

void stop_music() {
}

void set_music_volume(int volume) {
}

#endif

}
