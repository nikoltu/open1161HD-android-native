#pragma once
// Keyboard + mouse default bindings: the one table read by the game (kbm.h) and by the
// launcher (portrait_jni.cpp, nativeKeyDefaults). Order = the kb_* action enum in kbm.h.
// A code is an SDL scancode, or 1000 + an SDL mouse button.

namespace kbm_defaults {

struct entry_t { const char* id; int code; };

static constexpr int mouse_base = 1000;

static constexpr entry_t table[] = {
	{"cam_up", 82}, {"cam_down", 81}, {"cam_left", 80}, {"cam_right", 79},
	{"card1", 20}, {"card2", 26}, {"card3", 8}, {"card4", 4}, {"card5", 22}, {"card6", 7}, {"card7", 29}, {"card8", 27}, {"card9", 6},
	{"cancel", 41}, {"menu", 67}, {"chat", 40}, {"alert", 44}, {"idle", 58}, {"army", 59}, {"center", 9}, {"zoom_in", 46}, {"zoom_out", 45}, {"subgroup", 43},
	{"group0", 39}, {"group1", 30}, {"group2", 31}, {"group3", 32}, {"group4", 33}, {"group5", 34}, {"group6", 35}, {"group7", 36}, {"group8", 37}, {"group9", 38},
	{"camloc1", 62}, {"camloc2", 63}, {"camloc3", 64},
	{"mouse_select", mouse_base + 1}, {"mouse_command", mouse_base + 3}, {"mouse_pan", mouse_base + 2},
};

static constexpr int count = (int)(sizeof table / sizeof table[0]);

}
