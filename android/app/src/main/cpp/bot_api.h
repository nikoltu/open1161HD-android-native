// OpenBW Android downloadable-bot ABI (v1).
//
// A bot is a shared library placed in the app's INTERNAL storage (external
// storage is noexec) and loaded with dlopen. The host passes a C function
// table; the bot issues commands through it. Everything crosses the .so
// boundary as plain C, so bots can be built with any NDK version.
//
// Exports a bot must provide:
//   int   openbw_bot_abi(void);                                  // return OPENBW_BOT_ABI_VERSION
//   void* openbw_bot_create(ob_bot_host*, const ob_host_api*);   // bot instance
//   void  openbw_bot_frame(void* bot);                           // once per sim frame
//   void  openbw_bot_destroy(void* bot);
//
// Optional export:
//   int   openbw_bot_race(void);   // 0 Zerg, 1 Terran, 2 Protoss; else = any
// A race-specific bot MUST declare its race: the slot's starting units are
// built from the slot race before the bot ever runs, so a Terran bot placed
// on a Protoss slot finds no Command Center and idles the entire game. When
// the export is present the host forces the bot slot to that race; when it is
// absent the launcher's per-slot race is kept.
//
// Unit handles (ob_unit.id) are only valid during the openbw_bot_frame call
// that produced them - re-query every frame.

#ifndef OPENBW_BOT_API_H
#define OPENBW_BOT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// v2 (2026-07-18): appended research/upgrade commands and intel queries.
// Additive-only - v1 bots keep working (the host accepts 1..CURRENT and
// the function table only grows at the end).
// v4 (2026-09-30): explored_tiles -- the bot's own fog-of-war exploration map.
// BW forbids placing a building on an UNEXPLORED tile (can_place_building), and a
// fresh map's expansions are black: without this a placer (Steamhammer's
// BuildingPlacer, UnifiedLite before its scouting fix) keeps proposing tiles the
// host must reject and never expands. Additive as always.
// v5 (2026-10-01): abilities -- use_ability (stim, siege, burrow, cloak ...),
// cast_at / cast_on (targeted spells and Spider Mines). Without them a bot could
// only move, attack and build: tanks never sieged, lurkers (which attack only
// while burrowed) were useless, no storm, no scan. Additive as always.
#define OPENBW_BOT_ABI_VERSION 5

typedef struct ob_bot_host ob_bot_host; // opaque host handle

typedef struct ob_unit {
	intptr_t id;      // opaque handle, valid only within the current frame call
	int32_t type;     // UnitTypes id (BW units.dat index)
	int32_t owner;    // player slot
	int32_t x, y;     // map pixels
	int32_t hp;       // integer hit points
	int32_t shields;  // integer shield points
	int32_t order;    // current Orders id
	int32_t idle;     // 1 when standing idle (guard/nothing)
	int32_t completed; // 1 when fully built/morphed
} ob_unit;

typedef struct ob_host_api {
	uint32_t abi_version; // OPENBW_BOT_ABI_VERSION

	// --- state queries -------------------------------------------------
	int32_t (*current_frame)(ob_bot_host*);
	int32_t (*my_player)(ob_bot_host*);       // the bot's player slot
	int32_t (*minerals)(ob_bot_host*, int32_t player);
	int32_t (*gas)(ob_bot_host*, int32_t player);
	int32_t (*supply_used)(ob_bot_host*, int32_t player);      // in half-supply units
	int32_t (*supply_available)(ob_bot_host*, int32_t player); // in half-supply units
	// Fill out[0..max) with player's units; player 11 = neutral (minerals,
	// geysers). Returns the number written.
	int32_t (*units)(ob_bot_host*, int32_t player, ob_unit* out, int32_t max);
	// Nearest enemy start location; returns 0 if none known.
	int32_t (*enemy_start)(ob_bot_host*, int32_t* x, int32_t* y);
	int32_t (*map_size)(ob_bot_host*, int32_t* width, int32_t* height); // pixels

	// --- commands (issued for my_player, deterministic action path) ----
	void (*order_move)(ob_bot_host*, intptr_t unit, int32_t x, int32_t y);
	void (*order_attack_move)(ob_bot_host*, intptr_t unit, int32_t x, int32_t y);
	// Right-click semantics (gather minerals/gas, follow, attack enemy...).
	void (*order_right_click)(ob_bot_host*, intptr_t unit, intptr_t target_unit);
	void (*order_stop)(ob_bot_host*, intptr_t unit);
	// Train unit_type from a production building. Returns 1 on accept.
	int32_t (*train)(ob_bot_host*, intptr_t trainer, int32_t unit_type);
	// Order a worker to construct unit_type at tile coordinates. Returns 1
	// if the build order was issued (placement may still fail server-side).
	int32_t (*build)(ob_bot_host*, intptr_t worker, int32_t unit_type, int32_t tile_x, int32_t tile_y);

	// --- diagnostics ---------------------------------------------------
	void (*log)(ob_bot_host*, const char* text);

	// --- v2 additions (NULL when the host is older than the bot) -------
	// Research tech_id (techdata.dat index) at a building. 1 on accept.
	int32_t (*research)(ob_bot_host*, intptr_t building, int32_t tech_id);
	// Start upgrade_id (upgrades.dat index) at a building. 1 on accept.
	int32_t (*upgrade)(ob_bot_host*, intptr_t building, int32_t upgrade_id);
	// Current upgrade level / tech-researched flag for a player.
	int32_t (*upgrade_level)(ob_bot_host*, int32_t player, int32_t upgrade_id);
	int32_t (*tech_researched)(ob_bot_host*, int32_t player, int32_t tech_id);
	// The bot's primary enemy slot (-1 if unknown).
	int32_t (*enemy_player)(ob_bot_host*);
	// Map start locations (potential expansions): fills out_xy pairs
	// [x0,y0,x1,y1,...] up to max PAIRS, returns pair count.
	int32_t (*start_locations)(ob_bot_host*, int32_t* out_xy, int32_t max);

	// --- v3 additions (NULL when the host is older than the bot) -------
	// Per-build-tile terrain flags, row-major, width*height bytes (see
	// map_size for pixel dimensions; tiles are 32px). Returns the number of
	// bytes written, or 0 if unavailable. Static terrain only - it does not
	// change with buildings, so a bot can fetch it once at start.
	// Real BWAPI bots (UAlbertaBot's MapTools) build their whole map model
	// and connectivity flood-fill from exactly this.
	int32_t (*map_tiles)(ob_bot_host*, uint8_t* out, int32_t max);
	// Extended state for one unit (see ob_unit_ex). Returns 1 on success.
	// Queried on demand rather than filled for every unit every frame -
	// most units are never asked about energy/targets/queues.
	int32_t (*unit_ex)(ob_bot_host*, intptr_t unit, struct ob_unit_ex* out);

	// --- v4 additions (NULL when the host is older than the bot) -------
	// Per-build-tile exploration for the bot's player, row-major width*height
	// bytes: 1 = explored (a building may be placed there, fog or not), 0 =
	// never seen. DYNAMIC (changes as units move) -- re-query periodically.
	// Returns bytes written, 0 if unavailable. Mirrors bwgame's tile.explored
	// (an inverted mask internally; this reports the natural sense).
	int32_t (*explored_tiles)(ob_bot_host*, uint8_t* out, int32_t max);
	// Build a Terran addon (Machine Shop, Comsat, Control Tower, Covert Ops,
	// Physics Lab) on `building`. The host derives the addon tile from units.dat
	// addon_position and issues the PlaceAddon order -- build() can't express
	// this (it takes a worker + a free tile). 1 on accept.
	int32_t (*build_addon)(ob_bot_host*, intptr_t building, int32_t addon_type);
	// Difficulty the launcher asked for: 0 very easy, 1 easy, 2 medium, 3 hard.
	int32_t (*difficulty)(ob_bot_host*);
	// Race of a player slot (0 Zerg, 1 Terran, 2 Protoss; -1 invalid) -- what the
	// lobby shows, so no fog concern.
	int32_t (*player_race)(ob_bot_host*, int32_t player);
	// 1 if `unit` can walk to map pixel (x,y) (ground connectivity; flyers always 1).
	int32_t (*is_reachable)(ob_bot_host*, intptr_t unit, int32_t x, int32_t y);

	// --- v5 additions (NULL when the host is older than the bot) -------
	// An ability with no target, on one of the bot's units (OB_AB_* below). 1 when
	// the engine accepted it (tech researched, unit able, not already doing it).
	int32_t (*use_ability)(ob_bot_host*, intptr_t unit, int32_t ability);
	// A targeted spell / ability by its BW order id (OB_ORDER_* below), at a map
	// pixel or on a unit. The engine checks tech, energy and range itself; 1 = the
	// order was issued. Only the listed spell orders are accepted.
	int32_t (*cast_at)(ob_bot_host*, intptr_t unit, int32_t order_id, int32_t x, int32_t y);
	int32_t (*cast_on)(ob_bot_host*, intptr_t unit, int32_t order_id, intptr_t target_unit);
} ob_host_api;

// Untargeted abilities for use_ability().
enum {
	OB_AB_STIM = 0,          // Stim Packs (marine / firebat)
	OB_AB_SIEGE = 1,         // Siege Tank -> siege mode
	OB_AB_UNSIEGE = 2,
	OB_AB_BURROW = 3,        // any burrower (a Lurker attacks only while burrowed)
	OB_AB_UNBURROW = 4,
	OB_AB_CLOAK = 5,         // wraith / ghost
	OB_AB_DECLOAK = 6,
	OB_AB_HOLD_POSITION = 7,
	OB_AB_TRAIN_FIGHTER = 8, // reaver scarab / carrier interceptor
};

// Targeted orders for cast_at() / cast_on() (bwenums.h Orders ids, 1.16.1).
enum {
	OB_ORDER_INFESTATION = 27, OB_ORDER_LOCKDOWN = 115, OB_ORDER_DARK_SWARM = 119, OB_ORDER_PARASITE = 120,
	OB_ORDER_SPAWN_BROODLINGS = 121, OB_ORDER_EMP = 122, OB_ORDER_PLACE_MINE = 132, OB_ORDER_RECALL = 137,
	OB_ORDER_SCANNER_SWEEP = 139, OB_ORDER_DEFENSIVE_MATRIX = 141, OB_ORDER_PSI_STORM = 142,
	OB_ORDER_IRRADIATE = 143, OB_ORDER_PLAGUE = 144, OB_ORDER_CONSUME = 145, OB_ORDER_ENSNARE = 146,
	OB_ORDER_STASIS = 147, OB_ORDER_HALLUCINATION = 148, OB_ORDER_RESTORATION = 180,
	OB_ORDER_DISRUPTION_WEB = 181, OB_ORDER_MIND_CONTROL = 182, OB_ORDER_FEEDBACK = 184,
	OB_ORDER_OPTICAL_FLARE = 185, OB_ORDER_MAELSTROM = 186,
};

// Bits in the map_tiles bytes.
enum {
	OB_TILE_WALKABLE  = 1 << 0,
	OB_TILE_BUILDABLE = 1 << 1,
	OB_TILE_HAS_CREEP = 1 << 2,
};

// Extended per-unit state, queried on demand (see unit_ex below). Kept out
// of ob_unit deliberately: ob_unit is written in bulk into a bot-allocated
// array, so growing it would break every already-compiled bot.
typedef struct ob_unit_ex {
	int32_t energy;                 // integer energy
	int32_t resources;              // minerals/gas left in a resource container
	intptr_t target;                // current order target unit, 0 = none
	intptr_t addon;                 // attached addon, 0 = none
	int32_t remaining_build_time;   // frames left constructing this unit
	int32_t remaining_train_time;   // frames left on the front queue item
	int32_t ground_cooldown;
	int32_t air_cooldown;
	int32_t is_training;            // 1 when the build queue is non-empty
	int32_t build_type;             // queued/under-construction type id, -1 = none
	int32_t is_lifted;
	int32_t is_burrowed;
} ob_unit_ex;

#ifdef __cplusplus
}
#endif

#endif
