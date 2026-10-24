#ifndef OPENBW_UI_LOADING_SCREEN_H
#define OPENBW_UI_LOADING_SCREEN_H

// Loading-screen renderer (melee: centred minimap + the two sides; campaign:
// the mission title), drawn in the launcher's design language (namespace pal:
// dimmed space backdrop, black panels with a dark red hairline, green section
// labels, yellow accent, green progress bar). Deliberately standalone: it depends only on plain data
// (an RGBA buffer + an info struct) and the Unispace coverage atlas, NOT on the
// engine state or SDL, so the exact code the app ships is also what the host
// harness (tools/loadscreen_check.cpp) renders to a PNG and inspects.
//
// ui.h fills info_t from the live game (st.players, start locations, the lobby
// slot table, the campaign dir) and calls draw() from both loading paths -- the
// HD asset prewarm screen and the LAN "net-fast" loading screen.
//
// Pixels are 0xAARRGGBB (the layout ui.h's RGBA HUD pass uses); colours passed
// in are 0x00RRGGBB. Everything sizes off the buffer height, so the screen reads
// the same at 1280x720, 1920x1200 and 2960x1848.

#include "font_unispace.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace bwgame {
namespace loading_screen {

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct player_t {
	std::string name;          // already display-safe (see ascii_text)
	int race = -1;             // 0 Zerg, 1 Terran, 2 Protoss; anything else = Random / not picked
	bool race_random = false;  // picked Random (a known race is then shown as "Zerg (Random)")
	uint32_t color = 0x808080; // player colour, 0x00RRGGBB
	int team = 0;              // players sharing a team id are allies
	bool is_local = false;     // highlighted
	std::string role;          // optional second-line note: "Computer", "Medium", "Bot"...
	// Portrait tile (launcher redesign F4): the player's unit portrait replaces the
	// colour badge -- the colour becomes the tile's frame, the race letter a corner
	// tag. `portrait_tile` asks for the tile; with no pixels (race hidden, clip
	// missing) it is a "no signal" static tile, so a portrait can never leak a race.
	bool portrait_tile = false;
	int portrait_id = -1;      // only feeds the cache signature
	int portrait_w = 0, portrait_h = 0;
	std::shared_ptr<const std::vector<uint32_t>> portrait; // portrait_w * portrait_h, 0x00RRGGBB (a still frame)
};

struct marker_t {
	float x = 0, y = 0;        // 0..1 across the map
	uint32_t color = 0x808080;
	bool active = false;       // a listed player starts here (else: an unused start location)
};

struct info_t {
	enum kind_t { none, melee, campaign };
	kind_t kind = none;

	// melee
	std::string map_name;      // under the minimap
	std::string map_detail;    // smaller line under the name ("128 x 128  -  Jungle World")
	std::vector<uint32_t> minimap; // minimap_w * minimap_h, 0x00RRGGBB, one pixel per tile
	int minimap_w = 0, minimap_h = 0;
	uint32_t minimap_rev = 0;  // bump when the pixels change (they are not hashed)
	std::vector<marker_t> markers;
	std::vector<player_t> players; // local player first

	// campaign
	std::string title;         // the mission's real title
	std::string overline;      // "EPISODE I  -  REBEL YELL"
	std::string underline;     // "TERRAN CAMPAIGN  -  MISSION 1"
};

// The launcher's colour tokens (the Java twin is Kit.java). The loading screen and the mission briefing draw with these
// so the launcher, the loading screen and the briefing read as one product.
namespace pal {
	const uint32_t base = 0x05080a;      // screen background under the backdrop
	const uint32_t panel = 0x06090c;     // panel fill, drawn at panel_alpha
	const int panel_alpha = 219;         // 86 %
	const uint32_t line = 0x5a1717;      // panel hairline (dark red)
	const uint32_t rim = 0x1b2227;       // faint outer ring around a panel
	const uint32_t steel = 0x5f6b72;     // secondary edges
	const uint32_t steel_top = 0x8b979e, steel_bot = 0x3b464c; // the panel's corner tab
	const uint32_t text = 0xcfe6cc;      // body text
	const uint32_t text2 = 0x7f9486;     // secondary text
	const uint32_t title = 0xffffff;     // titles, names
	const uint32_t label = 0x4fd46a;     // section labels, selected edge
	const uint32_t accent = 0xf2c53d;    // yellow accent (same as HUD v2)
	const uint32_t danger = 0xe0564e;
	const uint32_t track = 0x1a2226;     // progress track
	const uint32_t sel_top = 0x1f6b34, sel_bot = 0x114420; // primary / selected gradient
	const uint32_t btn_top = 0x2a3338, btn_bot = 0x161c20; // secondary button gradient
	const int sel_tint = 36;             // label green over a panel = "selected row" (14 %)
	const int backdrop_alpha = 128;      // the launcher shows the backdrop at 50 %
}

struct style_t {
	// Backdrop: the main-menu space scene (glue\palmm\backgnd.pcx) from the user's
	// own MPQ, decoded by the caller. Null = no game files -> a dark gradient.
	std::shared_ptr<const std::vector<uint32_t>> backdrop; // backdrop_w * backdrop_h, 0x00RRGGBB
	int backdrop_w = 0, backdrop_h = 0;
	bool cancel = false;                               // net path: abort button next to the bar
	std::string cancel_label = "CANCEL";
	std::string status;                                // optional line above the bar (net path)
	int64_t anim_ms = 0;                               // wall clock for the bar shimmer
	// Words the renderer itself authors (the caller localises them).
	std::string s_vs = "VS", s_ffa = "FREE FOR ALL", s_team = "TEAM", s_random = "Random";
	std::string s_race[3] = {"Zerg", "Terran", "Protoss"};
};

struct rect_t { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

// 8-bit RLE PCX with the 256-colour trailer (the format of BW's glue\ screens)
// -> 0x00RRGGBB. Plain bytes in, pixels out, so the app (ui.h) and the host
// harness decode the backdrop with the same code. False on anything unexpected.
inline bool decode_pcx8(const uint8_t* d, size_t n, std::vector<uint32_t>& px, int& w_out, int& h_out) {
	px.clear(); w_out = h_out = 0;
	// header: 0x0A, version, encoding 1 (RLE), 8 bpp, window, ..., planes @65, bytes/line @66
	if (!d || n <= 128 + 769 || d[0] != 0x0a || d[2] != 1 || d[3] != 8 || d[65] != 1 || d[n - 769] != 0x0c) return false;
	const int x_min = d[4] | (d[5] << 8), y_min = d[6] | (d[7] << 8), x_max = d[8] | (d[9] << 8), y_max = d[10] | (d[11] << 8);
	const int w = x_max - x_min + 1, h = y_max - y_min + 1;
	const size_t bpl = (size_t)(d[66] | (d[67] << 8));
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bpl < (size_t)w) return false;
	const uint8_t* pal_rgb = d + n - 768;
	px.assign((size_t)w * h, 0);
	size_t pos = 128; const size_t end = n - 769;
	std::vector<uint8_t> line(bpl);
	for (int y = 0; y != h; ++y) {
		size_t x = 0;
		while (x < bpl && pos < end) {
			uint8_t b = d[pos++];
			size_t run = 1;
			if ((b & 0xc0) == 0xc0) { run = b & 0x3f; if (pos >= end) break; b = d[pos++]; }
			while (run-- && x < bpl) line[x++] = b;
		}
		uint32_t* dst = px.data() + (size_t)y * w;
		for (int i = 0; i != w; ++i) {
			const uint8_t* c = pal_rgb + (size_t)line[(size_t)i] * 3;
			dst[i] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
		}
	}
	w_out = w; h_out = h;
	return true;
}

// The static composition (backdrop, minimap, cards, title) is rendered once and
// re-blitted; only the progress bar / status line are redrawn per frame. Keyed
// by a hash of everything that feeds the static part.
struct cache_t {
	std::vector<uint32_t> px;
	int w = 0, h = 0;
	uint64_t sig = 0;
	void release() { std::vector<uint32_t>().swap(px); w = h = 0; sig = 0; }
};

// BW's eight player colours (index = player_t::color in the engine).
inline uint32_t bw_player_color(int index) {
	static const uint32_t tbl[8] = {
		0xf40404, // red
		0x0c48cc, // blue
		0x2cb494, // teal
		0x88409c, // purple
		0xf88c14, // orange
		0x703014, // brown
		0xcce0d0, // white
		0xfcfc38, // yellow
	};
	return (index >= 0 && index < 8) ? tbl[index] : 0x808080;
}

// ---------------------------------------------------------------------------
// Text safety: the atlas only has ASCII 32..126. Map names / player names can be
// UTF-8 (or CP949 on Korean maps) and BW scenario strings carry colour control
// bytes. Fold what has an obvious ASCII form, drop the rest, never crash.
// ---------------------------------------------------------------------------

inline std::string ascii_text(const std::string& s, const std::string& fallback = std::string()) {
	static const char latin1[] =
		"AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
	static const char latin_a[] =
		"AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
		"lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
	std::string out;
	size_t i = 0, n = s.size();
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		uint32_t cp = 0; size_t len = 1;
		if (c < 0x80) cp = c;
		else if ((c & 0xe0) == 0xc0 && i + 1 < n && ((unsigned char)s[i + 1] & 0xc0) == 0x80) {
			cp = ((uint32_t)(c & 0x1f) << 6) | ((unsigned char)s[i + 1] & 0x3f); len = 2;
		} else if ((c & 0xf0) == 0xe0 && i + 2 < n && ((unsigned char)s[i + 1] & 0xc0) == 0x80
				&& ((unsigned char)s[i + 2] & 0xc0) == 0x80) {
			cp = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)((unsigned char)s[i + 1] & 0x3f) << 6)
				| ((unsigned char)s[i + 2] & 0x3f); len = 3;
		} else if ((c & 0xf8) == 0xf0 && i + 3 < n) { cp = 0xfffd; len = 4; }
		else cp = 0xfffd; // stray high byte (CP949 / Latin-1 file names): dropped
		i += len;
		if (cp == '\t' || cp == '\n' || cp == '\r') out += ' ';
		else if (cp < 0x20 || cp == 0x7f) continue;            // BW colour / control codes
		else if (cp < 0x7f) out += (char)cp;
		else if (cp >= 0xc0 && cp <= 0xff) out += latin1[cp - 0xc0];
		else if (cp >= 0x100 && cp <= 0x17f) out += latin_a[cp - 0x100];
		else if (cp == 0x2018 || cp == 0x2019) out += '\'';
		else if (cp == 0x201c || cp == 0x201d) out += '"';
		else if (cp == 0x2013 || cp == 0x2014) out += '-';
		else if (cp == 0xa0) out += ' ';
		// everything else (CJK, symbols) has no glyph: dropped
	}
	// Collapse runs of spaces, trim.
	std::string r;
	for (char ch : out) {
		if (ch == ' ' && (r.empty() || r.back() == ' ')) continue;
		r += ch;
	}
	while (!r.empty() && r.back() == ' ') r.pop_back();
	// A name that lost most of itself ("()", "-") is worse than the fallback.
	bool has_alnum = false;
	for (char ch : r) if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) { has_alnum = true; break; }
	if (!has_alnum) return fallback;
	return r;
}

inline std::string upper(std::string s) {
	for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	return s;
}

// "maps/(2)Bottleneck.scm" -> "(2)Bottleneck". Used for the map label; the
// file name is the reliable one (CHK scenario names are often localized or
// decorated with colour codes).
inline std::string map_label_from_path(const std::string& path) {
	std::string s = path;
	size_t sl = s.find_last_of("/\\");
	if (sl != std::string::npos) s = s.substr(sl + 1);
	size_t dot = s.rfind('.');
	if (dot != std::string::npos && dot > 0 && s.size() - dot <= 5) s = s.substr(0, dot);
	// "(2)Bottleneck" -> "(2) Bottleneck" (how the launcher's map list spells it).
	if (s.size() > 3 && s[0] == '(') {
		size_t close = s.find(')');
		if (close != std::string::npos && close <= 3 && close + 1 < s.size() && s[close + 1] != ' ') s.insert(close + 1, " ");
	}
	return s;
}

// Fair play: BW never tells you where the enemy starts. A start location gets a
// coloured player marker only for the local side; the enemy's are drawn as
// neutral rings like the unused ones -- except on a two-start map, where the
// other position is no secret.
inline bool marker_revealed(bool local_side, size_t start_location_count) {
	return local_side || start_location_count <= 2;
}

inline const char* tileset_label(int tileset_index) {
	static const char* names[8] = {"Badlands", "Space Platform", "Installation", "Ash World",
		"Jungle World", "Desert", "Ice", "Twilight"};
	return (tileset_index >= 0 && tileset_index < 8) ? names[tileset_index] : "";
}

// ---------------------------------------------------------------------------
// Campaign titles.
//
// MEASURED (aidump mapinfo over all 65 mission CHKs): the CHK scenario name is
// NOT a usable title source. Retail missions carry development names
// ("T5) The Antigan Revolt" for Revolution, "T6) New Year's Day" for Norad II,
// "Z8) The Dark Templar" for Eye for an Eye), eight are "Untitled Scenario",
// and zerg03's is a WAV path. So the shipped titles come from this table (the
// same names the launcher's mission list shows); the CHK name is only the
// fallback for a mission dir the table does not know (mods).
// ---------------------------------------------------------------------------

struct campaign_words_t {
	std::string episode = "EPISODE", mission = "MISSION", campaign = "CAMPAIGN",
		tutorial = "TUTORIAL", secret = "SECRET MISSION";
};

inline std::string clean_scenario_name(const std::string& raw) {
	std::string s = ascii_text(raw);
	if (s.empty()) return s;
	if (s.find('\\') != std::string::npos || s.find(".wav") != std::string::npos) return std::string();
	if (s == "Untitled Scenario" || s == "Untitled Scenario.") return std::string();
	// Strip a "T5) " style prefix.
	if (s.size() > 4) {
		size_t p = 1;
		while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
		if (p > 1 && p < s.size() && s[p] == ')' && (s[0] == 'T' || s[0] == 'Z' || s[0] == 'P')) {
			s = s.substr(p + 1);
			while (!s.empty() && s[0] == ' ') s.erase(0, 1);
		}
	}
	return s;
}

// Fills info.title/overline/underline for a campaign dir like
// "campaign/terran/terran01". Always produces a title (never empty).
inline void build_campaign_info(const std::string& campaign_dir, const std::string& raw_scenario_name,
		info_t& info, const campaign_words_t& words = campaign_words_t()) {
	struct ep_t { const char* dir; const char* numeral; const char* name; const char* race; bool bw; };
	static const ep_t eps[] = {
		{"terran", "I", "REBEL YELL", "TERRAN", false},
		{"zerg", "II", "OVERMIND", "ZERG", false},
		{"protoss", "III", "THE FALL", "PROTOSS", false},
		{"expprotoss", "IV", "THE STAND", "PROTOSS", true},
		{"expterran", "V", "THE IRON FIST", "TERRAN", true},
		{"expzerg", "VI", "THE QUEEN OF BLADES", "ZERG", true},
	};
	struct m_t { const char* dir; const char* title; };
	static const m_t missions[] = {
		{"terran/tutorial", "Boot Camp"},
		{"terran/terran01", "Wasteland"},
		{"terran/terran02", "Backwater Station"},
		{"terran/terran03", "Desperate Alliance"},
		{"terran/terran04", "The Jacobs Installation"},
		{"terran/terran05", "Revolution"},
		{"terran/terran06", "Norad II"},
		{"terran/terran07", "The Trump Card"},
		{"terran/terran08", "The Big Push"},
		{"terran/terran09", "New Gettysburg"},
		{"terran/terran10", "The Hammer Falls"},
		{"zerg/zerg01", "Among the Ruins"},
		{"zerg/zerg02", "Egression"},
		{"zerg/zerg03", "The New Dominion"},
		{"zerg/zerg04", "Agent of the Swarm"},
		{"zerg/zerg05", "The Amerigo"},
		{"zerg/zerg06", "The Dark Templar"},
		{"zerg/zerg07", "The Culling"},
		{"zerg/zerg08", "Eye for an Eye"},
		{"zerg/zerg09", "The Invasion of Aiur"},
		{"zerg/zerg10", "Full Circle"},
		{"protoss/protoss01", "First Strike"},
		{"protoss/protoss02", "Into the Flames"},
		{"protoss/protoss03", "Higher Ground"},
		{"protoss/protoss04", "The Hunt for Tassadar"},
		{"protoss/protoss05", "Choosing Sides"},
		{"protoss/protoss06", "Into the Darkness"},
		{"protoss/protoss07", "Homeland"},
		{"protoss/protoss08", "The Trial of Tassadar"},
		{"protoss/protoss09", "Shadow Hunters"},
		{"protoss/protoss10", "Eye of the Storm"},
		{"expprotoss/protoss01", "Escape from Aiur"},
		{"expprotoss/protoss02", "Dunes of Shakuras"},
		{"expprotoss/protoss03", "Legacy of the Xel'Naga"},
		{"expprotoss/protoss04", "The Quest for Uraj"},
		{"expprotoss/protoss05", "The Battle of Braxis"},
		{"expprotoss/protoss06", "Return to Char"},
		{"expprotoss/protoss07", "The Insurgent"},
		{"expprotoss/protoss08", "Countdown"},
		{"expterran/terran01", "First Strike"},
		{"expterran/terran02", "The Dylarian Shipyards"},
		{"expterran/terran03", "Ruins of Tarsonis"},
		{"expterran/terran04", "Assault on Korhal"},
		{"expterran/terran05a", "Emperor's Fall (Ground Zero)"},
		{"expterran/terran05b", "Emperor's Fall (Birds of War)"},
		{"expterran/terran06", "Emperor's Flight"},
		{"expterran/terran07", "Patriot's Blood"},
		{"expterran/terran08", "To Chain the Beast"},
		{"expzerg/zerg01", "Vile Disruption"},
		{"expzerg/zerg02", "Reign of Fire"},
		{"expzerg/zerg03", "The Kel-Morian Combine"},
		{"expzerg/zerg04a", "The Liberation of Korhal"},
		{"expzerg/zerg04b", "The Liberation of Korhal"},
		{"expzerg/zerg04c", "The Liberation of Korhal"},
		{"expzerg/zerg04d", "The Liberation of Korhal"},
		{"expzerg/zerg04e", "The Liberation of Korhal"},
		{"expzerg/zerg04f", "The Liberation of Korhal"},
		{"expzerg/zerg05", "True Colors"},
		{"expzerg/zerg06", "Fury of the Swarm"},
		{"expzerg/zerg07", "Drawing of the Web"},
		{"expzerg/zerg08", "To Slay the Beast"},
		{"expzerg/zerg09", "The Reckoning"},
		{"expzerg/zerg10", "Omega"},
		{"expzerg/bonus", "Dark Origin"},
	};
	// Normalise: lower-case, forward slashes, no "campaign/" prefix, no trailing slash.
	std::string d;
	for (char c : campaign_dir) d += (c == '\\') ? '/' : (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
	while (!d.empty() && d.back() == '/') d.pop_back();
	if (d.compare(0, 9, "campaign/") == 0) d = d.substr(9);
	std::string ep_dir = d, mission = d;
	size_t sl = d.find('/');
	if (sl != std::string::npos) { ep_dir = d.substr(0, sl); mission = d.substr(d.rfind('/') + 1); }

	info.kind = info_t::campaign;
	info.title.clear(); info.overline.clear(); info.underline.clear();
	for (auto& m : missions) if (d == m.dir) { info.title = m.title; break; }
	const bool is_tutorial = mission == "tutorial";
	const bool is_bonus = mission == "bonus";
	// The Zerg/Protoss "tutorial" dirs are placeholders (protoss/tutorial's CHK is a
	// copy of First Strike), so they must not take the CHK name.
	if (info.title.empty() && is_tutorial) info.title = "Tutorial";
	if (info.title.empty()) info.title = clean_scenario_name(raw_scenario_name);
	if (info.title.empty()) {
		// Last resort: the mission dir itself, e.g. "mymod07" -> "Mymod07".
		std::string t = ascii_text(mission, "Mission");
		if (!t.empty() && t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 'a' + 'A');
		info.title = t;
	}
	// Mission number = the digits in the mission dir ("terran05a" -> 5).
	int number = 0; bool have_number = false;
	for (char c : mission) {
		if (c >= '0' && c <= '9') { number = number * 10 + (c - '0'); have_number = true; }
		else if (have_number) break;
	}
	const ep_t* ep = nullptr;
	for (auto& e : eps) if (ep_dir == e.dir) { ep = &e; break; }
	if (ep) {
		info.overline = (ep->bw ? std::string("BROOD WAR  -  ") : std::string())
			+ upper(words.episode) + " " + ep->numeral + "  -  " + ep->name;
		info.underline = std::string(ep->race) + " " + upper(words.campaign);
	} else {
		info.underline = upper(words.campaign);
	}
	if (is_tutorial) info.underline += "  -  " + upper(words.tutorial);
	else if (is_bonus) info.underline += "  -  " + upper(words.secret);
	else if (have_number && number > 0) info.underline += "  -  " + upper(words.mission) + " " + std::to_string(number);
}

// ---------------------------------------------------------------------------
// Canvas + primitives (all clipped)
// ---------------------------------------------------------------------------

struct canvas {
	uint32_t* data = nullptr; size_t pitch = 0; int w = 0, h = 0;
	// Optional window: the buffer holds only the sub-rectangle (wx, wy, ww, wh) of
	// a w x h screen, `data` pointing at the window's first pixel. Coordinates stay
	// screen coordinates and every size the primitives derive from `h` stays the
	// full screen's, so a small cached layer (the replay player's bottom bar, its
	// side panel) is drawn exactly as it would be on the screen itself. ww == 0 =
	// no window (the whole screen). Only blend()-based primitives honour it
	// (fill, frame, disc, text, panel, button) -- not draw_backdrop / draw_minimap.
	int wx = 0, wy = 0, ww = 0, wh = 0;
	void window(int x, int y, int width, int height) { wx = x; wy = y; ww = width; wh = height; }

	inline void blend(int x, int y, uint32_t rgb, int a) {
		if (ww) {
			x -= wx; y -= wy;
			if ((unsigned)x >= (unsigned)ww || (unsigned)y >= (unsigned)wh || a <= 0) return;
		} else if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h || a <= 0) return;
		uint32_t& d = data[(size_t)y * pitch + x];
		if (a >= 255) { d = 0xff000000u | (rgb & 0xffffff); return; }
		int ia = 255 - a;
		uint32_t r = (((rgb >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) / 255;
		uint32_t g = (((rgb >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia) / 255;
		uint32_t b = ((rgb & 0xff) * a + (d & 0xff) * ia) / 255;
		d = 0xff000000u | (r << 16) | (g << 8) | b;
	}
	void fill(int x0, int y0, int x1, int y1, uint32_t rgb, int a = 255) {
		if (ww) {
			if (x0 < wx) x0 = wx; if (y0 < wy) y0 = wy; if (x1 > wx + ww) x1 = wx + ww; if (y1 > wy + wh) y1 = wy + wh;
		} else {
			if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
		}
		for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) blend(x, y, rgb, a);
	}
	// Rectangle outline, t pixels thick, drawn inside the rect.
	void frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, int a = 255) {
		if (t < 1) t = 1;
		fill(x0, y0, x1, y0 + t, rgb, a);
		fill(x0, y1 - t, x1, y1, rgb, a);
		fill(x0, y0 + t, x0 + t, y1 - t, rgb, a);
		fill(x1 - t, y0 + t, x1, y1 - t, rgb, a);
	}
	// Anti-aliased disc.
	void disc(float cx, float cy, float r, uint32_t rgb, int a = 255) {
		int x0 = (int)std::floor(cx - r - 1), x1 = (int)std::ceil(cx + r + 1);
		int y0 = (int)std::floor(cy - r - 1), y1 = (int)std::ceil(cy + r + 1);
		for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			float cov = r + 0.5f - std::sqrt(dx * dx + dy * dy);
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			blend(x, y, rgb, (int)(a * cov));
		}
	}
};

// --- text ------------------------------------------------------------------
// The Unispace atlas is a 9x17 coverage bitmap. The HUD upscales it by integer
// nearest-neighbour, which is crisp at 1-2x but blocky at the 5-10x a loading
// title needs. Here the coverage is sampled bilinearly and re-thresholded
// around 50% (the usual trick for scaling a coverage/alpha mask), which keeps
// the glyph edges smooth and one-pixel anti-aliased at any size, including
// fractional ones -- so text can be sized exactly to the screen height.

inline float text_advance(float h, float tracking = 0) {
	return (ui::unispace_w + 1) * (h / ui::unispace_h) + tracking * h;
}
inline float text_width(const std::string& s, float h, float tracking = 0) {
	if (s.empty()) return 0;
	return s.size() * text_advance(h, tracking) - (h / ui::unispace_h) - tracking * h;
}

inline void draw_text_plain(canvas& c, float x, float y, const std::string& s, uint32_t rgb, float h, float tracking, int alpha) {
	const int W = ui::unispace_w, H = ui::unispace_h;
	const float scale = h / H;
	if (scale <= 0.05f) return;
	const float adv = text_advance(h, tracking);
	float k = scale / 1.4f; if (k < 1) k = 1; // edge re-sharpening (1 = plain bilinear)
	int y0 = (int)std::floor(y), y1 = (int)std::ceil(y + h);
	if (y1 <= 0 || y0 >= c.h) return;
	for (size_t i = 0; i != s.size(); ++i) {
		unsigned char ch = (unsigned char)s[i];
		if (ch < ui::unispace_first || ch >= ui::unispace_first + ui::unispace_count || ch == ' ') continue;
		const unsigned char* gl = ui::unispace_data + (size_t)(ch - ui::unispace_first) * H * W;
		float gx = x + i * adv;
		int x0 = (int)std::floor(gx), x1 = (int)std::ceil(gx + W * scale);
		if (x1 <= 0 || x0 >= c.w) continue;
		for (int py = y0; py < y1; ++py) {
			float v = (py + 0.5f - y) / scale - 0.5f;
			int vi = (int)std::floor(v); float vf = v - vi;
			for (int px = x0; px < x1; ++px) {
				float u = (px + 0.5f - gx) / scale - 0.5f;
				int uidx = (int)std::floor(u); float uf = u - uidx;
				auto tap = [&](int ix, int iy) -> float {
					if (ix < 0 || iy < 0 || ix >= W || iy >= H) return 0.0f;
					return gl[iy * W + ix];
				};
				float cov = (tap(uidx, vi) * (1 - uf) + tap(uidx + 1, vi) * uf) * (1 - vf)
					+ (tap(uidx, vi + 1) * (1 - uf) + tap(uidx + 1, vi + 1) * uf) * vf;
				float a = (cov / 255.0f - 0.5f) * k + 0.5f;
				if (a <= 0.02f) continue;
				if (a > 1) a = 1;
				c.blend(px, py, rgb, (int)(alpha * a));
			}
		}
	}
}

// Text with a soft drop shadow (readability over the minimap glow / cards).
inline void draw_text(canvas& c, float x, float y, const std::string& s, uint32_t rgb, float h,
		float tracking = 0, int alpha = 255, bool shadow = true) {
	if (shadow) {
		float o = h / 14.0f; if (o < 1) o = 1;
		draw_text_plain(c, x + o, y + o, s, 0x000000, h, tracking, alpha * 170 / 255);
	}
	draw_text_plain(c, x, y, s, rgb, h, tracking, alpha);
}
inline void draw_text_centered(canvas& c, float cx, float y, const std::string& s, uint32_t rgb, float h,
		float tracking = 0, int alpha = 255) {
	draw_text(c, cx - text_width(s, h, tracking) / 2, y, s, rgb, h, tracking, alpha);
}

// Shrink h (down to min_h) so s fits max_w.
inline float fit_height(const std::string& s, float h, float min_h, float max_w, float tracking = 0) {
	float w = text_width(s, h, tracking);
	if (w <= max_w || w <= 0) return h;
	float nh = h * max_w / w;
	return nh < min_h ? min_h : nh;
}
// Cut s with ".." so it fits max_w at height h.
inline std::string ellipsize(const std::string& s, float h, float max_w, float tracking = 0) {
	if (text_width(s, h, tracking) <= max_w) return s;
	std::string r = s;
	while (r.size() > 1) {
		r.pop_back();
		while (!r.empty() && r.back() == ' ') r.pop_back();
		if (text_width(r + "..", h, tracking) <= max_w) return r + "..";
	}
	return r;
}

inline uint32_t mix(uint32_t a, uint32_t b, float t) {
	if (t < 0) t = 0; if (t > 1) t = 1;
	auto ch = [&](int sh) { return (uint32_t)(((a >> sh) & 0xff) * (1 - t) + ((b >> sh) & 0xff) * t + 0.5f); };
	return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
inline int luma(uint32_t rgb) {
	return (int)(((rgb >> 16) & 0xff) * 299 + ((rgb >> 8) & 0xff) * 587 + (rgb & 0xff) * 114) / 1000;
}

// ---------------------------------------------------------------------------
// Static composition
// ---------------------------------------------------------------------------

// Backdrop: the launcher's -- the main-menu space scene scaled to cover the
// screen (centre crop, like ImageView.CENTER_CROP) at 50 % over pal::base. With
// no pixels (no game files, mod without the file) a dark vertical gradient in the
// same tone, so the screen never depends on game art.
inline void draw_backdrop(canvas& c, const style_t& st) {
	const int bw = st.backdrop_w, bh = st.backdrop_h;
	const bool have = st.backdrop && bw > 0 && bh > 0 && st.backdrop->size() >= (size_t)bw * bh;
	const int base_r = (pal::base >> 16) & 0xff, base_g = (pal::base >> 8) & 0xff, base_b = pal::base & 0xff;
	if (!have) {
		for (int y = 0; y < c.h; ++y) {
			float t = (float)y / (float)(c.h > 1 ? c.h - 1 : 1);
			float br = 11 + (base_r - 11) * t, bg = 17 + (base_g - 17) * t, bb = 20 + (base_b - 20) * t;
			uint32_t* row = c.data + (size_t)y * c.pitch;
			for (int x = 0; x < c.w; ++x) {
				// 2x2 ordered dither hides the 8-bit banding of such a shallow gradient.
				float d = (float)(((x & 1) * 2 + (y & 1) * 3) & 3) * 0.25f - 0.375f;
				int r = (int)(br + d + 0.5f), g = (int)(bg + d + 0.5f), b = (int)(bb + d + 0.5f);
				if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
				row[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
			}
		}
		return;
	}
	const uint32_t* src = st.backdrop->data();
	const float sc = std::max((float)c.w / bw, (float)c.h / bh);   // cover
	const float inv = 1.0f / sc;
	const float ox = bw * 0.5f - c.w * 0.5f * inv, oy = bh * 0.5f - c.h * 0.5f * inv;
	const int a = pal::backdrop_alpha, ia = 255 - a;
	const int bases[3] = {base_b, base_g, base_r};
	std::vector<int> u0s((size_t)c.w), ufs((size_t)c.w);
	for (int x = 0; x < c.w; ++x) {
		float u = (x + 0.5f) * inv + ox - 0.5f;
		if (u < 0) u = 0; if (u > bw - 1) u = (float)(bw - 1);
		int u0 = (int)u; if (u0 > bw - 2) u0 = bw > 1 ? bw - 2 : 0;
		u0s[(size_t)x] = u0; ufs[(size_t)x] = (int)((u - u0) * 256.0f);
	}
	const int xstep = bw > 1 ? 1 : 0;
	for (int y = 0; y < c.h; ++y) {
		float v = (y + 0.5f) * inv + oy - 0.5f;
		if (v < 0) v = 0; if (v > bh - 1) v = (float)(bh - 1);
		int v0 = (int)v; if (v0 > bh - 2) v0 = bh > 1 ? bh - 2 : 0;
		const int vf = (int)((v - v0) * 256.0f);
		const uint32_t* r0 = src + (size_t)v0 * bw;
		const uint32_t* r1 = r0 + (bh > 1 ? bw : 0);
		uint32_t* row = c.data + (size_t)y * c.pitch;
		for (int x = 0; x < c.w; ++x) {
			const int u0 = u0s[(size_t)x], uf = ufs[(size_t)x];
			const uint32_t p00 = r0[u0], p01 = r0[u0 + xstep], p10 = r1[u0], p11 = r1[u0 + xstep];
			uint32_t out = 0xff000000u;
			for (int k = 0; k < 3; ++k) {
				const int sh = k * 8;
				int top = (int)((p00 >> sh) & 0xff) * (256 - uf) + (int)((p01 >> sh) & 0xff) * uf;
				int bot = (int)((p10 >> sh) & 0xff) * (256 - uf) + (int)((p11 >> sh) & 0xff) * uf;
				int px = (top * (256 - vf) + bot * vf) >> 16;
				out |= (uint32_t)((px * a + bases[k] * ia) / 255) << sh;
			}
			row[x] = out;
		}
	}
}

// The launcher's panel: black at 86 % over the backdrop, a dark red hairline, a
// black line inside it and a faint ring outside; `tab` adds the small steel
// wedge on the top-left corner the launcher's main panels carry. `selected`
// tints it green and swaps the hairline for the label green (a selected row).
inline void draw_panel(canvas& c, int x0, int y0, int x1, int y1, bool tab = false, bool selected = false) {
	if (x1 <= x0 || y1 <= y0) return;
	const int u = std::max(1, c.h / 400);
	c.frame(x0 - u, y0 - u, x1 + u, y1 + u, u, pal::rim, 255);
	c.fill(x0, y0, x1, y1, pal::panel, pal::panel_alpha);
	if (selected) c.fill(x0, y0, x1, y1, pal::label, pal::sel_tint);
	c.frame(x0, y0, x1, y1, u, selected ? pal::label : pal::line, 255);
	c.frame(x0 + u, y0 + u, x1 - u, y1 - u, u, 0x000000, selected ? 110 : 255);
	if (tab) {
		// 2.6em x 0.5em in the launcher; its right edge slants in towards the bottom.
		int tw = std::max(12, c.h / 22), th = std::max(3, c.h / 115);
		if (tw > (x1 - x0) / 2) tw = (x1 - x0) / 2;
		for (int y = 0; y < th; ++y) {
			int w = tw - (tw * 14 / 100) * y / (th > 1 ? th - 1 : 1);
			c.fill(x0 - u, y0 - u + y, x0 - u + w, y0 - u + y + 1, mix(pal::steel_top, pal::steel_bot, (float)y / (float)(th > 1 ? th - 1 : 1)), 255);
		}
	}
}

// Vertical two-colour gradient fill.
inline void fill_vgrad(canvas& c, int x0, int y0, int x1, int y1, uint32_t top, uint32_t bot) {
	for (int y = y0; y < y1; ++y)
		c.fill(x0, y, x1, y + 1, mix(top, bot, (float)(y - y0) / (float)(y1 - y0 > 1 ? y1 - y0 - 1 : 1)), 255);
}

// A hairline in the panel red (separators inside a panel, the header's flanks).
inline void draw_hairline(canvas& c, int x0, int x1, int y, int t, uint32_t rgb = pal::line) {
	if (t < 1) t = 1;
	c.fill(x0, y, x1, y + t, rgb, 255);
}

inline void draw_minimap(canvas& c, const info_t& info, int x0, int y0, int dw, int dh, const style_t& st) {
	const int u = std::max(1, c.h / 400);            // line unit
	const int pad = std::max(3, c.h / 110);          // plate margin around the map
	(void)st;
	// The map sits in a launcher panel (hairline + steel corner tab), like the
	// launcher's map preview.
	draw_panel(c, x0 - pad, y0 - pad, x0 + dw + pad, y0 + dh + pad, true);
	const int mw = info.minimap_w, mh = info.minimap_h;
	if (mw > 0 && mh > 0 && (int)info.minimap.size() >= mw * mh) {
		// Bilinear upscale (one source pixel per map tile).
		for (int y = 0; y < dh; ++y) {
			int py = y0 + y;
			if (py < 0 || py >= c.h) continue;
			float v = (y + 0.5f) * mh / dh - 0.5f;
			int v0 = (int)std::floor(v); float vf = v - v0;
			int v1 = v0 + 1;
			if (v0 < 0) v0 = 0; if (v1 < 0) v1 = 0; if (v0 >= mh) v0 = mh - 1; if (v1 >= mh) v1 = mh - 1;
			const uint32_t* r0 = info.minimap.data() + (size_t)v0 * mw;
			const uint32_t* r1 = info.minimap.data() + (size_t)v1 * mw;
			uint32_t* row = c.data + (size_t)py * c.pitch;
			for (int x = 0; x < dw; ++x) {
				int px = x0 + x;
				if (px < 0 || px >= c.w) continue;
				float uu = (x + 0.5f) * mw / dw - 0.5f;
				int u0 = (int)std::floor(uu); float uf = uu - u0;
				int u1 = u0 + 1;
				if (u0 < 0) u0 = 0; if (u1 < 0) u1 = 0; if (u0 >= mw) u0 = mw - 1; if (u1 >= mw) u1 = mw - 1;
				uint32_t a = r0[u0], b = r0[u1], cc = r1[u0], d = r1[u1];
				uint32_t out = 0xff000000u;
				for (int sh = 0; sh <= 16; sh += 8) {
					float top = ((a >> sh) & 0xff) * (1 - uf) + ((b >> sh) & 0xff) * uf;
					float bot = ((cc >> sh) & 0xff) * (1 - uf) + ((d >> sh) & 0xff) * uf;
					out |= (uint32_t)(top * (1 - vf) + bot * vf + 0.5f) << sh;
				}
				row[px] = out;
			}
		}
	} else {
		// No minimap pixels: an empty plate with a faint grid, so the layout holds.
		for (int k = 1; k < 4; ++k) {
			c.fill(x0 + dw * k / 4, y0, x0 + dw * k / 4 + u, y0 + dh, pal::track, 255);
			c.fill(x0, y0 + dh * k / 4, x0 + dw, y0 + dh * k / 4 + u, pal::track, 255);
		}
	}
	// Start locations: unused ones as a small hollow ring, the players' as a
	// colour dot with a white rim.
	float r = std::max(4.0f, c.h / 105.0f);
	for (auto& m : info.markers) {
		if (m.active) continue;
		float mx = x0 + m.x * dw, my = y0 + m.y * dh;
		c.disc(mx, my, r * 0.75f, 0x000000, 150);
		c.disc(mx, my, r * 0.55f, 0xa8b4c0, 230);
		c.disc(mx, my, r * 0.30f, 0x10161e, 255);
	}
	for (auto& m : info.markers) {
		if (!m.active) continue;
		float mx = x0 + m.x * dw, my = y0 + m.y * dh;
		c.disc(mx, my, r + u * 2.5f, 0x000000, 170);
		c.disc(mx, my, r + u * 1.3f, 0xffffff, 255);
		c.disc(mx, my, r, m.color, 255);
	}
}

inline std::string race_text(const player_t& p, const style_t& st) {
	if (p.race < 0 || p.race > 2) return st.s_random;
	if (p.race_random) return st.s_race[p.race] + " (" + st.s_random + ")";
	return st.s_race[p.race];
}

// Portrait tile: the player's unit portrait (a still frame) in a frame of the
// player's colour, the race letter on a tag in a bottom corner. Shared by the
// loading screen's player rows and the replay player's cards (ui.h). The clips
// are 60x56 pixel art: whole multiples are drawn texel-exact (with the
// launcher's scanlines), half steps (1.5x, 2.5x -- small cards / low render
// scale) and anything below 1x are filtered so the texels stay even. With no
// pixels it is a "no signal" static tile, so a portrait can never leak a race.
struct portrait_tile_t {
	int dw = 0, dh = 0;   // picture size
	int scale = 0;        // whole-texel scale, 0 = filtered
	int ft = 0;           // frame thickness
	int tw() const { return dw + ft * 2; }
	int th() const { return dh + ft * 2; }
};
// The largest tile whose picture is at most `avail` pixels high.
inline portrait_tile_t portrait_tile_fit(const player_t& p, int avail, int ft) {
	const int pw = p.portrait_w > 0 ? p.portrait_w : 60, ph = p.portrait_h > 0 ? p.portrait_h : 56;
	portrait_tile_t g;
	g.ft = ft;
	int half_steps = avail * 2 / ph;                   // scale in halves
	if (half_steps >= 2) { g.dw = pw * half_steps / 2; g.dh = ph * half_steps / 2; }
	else { g.dh = avail > 4 ? avail : 4; g.dw = g.dh * pw / ph; }
	g.scale = (half_steps >= 2 && half_steps % 2 == 0) ? half_steps / 2 : 0;
	return g;
}
// (tx0, ty0) = the tile's top-left corner; `tag_left` puts the race tag in the
// bottom-left corner instead of the bottom-right.
inline void draw_portrait_tile(canvas& c, const player_t& p, const portrait_tile_t& g, int tx0, int ty0, bool tag_left) {
	const int u = std::max(1, c.h / 400);
	const char* letter = p.race == 0 ? "Z" : p.race == 1 ? "T" : p.race == 2 ? "P" : "?";
	const int pw = p.portrait_w > 0 ? p.portrait_w : 60, ph = p.portrait_h > 0 ? p.portrait_h : 56;
	const int ft = g.ft, dw = g.dw, dh = g.dh, scale = g.scale;
	const int tw = g.tw(), th = g.th();
	c.fill(tx0, ty0, tx0 + tw, ty0 + th, p.color, 255);
	const bool have = p.portrait && (int)p.portrait->size() >= pw * ph && p.portrait_w > 0;
	auto texel = [&](int sx, int sy) -> uint32_t {
		if (sx < 0) sx = 0; if (sy < 0) sy = 0; if (sx >= pw) sx = pw - 1; if (sy >= ph) sy = ph - 1;
		if (have) return (*p.portrait)[(size_t)sy * pw + sx];
		// "no signal": 2x2-texel static, seeded by position only (stable frame to frame)
		uint32_t hsh = (uint32_t)(sx / 2) * 374761393u + (uint32_t)(sy / 2) * 668265263u;
		hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
		uint32_t gr = 24 + ((hsh >> 16) % 72);
		return (gr << 16) | (gr << 8) | gr;
	};
	for (int y = 0; y < dh; ++y) {
		for (int x = 0; x < dw; ++x) {
			uint32_t px;
			if (scale) px = texel(x / scale, y / scale);
			else {
				float fu = (x + 0.5f) * pw / dw - 0.5f, fv = (y + 0.5f) * ph / dh - 0.5f;
				int u0 = (int)std::floor(fu), v0 = (int)std::floor(fv);
				float uf = fu - u0, vf = fv - v0;
				uint32_t a = texel(u0, v0), b = texel(u0 + 1, v0), cc = texel(u0, v0 + 1), d = texel(u0 + 1, v0 + 1);
				px = 0;
				for (int sh = 0; sh <= 16; sh += 8) {
					float top = ((a >> sh) & 0xff) * (1 - uf) + ((b >> sh) & 0xff) * uf;
					float bot = ((cc >> sh) & 0xff) * (1 - uf) + ((d >> sh) & 0xff) * uf;
					px |= (uint32_t)(top * (1 - vf) + bot * vf + 0.5f) << sh;
				}
			}
			c.blend(tx0 + ft + x, ty0 + ft + y, px, 255);
		}
		// scanline on the last row of every source texel (as the launcher draws them)
		if (scale >= 2 && (y % scale) == scale - 1) c.fill(tx0 + ft, ty0 + ft + y, tx0 + ft + dw, ty0 + ft + y + 1, 0x000000, 56);
	}
	c.frame(tx0, ty0, tx0 + tw, ty0 + th, u, 0x000000, 70);
	// Race tag in the tile's outer bottom corner.
	int ts = th * 30 / 100; if (ts < 9) ts = 9;
	int gx0 = tag_left ? tx0 : tx0 + tw - ts, gy0 = ty0 + th - ts;
	c.fill(gx0, gy0, gx0 + ts, gy0 + ts, p.color, 255);
	c.frame(gx0, gy0, gx0 + ts, gy0 + ts, u, 0x000000, 120);
	float lh = ts * 0.66f;
	uint32_t lc = luma(p.color) > 150 ? 0x101418 : 0xffffff;
	float lw = text_width(letter, lh);
	draw_text(c, gx0 + (ts - lw) / 2, gy0 + (ts - lh) / 2, letter, lc, lh, 0, 240, luma(p.color) <= 150);
}

// One player row. `mirror` = right-hand column (badge on the right, text
// right-aligned), so the two sides face each other across the minimap.
inline void draw_player_card(canvas& c, const player_t& p, int x0, int y0, int w, int h, bool mirror, const style_t& st) {
	const int u = std::max(1, c.h / 400);
	// The launcher's player row: a panel with the red hairline; the local player's
	// is the "selected" one (green tint + green edge).
	draw_panel(c, x0, y0, x0 + w, y0 + h, false, p.is_local);
	int bs = h - u * 2;
	int bx0 = mirror ? x0 + w - u - bs : x0 + u, by0 = y0 + u;
	const char* letter = p.race == 0 ? "Z" : p.race == 1 ? "T" : p.race == 2 ? "P" : "?";
	if (p.portrait_tile) {
		// Portrait tile, framed in the player colour; it takes the badge's place.
		const int ft = u * 2;                              // frame thickness
		const portrait_tile_t g = portrait_tile_fit(p, h - u * 2 - ft * 2, ft);
		const int tw = g.tw(), th = g.th();
		int tx0 = mirror ? x0 + w - u - tw : x0 + u, ty0 = y0 + (h - th) / 2;
		draw_portrait_tile(c, p, g, tx0, ty0, mirror);
		// the text block below is laid out against the tile instead of the badge
		bs = tw; bx0 = tx0;
	} else {
		// Colour badge with the race initial.
		uint32_t top = mix(p.color, 0xffffff, 0.10f), bot = mix(p.color, 0x000000, 0.28f);
		for (int y = 0; y < bs; ++y) {
			uint32_t col = mix(top, bot, (float)y / (float)(bs > 1 ? bs - 1 : 1));
			c.fill(bx0, by0 + y, bx0 + bs, by0 + y + 1, col, 255);
		}
		c.frame(bx0, by0, bx0 + bs, by0 + bs, u, 0x000000, 70);
		float lh = bs * 0.62f;
		uint32_t lc = luma(p.color) > 150 ? 0x101418 : 0xffffff;
		float lw = text_width(letter, lh);
		draw_text(c, bx0 + (bs - lw) / 2, by0 + (bs - lh) / 2, letter, lc, lh, 0, 235, luma(p.color) <= 150);
	}
	// Text block.
	float padx = h * 0.20f;
	float tx0 = mirror ? x0 + padx : bx0 + bs + padx;
	float tx1 = mirror ? bx0 - padx : x0 + w - padx;
	float tw = tx1 - tx0;
	if (tw < 8) return;
	float nh = std::min(h * 0.37f, c.h * 0.043f);
	float sh2 = std::min(h * 0.25f, c.h * 0.028f);
	float gap = h * 0.10f;
	float block = nh + gap + sh2;
	float ny = y0 + (h - block) / 2;
	std::string name = p.name;
	float fh = fit_height(name, nh, nh * 0.72f, tw);
	name = ellipsize(name, fh, tw);
	float nw = text_width(name, fh);
	draw_text(c, mirror ? tx1 - nw : tx0, ny + (nh - fh) / 2, name, pal::title, fh);
	// Second line: race, then the role in a dimmer tone.
	std::string race = race_text(p, st);
	std::string role = p.role;
	std::string sep = role.empty() ? "" : " - ";
	float sy = ny + nh + gap;
	std::string full = race + sep + role;
	if (text_width(full, sh2) > tw) {            // too narrow: drop the role first
		role.clear(); sep.clear();
		race = ellipsize(race, sh2, tw);
	}
	float fw = text_width(race + sep + role, sh2);
	float sx = mirror ? tx1 - fw : tx0;
	draw_text(c, sx, sy, race, pal::text, sh2);
	if (!role.empty()) draw_text(c, sx + text_advance(sh2) * race.size(), sy, sep + role, pal::text2, sh2);
}

// Decide which players stand on which side.
//  - exactly two teams: one team per side ("VS");
//  - two players: one each ("VS");
//  - anything else (FFA, 3+ teams): kept in team order and split evenly, at a
//    team boundary when one is close to the middle.
struct sides_t { std::vector<int> left, right; bool versus = false; bool ffa = false; bool team_labels = false; };
inline sides_t split_sides(const info_t& info) {
	sides_t s;
	const int n = (int)info.players.size();
	if (n == 0) return s;
	std::vector<int> teams; // distinct, in order of first appearance
	for (auto& p : info.players) if (std::find(teams.begin(), teams.end(), p.team) == teams.end()) teams.push_back(p.team);
	if (teams.size() == 2) {
		for (int i = 0; i < n; ++i) (info.players[i].team == teams[0] ? s.left : s.right).push_back(i);
		s.versus = true;
		s.team_labels = s.left.size() > 1 || s.right.size() > 1;
		return s;
	}
	if (n == 1) { s.left.push_back(0); return s; }
	if (n == 2) { s.left.push_back(0); s.right.push_back(1); s.versus = teams.size() != 1; return s; }
	s.ffa = (int)teams.size() == n;      // every player alone; 3+ real teams get no header word
	// Order by team (stable), then split.
	std::vector<int> order;
	for (int t : teams) for (int i = 0; i < n; ++i) if (info.players[i].team == t) order.push_back(i);
	int half = (n + 1) / 2, cut = half;
	if ((int)teams.size() < n) {
		// Prefer the team boundary nearest the middle, if it keeps both sides <= half+1.
		int best = -1, best_d = 1 << 30;
		for (int i = 1; i < n; ++i) {
			if (info.players[order[i]].team == info.players[order[i - 1]].team) continue;
			int d = std::abs(i - half);
			if (d < best_d) { best_d = d; best = i; }
		}
		if (best > 0 && best_d <= 1) cut = best;
	}
	for (int i = 0; i < n; ++i) (i < cut ? s.left : s.right).push_back(order[i]);
	return s;
}

inline void draw_melee(canvas& c, const info_t& info, const style_t& st) {
	const int sw = c.w, sh = c.h;
	const int u = std::max(1, sh / 400);
	// --- minimap slot: centred, aspect-correct, at most half the height -----
	const float slot_y0 = sh * 0.185f, slot_h = sh * 0.50f, slot_w = sw * 0.36f;
	int mw = info.minimap_w > 0 ? info.minimap_w : 1, mh = info.minimap_h > 0 ? info.minimap_h : 1;
	float sc = std::min(slot_w / mw, slot_h / mh);
	int dw = std::max(8, (int)(mw * sc)), dh = std::max(8, (int)(mh * sc));
	int mx0 = (sw - dw) / 2, my0 = (int)(slot_y0 + (slot_h - dh) / 2);
	draw_minimap(c, info, mx0, my0, dw, dh, st);

	// --- map name + detail under the minimap -------------------------------
	const int pad = std::max(3, sh / 110);
	float name_h = sh * 0.050f, det_h = sh * 0.027f;
	float name_y = my0 + dh + pad + sh * 0.026f;
	if (!info.map_name.empty()) {
		float max_w = sw * 0.46f;
		float fh = fit_height(info.map_name, name_h, name_h * 0.6f, max_w);
		std::string nm = ellipsize(info.map_name, fh, max_w);
		draw_text_centered(c, sw * 0.5f, name_y + (name_h - fh) / 2, nm, pal::title, fh);
	}
	if (!info.map_detail.empty()) {
		std::string d = ellipsize(upper(info.map_detail), det_h, sw * 0.46f, 0.08f);
		draw_text_centered(c, sw * 0.5f, name_y + name_h + sh * 0.014f, d, pal::text2, det_h, 0.08f);
	}

	// --- the two sides ------------------------------------------------------
	sides_t sides = split_sides(info);
	// Header above the minimap: "VS" (two sides) or "FREE FOR ALL".
	{
		std::string hd = sides.versus ? st.s_vs : (sides.ffa ? st.s_ffa : std::string());
		if (!hd.empty()) {
			// "VS" is the one big word on the screen: the yellow accent. "FREE FOR ALL"
			// is a section label: green, upper-case, letter-spaced.
			float hh = sides.versus ? sh * 0.075f : sh * 0.040f;
			float track = sides.versus ? 0.12f : 0.16f;
			float hw = text_width(hd, hh, track);
			float cy = sh * 0.095f;
			draw_text(c, sw * 0.5f - hw / 2, cy - hh / 2, hd, sides.versus ? pal::accent : pal::label, hh, track);
			// Short flanking hairlines in the panel red, fading away from the word.
			float gap = hh * 0.55f, len = sw * 0.10f;
			for (int side = -1; side <= 1; side += 2) {
				float xa = sw * 0.5f + side * (hw / 2 + gap);
				for (int k = 0; k < (int)len; ++k) {
					int a = (int)(255 * (1.0f - (float)k / len));
					for (int t = 0; t < u * 2; ++t) c.blend((int)(xa + side * k), (int)cy - u + t, mix(pal::line, pal::danger, 0.25f), a);
				}
			}
		}
	}
	if (!sides.left.empty() || !sides.right.empty()) {
		const float avail = sh * 0.52f;
		const int margin = (int)(sw * 0.035f), mgap = (int)(sw * 0.028f) + pad;
		const int col_w = mx0 - mgap - margin;           // space between the screen edge and the map
		const float centre_y = slot_y0 + slot_h / 2;
		for (int side = 0; side < 2; ++side) {
			const std::vector<int>& list = side == 0 ? sides.left : sides.right;
			if (list.empty()) continue;
			// Each side sizes its own rows, so a lone player facing seven keeps a full-size card.
			const int rows = (int)list.size();
			float gap = sh * (rows > 4 ? 0.012f : 0.018f);
			float ch = std::min(sh * 0.115f, (avail - (rows - 1) * gap) / rows);
			int card_w = std::min(col_w, (int)(sw * 0.31f));
			if (card_w < (int)(ch * 2)) card_w = (int)(ch * 2);
			float total = rows * ch + (rows - 1) * gap;
			float y = centre_y - total / 2;
			// Cards hug the minimap, so the two sides read as facing each other.
			int x0 = side == 0 ? mx0 - mgap - card_w : mx0 + dw + mgap;
			if (sides.team_labels) {
				float lh = sh * 0.028f;
				std::string lbl = upper(st.s_team) + " " + std::to_string(side + 1);
				float lw = text_width(lbl, lh, 0.12f);
				draw_text(c, side == 0 ? (float)x0 : x0 + card_w - lw, y - lh - sh * 0.014f, lbl, pal::label, lh, 0.12f);
			}
			for (int i = 0; i < rows; ++i)
				draw_player_card(c, info.players[list[i]], x0, (int)(y + i * (ch + gap)), card_w, (int)ch, side == 1, st);
		}
	}
}

// Campaign: the mission card -- a centred launcher panel with the episode as a
// green section label, the mission title in white, a hairline, and the
// campaign / mission number in the yellow accent (the launcher's mission detail
// panel uses the same three levels).
inline void draw_campaign(canvas& c, const info_t& info, const style_t& st) {
	(void)st;
	const int sw = c.w, sh = c.h;
	const int u = std::max(1, sh / 400);
	const float cx = sw * 0.5f;
	const float padx = sw * 0.05f;
	const float max_w = sw * 0.88f - 2 * padx;       // text width inside the panel
	// Title: as large as fits one line; a long one wraps onto two lines at the
	// space nearest its middle rather than shrinking into small print.
	std::string title = info.title.empty() ? std::string("Mission") : info.title;
	float th = sh * 0.115f, min_one_line = sh * 0.078f;
	std::vector<std::string> lines{title};
	if (text_width(title, min_one_line, 0.02f) > max_w) {
		size_t best = std::string::npos; size_t mid = title.size() / 2;
		for (size_t i = 0; i < title.size(); ++i) {
			if (title[i] != ' ') continue;
			if (best == std::string::npos || (i > mid ? i - mid : mid - i) < (best > mid ? best - mid : mid - best)) best = i;
		}
		if (best != std::string::npos) lines = {title.substr(0, best), title.substr(best + 1)};
	}
	float fh = th;
	for (auto& l : lines) fh = std::min(fh, fit_height(l, th, sh * 0.03f, max_w, 0.02f));
	if (lines.size() == 2 && fh > sh * 0.095f) fh = sh * 0.095f;
	for (auto& l : lines) l = ellipsize(l, fh, max_w, 0.02f);
	float line_gap = fh * 0.18f;
	float block_h = lines.size() * fh + (lines.size() - 1) * line_gap;
	float widest = 0;
	for (auto& l : lines) widest = std::max(widest, text_width(l, fh, 0.02f));
	// Overline (episode) and underline (campaign - mission n), fitted to the panel.
	std::string over = info.overline, under = info.underline;
	float ofh = 0, ufh = 0;
	if (!over.empty()) {
		ofh = fit_height(over, sh * 0.030f, sh * 0.018f, max_w, 0.16f);
		over = ellipsize(over, ofh, max_w, 0.16f);
		widest = std::max(widest, text_width(over, ofh, 0.16f));
	}
	if (!under.empty()) {
		ufh = fit_height(under, sh * 0.030f, sh * 0.018f, max_w, 0.14f);
		under = ellipsize(under, ufh, max_w, 0.14f);
		widest = std::max(widest, text_width(under, ufh, 0.14f));
	}
	const float pad_y = sh * 0.055f, gap_o = sh * 0.036f, gap_r = sh * 0.042f, gap_u = sh * 0.032f;
	float total = pad_y + (over.empty() ? 0 : ofh + gap_o) + block_h + (under.empty() ? 0 : gap_r + u + gap_u + ufh) + pad_y;
	float pw = std::max(sw * 0.50f, std::min(sw * 0.88f, widest + 2 * padx));
	int px0 = (int)(cx - pw / 2), px1 = (int)(cx + pw / 2);
	int py0 = (int)(sh * 0.44f - total / 2), py1 = (int)(py0 + total);
	draw_panel(c, px0, py0, px1, py1, true);
	float y = py0 + pad_y;
	if (!over.empty()) {
		draw_text_centered(c, cx, y, over, pal::label, ofh, 0.16f);
		y += ofh + gap_o;
	}
	for (size_t i = 0; i < lines.size(); ++i)
		draw_text_centered(c, cx, y + i * (fh + line_gap), lines[i], pal::title, fh, 0.02f);
	y += block_h;
	if (!under.empty()) {
		y += gap_r;
		draw_hairline(c, (int)(px0 + padx), (int)(px1 - padx), (int)y, u);
		y += u + gap_u;
		draw_text_centered(c, cx, y, under, pal::accent, ufh, 0.14f);
	}
}

inline uint64_t signature(const info_t& info, const style_t& st, int w, int h) {
	uint64_t x = 1469598103934665603ull;
	auto mixb = [&](const void* p, size_t n) {
		const unsigned char* b = (const unsigned char*)p;
		for (size_t i = 0; i != n; ++i) { x ^= b[i]; x *= 1099511628211ull; }
	};
	auto mixs = [&](const std::string& s) { mixb(s.data(), s.size()); unsigned char z = 0xff; mixb(&z, 1); };
	auto mixi = [&](int64_t v) { mixb(&v, sizeof(v)); };
	mixi(w); mixi(h); mixi((int)info.kind);
	mixs(info.map_name); mixs(info.map_detail); mixs(info.title); mixs(info.overline); mixs(info.underline);
	mixi(info.minimap_w); mixi(info.minimap_h); mixi((int64_t)info.minimap.size()); mixi(info.minimap_rev);
	for (auto& m : info.markers) { mixb(&m.x, sizeof(m.x)); mixb(&m.y, sizeof(m.y)); mixi(m.color); mixi(m.active); }
	for (auto& p : info.players) {
		mixs(p.name); mixs(p.role); mixi(p.race); mixi(p.race_random); mixi(p.color); mixi(p.team); mixi(p.is_local);
		mixi(p.portrait_tile); mixi(p.portrait_id); mixi(p.portrait ? (int64_t)p.portrait->size() : -1);
	}
	// Backdrop: its size plus a sparse sample of the pixels (the whole image is
	// 300k pixels; 64 samples tell one picture from another well enough for a
	// cache key, and a backdrop arriving late changes the size from -1).
	mixi(st.backdrop_w); mixi(st.backdrop_h); mixi(st.backdrop ? (int64_t)st.backdrop->size() : -1);
	if (st.backdrop && !st.backdrop->empty()) {
		const size_t n = st.backdrop->size();
		for (size_t k = 0; k != 64; ++k) mixi((*st.backdrop)[(k * 2654435761u + 12345u) % n]);
	}
	mixs(st.s_vs); mixs(st.s_ffa); mixs(st.s_team); mixs(st.s_random);
	for (auto& r : st.s_race) mixs(r);
	return x ? x : 1;
}

inline void draw_static(canvas& c, const info_t& info, const style_t& st) {
	draw_backdrop(c, st);
	if (info.kind == info_t::melee) draw_melee(c, info, st);
	else if (info.kind == info_t::campaign) draw_campaign(c, info, st);
}

// A launcher button: `kind` 0 secondary (steel), 1 primary (green gradient,
// green edge, white label), 2 danger (steel body, red edge and label), 3 accent
// (steel body, yellow edge and label). Shared with the briefing's buttons in ui.h.
inline void draw_button(canvas& c, int x0, int y0, int x1, int y1, const std::string& label, int kind, float max_text_h = 0) {
	if (x1 <= x0 || y1 <= y0) return;
	const int u = std::max(1, c.h / 400);
	const int bw = x1 - x0, bh = y1 - y0;
	if (kind == 1) fill_vgrad(c, x0, y0, x1, y1, pal::sel_top, pal::sel_bot);
	else fill_vgrad(c, x0, y0, x1, y1, pal::btn_top, pal::btn_bot);
	c.frame(x0, y0, x1, y1, u, kind == 1 ? pal::label : kind == 2 ? pal::danger : kind == 3 ? pal::accent : pal::steel, 255);
	float lh = bh * 0.40f;
	if (max_text_h > 0 && lh > max_text_h) lh = max_text_h;
	const float track = 0.08f;
	lh = fit_height(label, lh, bh * 0.18f, bw * 0.86f, track);
	std::string s = ellipsize(label, lh, bw * 0.92f, track);
	draw_text_centered(c, x0 + bw * 0.5f, y0 + (bh - lh) / 2, s,
		kind == 1 ? pal::title : kind == 2 ? mix(pal::danger, 0xffffff, 0.30f) : kind == 3 ? pal::accent : pal::text, lh, track);
}

// ---------------------------------------------------------------------------
// Per-frame part: progress bar (+ shimmer), status line, cancel button.
// ---------------------------------------------------------------------------

inline void draw_dynamic(canvas& c, float progress, const style_t& st, rect_t* cancel_out) {
	const int sw = c.w, sh = c.h;
	const int u = std::max(1, sh / 400);
	float p = progress; if (p < 0) p = 0; if (p > 1) p = 1;
	// One bar along the bottom, full width less ~3.5% side margins, the same
	// distance up from the bottom edge.
	int mx = sw * 35 / 1000; if (mx < 10) mx = 10;
	int bh = sh / 44; if (bh < 8) bh = 8;
	int by = sh - bh - sw * 35 / 1000;
	int bx = mx, bw = sw - 2 * mx;
	if (st.cancel) {
		// The abort button sits at the right end of the bar's row; the bar stops short of it.
		int cbw = sw / 8, cbh = sh / 14;
		int cbx = sw - mx - cbw, cby = by + bh / 2 - cbh / 2;
		if (cby + cbh > sh - 4) cby = sh - 4 - cbh;
		bw -= cbw + sw / 50;
		draw_button(c, cbx, cby, cbx + cbw, cby + cbh, st.cancel_label, 2);
		if (cancel_out) { cancel_out->x0 = cbx; cancel_out->y0 = cby; cancel_out->x1 = cbx + cbw; cancel_out->y1 = cby + cbh; }
	} else if (cancel_out) {
		*cancel_out = rect_t();
	}
	if (!st.status.empty()) {
		float h = sh * 0.028f;
		std::string s = ellipsize(st.status, h, (float)bw);
		draw_text_centered(c, bx + bw * 0.5f, by - h - sh * 0.022f, s, pal::text, h);
	}
	// Track (the launcher's bar: a flat dark track, a green fill) with a thin
	// black surround so it holds against a bright patch of the backdrop.
	c.fill(bx - u, by - u, bx + bw + u, by + bh + u, 0x000000, 255);
	c.fill(bx, by, bx + bw, by + bh, pal::track, 255);
	int fillw = (int)(bw * p);
	if (fillw > 0) {
		fill_vgrad(c, bx, by, bx + fillw, by + bh, pal::label, pal::sel_top);
		c.fill(bx, by, bx + fillw, by + u, mix(pal::label, 0xffffff, 0.45f), 255);   // top glint
	}
	// Shimmer sweeping across the filled part, so it reads as "working" between
	// the discrete progress steps.
	if (fillw > 8) {
		int shw = bw / 8; if (shw < 12) shw = 12;
		int sx = bx + (int)((st.anim_ms / 4) % (int64_t)(fillw + shw)) - shw;
		for (int x = sx; x < sx + shw; ++x) {
			if (x < bx || x >= bx + fillw) continue;
			int d = x - (sx + shw / 2); if (d < 0) d = -d;
			int glow = (shw / 2 - d) * 80 / (shw / 2 > 0 ? shw / 2 : 1);
			if (glow <= 0) continue;
			for (int y = by; y < by + bh; ++y) c.blend(x, y, 0xffffff, glow);
		}
	}
	// Leading edge in the yellow accent: where the bar is right now.
	if (fillw > u * 2 && fillw < bw) c.fill(bx + fillw - u * 2, by, bx + fillw, by + bh, pal::accent, 255);
}

// Entry point. `cache` may be null (everything is redrawn each call).
inline void draw(uint32_t* data, size_t pitch, int w, int h, const info_t& info, float progress,
		const style_t& st, cache_t* cache = nullptr, rect_t* cancel_out = nullptr) {
	if (!data || w <= 0 || h <= 0) return;
	canvas c; c.data = data; c.pitch = pitch; c.w = w; c.h = h;
	if (cache) {
		uint64_t sig = signature(info, st, w, h);
		if (cache->sig != sig || cache->w != w || cache->h != h || cache->px.size() != (size_t)w * h) {
			cache->px.assign((size_t)w * h, 0);
			canvas cc; cc.data = cache->px.data(); cc.pitch = (size_t)w; cc.w = w; cc.h = h;
			draw_static(cc, info, st);
			cache->w = w; cache->h = h; cache->sig = sig;
		}
		for (int y = 0; y < h; ++y)
			memcpy(data + (size_t)y * pitch, cache->px.data() + (size_t)y * w, (size_t)w * 4);
	} else {
		draw_static(c, info, st);
	}
	draw_dynamic(c, progress, st, cancel_out);
}

} // namespace loading_screen
} // namespace bwgame

#endif
