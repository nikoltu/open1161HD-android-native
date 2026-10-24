// UnifiedAI - THE built-in AI: one bot for all three races with FOUR difficulty
// levels (very easy / easy / medium / hard). Grown from UnifiedLite (bot ABI v3);
// this file adds the difficulty layer on ABI v4.
//
// Difficulty = DATA: one `level_t` row per level (LEVELS[4]) of honest, non-
// cheating knobs -- decision cadence, reaction delay, order budget, economy
// targets, supply margin, opening quality, expansion count/triggers, wave size
// and timing, retreat, upgrade/tech depth, rush/anti-air response, scouting,
// resource discipline, idle handicap. The level comes from the launcher
// ("Sunkumas") through the host's v4 difficulty() query; a v3 host => medium,
// which is exactly UnifiedLite's behaviour.
//
// What makes it "unified":
//   * Race auto-detect. openbw_bot_race() returns -1 ("any"), so the host keeps
//     whatever race the launcher gave the slot (including Random). On the first
//     frame the bot reads its own race from the base it owns (Command Center /
//     Nexus / Hatchery) and the enemy's from theirs -- the host unit listing is
//     not fog-limited, so both are known at once. No ABI change needed.
//   * Declarative build orders. Each race x enemy-race matchup has an OPENING:
//     a supply-gated list of {at_supply, unit, count} steps executed in order
//     (the classic "9 pool", "1 gate core" notation). Authoring a new opening is
//     editing a table, not logic. When the opening is exhausted the reactive
//     MACRO engine (the proven Lite loops) takes over.
//   * Situational tactics. Every tick the bot derives flags from what it sees --
//     enemy rushing, enemy expanded, enemy air, army balance -- and reacts:
//     defensive structure + army-first on a rush, expand/pressure when the enemy
//     expands, anti-air on air, attack when ahead, hold when behind. The army
//     COMPOSITION is also per matchup (bio+medic vs Zerg, tanks vs Protoss...).
//
// Deterministic like the Lite bots: every command goes through the host's
// recorded action path. Reads only the public ABI.
//
// Unit / tech / upgrade ids verified against the BWAPI type tables (units.dat
// index; BWAPI TechType / UpgradeType). Zerg upgrades are intentionally absent in
// v1 (ids not yet verified -- do not guess).
//
// Build (from repo root): bash android/scripts/build-ai.sh

#include "bot_api.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------- ids ------
enum race_e { R_ZERG = 0, R_TERRAN = 1, R_PROTOSS = 2, R_UNKNOWN = -1 };

enum {
	// Terran
	T_MARINE = 0, T_GHOST = 1, T_VULTURE = 2, T_TANK = 5, T_SCV = 7, T_WRAITH = 8,
	T_FIREBAT = 32, T_MEDIC = 34,
	T_CC = 106, T_COMSAT = 107, T_DEPOT = 109, T_REFINERY = 110, T_BARRACKS = 111,
	T_ACADEMY = 112, T_FACTORY = 113, T_STARPORT = 114, T_MACHINE_SHOP = 120,
	T_EBAY = 122, T_TURRET = 124, T_BUNKER = 125,
	// Protoss
	P_PROBE = 64, P_ZEALOT = 65, P_DRAGOON = 66, P_HT = 67, P_DT = 61, P_REAVER = 83,
	P_OBSERVER = 84, P_NEXUS = 154, P_ROBO = 155, P_PYLON = 156, P_ASSIM = 157,
	P_GATEWAY = 160, P_CANNON = 162, P_CITADEL = 163, P_CORE = 164, P_ARCHIVES = 165,
	P_FORGE = 166, P_STARGATE = 167,
	// Zerg
	Z_LARVA = 35, Z_ZERGLING = 37, Z_HYDRA = 38, Z_ULTRA = 39, Z_DRONE = 41,
	Z_OVERLORD = 42, Z_MUTA = 43, Z_LURKER = 103, Z_HATCH = 131, Z_LAIR = 132,
	Z_HIVE = 133, Z_HYDRA_DEN = 135, Z_EVO = 139, Z_SPIRE = 141, Z_POOL = 142,
	Z_CREEP_COLONY = 143, Z_SPORE = 144, Z_SUNKEN = 146, Z_EXTRACTOR = 149,
	// neutral
	N_MIN1 = 176, N_MIN2 = 177, N_MIN3 = 178, N_GEYSER = 188,
};
// Extra unit ids (UnitTypeData.h / BWAPI.h, byte-exact from units.dat).
enum {
	T_GOLIATH = 3, T_SCIENCE_VESSEL = 9, T_DROPSHIP = 11, T_BATTLECRUISER = 12, T_TANK_SIEGED = 30, T_VALKYRIE = 58,
	P_CORSAIR = 60, P_DARK_ARCHON = 63, P_ARCHON = 68, P_SHUTTLE = 69, P_SCOUT = 70, P_ARBITER = 71, P_CARRIER = 72, P_INTERCEPTOR = 73,
	Z_GUARDIAN = 44, Z_QUEEN = 45, Z_DEFILER = 46, Z_SCOURGE = 47, Z_DEVOURER = 62,
};
// TechType ids (TechUpgradeData.h kTechTypes; index = line - 11).
enum { T_CONTROL_TOWER = 115, T_SCIENCE_FACILITY = 116, P_OBSERVATORY = 159, Z_EGG = 36, Z_LURKER_EGG = 97 };   // bwenums.h UnitTypes order
enum { TECH_STIM = 0, TECH_SIEGE_MODE = 5, TECH_BURROWING = 11, TECH_PSI_STORM = 19, TECH_LURKER_ASPECT = 32 };
// UpgradeType ids (kUpgradeTypes; index = line - 58). NOTE: 29 is Muscular Augments --
// the old UP_P_SINGULARITY=29 silently researched nothing (host rejects); Singularity
// Charge is 33. Every id below was read from the byte-exact table, never from memory.
enum { UP_T_INF_ARMOR = 0, UP_T_VEH_PLATING = 1, UP_T_INF_WEAPONS = 7, UP_T_VEH_WEAPONS = 8, UP_T_U238 = 16, UP_T_ION_THRUSTERS = 17,
       UP_P_GROUND_ARMOR = 5, UP_P_GROUND_WEAPONS = 13, UP_P_SINGULARITY = 33, UP_P_LEG_ENHANCEMENTS = 34,
       UP_Z_CARAPACE = 3, UP_Z_FLYER_CARAPACE = 4, UP_Z_MELEE = 10, UP_Z_MISSILE = 11, UP_Z_FLYER_ATTACKS = 12,
       UP_Z_METABOLIC_BOOST = 27, UP_Z_MUSCULAR_AUGMENTS = 29, UP_Z_GROOVED_SPINES = 30 };

static int is_mineral(int32_t t) { return t == N_MIN1 || t == N_MIN2 || t == N_MIN3; }
static int is_base(int32_t t) { return t == T_CC || t == P_NEXUS || t == Z_HATCH || t == Z_LAIR || t == Z_HIVE; }
static int is_worker(int32_t t) { return t == T_SCV || t == P_PROBE || t == Z_DRONE; }
static int race_of_base(int32_t t) {
	if (t == T_CC) return R_TERRAN;
	if (t == P_NEXUS) return R_PROTOSS;
	if (t == Z_HATCH || t == Z_LAIR || t == Z_HIVE) return R_ZERG;
	return R_UNKNOWN;
}
// Every flyer (the enemy_air flag skips overlords/observers/shuttles itself).
static int is_air_unit(int32_t t) {
	switch (t) {
	case T_WRAITH: case T_SCIENCE_VESSEL: case T_DROPSHIP: case T_BATTLECRUISER: case T_VALKYRIE:
	case P_CORSAIR: case P_SHUTTLE: case P_SCOUT: case P_ARBITER: case P_CARRIER: case P_INTERCEPTOR: case P_OBSERVER:
	case Z_OVERLORD: case Z_MUTA: case Z_GUARDIAN: case Z_QUEEN: case Z_SCOURGE: case Z_DEVOURER:
		return 1;
	default: return 0;
	}
}
// "Army" = anything that fights and is not a worker/building/larva/overlord.
// (Full list verified against units.dat ids; the old list missed Goliath, Archon,
// Carrier, Battlecruiser, Guardian... so enemy_air / army balance under-counted.)
static int is_army(int32_t t) {
	switch (t) {
	case T_MARINE: case T_GHOST: case T_VULTURE: case T_GOLIATH: case T_TANK: case T_TANK_SIEGED: case T_WRAITH:
	case T_BATTLECRUISER: case T_FIREBAT: case T_MEDIC: case T_VALKYRIE:   // (Science Vessel = detector escort, not army)
	case P_ZEALOT: case P_DRAGOON: case P_DT: case P_ARCHON: case P_DARK_ARCHON: case P_REAVER:   // (High Templar = caster escort, not army)
	case P_SCOUT: case P_CORSAIR: case P_ARBITER: case P_CARRIER:
	case Z_ZERGLING: case Z_HYDRA: case Z_ULTRA: case Z_MUTA: case Z_LURKER: case Z_GUARDIAN: case Z_QUEEN:
	case Z_DEFILER: case Z_SCOURGE: case Z_DEVOURER:
		return 1;
	default: return 0;
	}
}
// Fighting strength of a unit = its supply cost in half-supply units (a zergling
// 1, a marine / hydralisk 2, a zealot / dragoon / tank 4, an ultralisk 8 ...).
// Every army comparison used to COUNT units, so 24 zerglings "matched" 24
// zealots and Zerg attacked -- and died -- at a third of the enemy's strength
// (measured with botmatch: 112 of 120 Zerg fighters lost AWAY from home).
static int unit_str(int32_t t) {
	switch (t) {
	case Z_ZERGLING: case Z_SCOURGE: return 1;
	case T_MARINE: case T_FIREBAT: case T_MEDIC: case T_GHOST: case Z_HYDRA: return 2;
	case T_VULTURE: case T_GOLIATH: case T_TANK: case T_TANK_SIEGED: case T_WRAITH:
	case P_ZEALOT: case P_DRAGOON: case P_HT: case P_DT: case P_CORSAIR:
	case Z_MUTA: case Z_LURKER: case Z_GUARDIAN: case Z_QUEEN: case Z_DEFILER: case Z_DEVOURER: return 4;
	case T_VALKYRIE: case P_SCOUT: return 6;
	case P_ARCHON: case P_DARK_ARCHON: case P_REAVER: case P_ARBITER: case Z_ULTRA: return 8;
	case T_BATTLECRUISER: case P_CARRIER: return 12;
	default: return 0;
	}
}
static int64_t d2(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
	int64_t dx = ax - bx, dy = ay - by;
	return dx * dx + dy * dy;
}

// ------------------------------------------------- build orders (DATA) -----
// A step: once supply_used >= at_supply, make sure we own >= count of `unit`
// (counting in-progress). Steps run in order; the first unmet one is pursued.
// Buildings are placed by the ring search; units are trained by the macro
// engine's normal producer rules, so a step only needs to name WHAT and WHEN.
struct bo_step { int at_supply; int unit; int count; };
#define BO_END { -1, -1, 0 }

// ---- Terran openings (supply is in FULL units here; the ABI reports halves) --
static const bo_step BO_T_VS_ZERG[] = {   // bio + academy fast: marines/medics/firebats punish lings
	{ 9, T_DEPOT, 1 }, { 11, T_BARRACKS, 1 }, { 13, T_BARRACKS, 2 }, { 14, T_REFINERY, 1 },
	{ 16, T_DEPOT, 2 }, { 18, T_ACADEMY, 1 }, { 20, T_BARRACKS, 3 }, { 22, T_EBAY, 1 },
	{ 26, T_DEPOT, 3 }, { 30, T_CC, 2 }, BO_END };
// 2 rax + bunker BEFORE the factory. The straight 1-rax factory expand is a
// player's build that survives on wall-ins and micro; measured with
// botmatch (3 seeds, vs the native Protoss AI) it lost 2 of 3 to the first
// 6-zealot push at 4:30 with three marines on the field.
static const bo_step BO_T_VS_PROTOSS[] = { // 2 rax + bunker -> factory/tanks
	{ 9, T_DEPOT, 1 }, { 11, T_BARRACKS, 1 }, { 13, T_BARRACKS, 2 }, { 15, T_DEPOT, 2 },
	{ 16, T_BUNKER, 1 }, { 18, T_REFINERY, 1 }, { 21, T_FACTORY, 1 }, { 23, T_DEPOT, 3 },
	{ 25, T_MACHINE_SHOP, 1 }, { 27, T_ACADEMY, 1 }, { 30, T_FACTORY, 2 }, { 34, T_CC, 2 }, BO_END };
static const bo_step BO_T_VS_TERRAN[] = {  // mixed: rax bio + a factory for tanks
	{ 9, T_DEPOT, 1 }, { 11, T_BARRACKS, 1 }, { 12, T_REFINERY, 1 }, { 15, T_DEPOT, 2 },
	{ 16, T_FACTORY, 1 }, { 18, T_BARRACKS, 2 }, { 20, T_ACADEMY, 1 }, { 22, T_MACHINE_SHOP, 1 },
	{ 24, T_DEPOT, 3 }, { 30, T_CC, 2 }, BO_END };

// ---- Protoss openings --------------------------------------------------------
static const bo_step BO_P_VS_ZERG[] = {   // zealot-heavy + forge (cannons on a rush), then goons
	{ 8, P_PYLON, 1 }, { 10, P_GATEWAY, 1 }, { 12, P_GATEWAY, 2 }, { 14, P_FORGE, 1 },
	{ 15, P_ASSIM, 1 }, { 16, P_PYLON, 2 }, { 18, P_CORE, 1 }, { 22, P_PYLON, 3 },
	{ 24, P_GATEWAY, 3 }, { 30, P_NEXUS, 2 }, BO_END };
static const bo_step BO_P_VS_TERRAN[] = { // 1 gate core -> dragoon range, then DT tech
	{ 8, P_PYLON, 1 }, { 10, P_GATEWAY, 1 }, { 11, P_ASSIM, 1 }, { 13, P_CORE, 1 },
	{ 15, P_PYLON, 2 }, { 17, P_GATEWAY, 2 }, { 20, P_CITADEL, 1 }, { 22, P_PYLON, 3 },
	{ 24, P_ARCHIVES, 1 }, { 26, P_FORGE, 1 }, { 30, P_NEXUS, 2 }, BO_END };
// Zealots first, gas later. The gas-at-13 version had 3 zealots at 4:30 against
// the native Protoss AI's 7 and lost the mirror in 2 of 3 seeds (botmatch).
static const bo_step BO_P_VS_PROTOSS[] = { // 3 gate zealots -> core/goons -> forge
	{ 8, P_PYLON, 1 }, { 10, P_GATEWAY, 1 }, { 12, P_GATEWAY, 2 }, { 15, P_PYLON, 2 },
	{ 18, P_GATEWAY, 3 }, { 21, P_ASSIM, 1 }, { 23, P_PYLON, 3 }, { 24, P_CORE, 1 },
	{ 28, P_FORGE, 1 }, { 32, P_NEXUS, 2 }, BO_END };

// ---- Zerg openings (buildings only -- units come from larva rules) ----------
// Pool BEFORE the second hatchery ("overpool"): measured with botmatch, the
// 12-hatch-then-pool order had its first zerglings at ~4:50 -- after the native
// Protoss/Terran AI's first push had already killed the drones at the natural.
// Hydralisk den BEFORE the lair, gas at 13: measured (botmatch eval, hard): Zerg
// lost ~460 fighters for ~160 against Terran and ~330 for ~85 against Protoss --
// zerglings fed into marines / zealots because the den came after 100 gas of lair.
static const bo_step BO_Z_VS_TERRAN[] = { // overpool -> hatch -> hydra den -> lair -> spire
	{ 9, Z_OVERLORD, 2 }, { 10, Z_POOL, 1 }, { 12, Z_HATCH, 2 }, { 13, Z_EXTRACTOR, 1 },
	{ 16, Z_OVERLORD, 3 }, { 17, Z_HYDRA_DEN, 1 }, { 19, Z_LAIR, 1 }, { 24, Z_OVERLORD, 4 },
	{ 30, Z_HATCH, 3 }, { 32, Z_EVO, 1 }, { 38, Z_SPIRE, 1 }, BO_END };   // lair right after the den: lurkers are the plan vs bio
static const bo_step BO_Z_VS_PROTOSS[] = { // overpool -> hatch -> hydra den (hydras vs gateway)
	{ 9, Z_OVERLORD, 2 }, { 10, Z_POOL, 1 }, { 12, Z_HATCH, 2 }, { 13, Z_EXTRACTOR, 1 },
	{ 16, Z_OVERLORD, 3 }, { 17, Z_HYDRA_DEN, 1 }, { 22, Z_EVO, 1 }, { 24, Z_OVERLORD, 4 },
	{ 26, Z_LAIR, 1 }, { 30, Z_HATCH, 3 }, BO_END };
static const bo_step BO_Z_VS_ZERG[] = {   // 9 pool: lings first, mirror is decided early
	{ 9, Z_POOL, 1 }, { 9, Z_OVERLORD, 2 }, { 11, Z_EXTRACTOR, 1 }, { 14, Z_OVERLORD, 3 },
	{ 16, Z_HATCH, 2 }, { 18, Z_LAIR, 1 }, { 20, Z_HYDRA_DEN, 1 }, { 24, Z_OVERLORD, 4 },
	{ 26, Z_SPIRE, 1 }, BO_END };

// ---- Generic openings (levels 0-1: one per race, no matchup knowledge) --------
static const bo_step BO_T_GENERIC[] = {
	{ 9, T_DEPOT, 1 }, { 11, T_BARRACKS, 1 }, { 15, T_DEPOT, 2 }, { 16, T_REFINERY, 1 },
	{ 20, T_BARRACKS, 2 }, { 24, T_DEPOT, 3 }, BO_END };
static const bo_step BO_P_GENERIC[] = {
	{ 8, P_PYLON, 1 }, { 10, P_GATEWAY, 1 }, { 14, P_ASSIM, 1 }, { 16, P_PYLON, 2 },
	{ 18, P_GATEWAY, 2 }, { 22, P_CORE, 1 }, { 24, P_PYLON, 3 }, BO_END };
static const bo_step BO_Z_GENERIC[] = {
	{ 9, Z_OVERLORD, 2 }, { 11, Z_POOL, 1 }, { 13, Z_EXTRACTOR, 1 }, { 16, Z_OVERLORD, 3 },
	{ 18, Z_HATCH, 2 }, { 24, Z_OVERLORD, 4 }, BO_END };

// kind: 0/1 generic per race, 2 matchup (UnifiedLite's nine tables).
static const bo_step* pick_opening(int me, int them, int kind) {
	if (kind < 2) return me == R_TERRAN ? BO_T_GENERIC : me == R_PROTOSS ? BO_P_GENERIC : BO_Z_GENERIC;
	if (me == R_TERRAN)  return them == R_PROTOSS ? BO_T_VS_PROTOSS : them == R_TERRAN ? BO_T_VS_TERRAN : BO_T_VS_ZERG;
	if (me == R_PROTOSS) return them == R_TERRAN ? BO_P_VS_TERRAN : them == R_PROTOSS ? BO_P_VS_PROTOSS : BO_P_VS_ZERG;
	/* zerg */           return them == R_PROTOSS ? BO_Z_VS_PROTOSS : them == R_ZERG ? BO_Z_VS_ZERG : BO_Z_VS_TERRAN;
}
static const char* race_name(int r) { return r == R_ZERG ? "zerg" : r == R_TERRAN ? "terran" : r == R_PROTOSS ? "protoss" : "?"; }

// ------------------------------------------------------ difficulty (DATA) ----
// Every knob is an honest handicap/skill setting -- nothing here grants the bot
// information or resources a player could not have. Medium reproduces
// UnifiedLite exactly (the device-validated baseline); the other rows scale the
// same machinery down or up. Start values; tune from the telemetry.
struct level_t {
	int tick;                  // frames between decisions (24 = 1 game second)
	int react_delay;           // frames a rush/air/expansion flag must persist before we react
	int max_orders;            // army orders per tick (0 = unlimited): a slow player can't move everything at once
	int workers_per_base, max_workers;
	int supply_margin_base, supply_margin_per_prod;
	int opening_kind, opening_delay_supply;   // generic/matchup; supply added to every step (slower build)
	int max_bases, expand_min_workers, expand_float;   // expansion cap and triggers (float 0 = off)
	int wave_min, attack_cooldown, ahead_num, ahead_den, retreat_pct;   // attack sizing/timing; retreat when army < enemy*pct
	int upgrades_max, tech_depth, rush_defense, anti_air, scout;        // depth of tech/upgrades; response levels; scouting
	// scout: 0 none (overlords sit in a blob over the main, nothing is explored);
	//        1 early natural scout + overlords spread around the base;
	//        2 + worker scout to the enemy, overlords on the nearby expansions, hurt overlords retreat;
	//        3 + overlords watch every free expansion and the enemy's approach
	int handicap_idle, saving; // % of ticks skipped entirely; 0 never bank, 1 bank for depots only, 2 full discipline
	int micro;                 // 0 none (units trickle), 1 army acts as a group + retreats, 2 + focus fire / pull-back / kiting
};
static const level_t LEVELS[4] = {
	// tick react ord | wk/b maxwk | sm smp | ok dly | bases exw exf | wave  cd  an ad ret | upg tec rsh aa sc | idle sav mic
	{ 48, 720, 4,     10, 20,      2, 0,     0, 6,     1, 99,   0,     30, 2400, 1, 1,  0,    0, 0, 0, 0, 0,    30, 0, 0 },  // 0 very easy
	{ 24, 360, 8,     14, 40,      4, 0,     1, 3,     2, 20,   0,     24, 1200, 1, 1,  0,    1, 1, 1, 1, 1,    10, 1, 0 },  // 1 easy
	{ 12,  96, 0,     16, 60,      4, 4,     2, 0,     3, 16, 1000,    18, 1200, 1, 1, 40,    3, 2, 2, 2, 2,     0, 2, 1 },  // 2 medium
	{  6,   0, 0,     18, 70,      4, 4,     2, 0,     5, 16,  600,    18,  900, 10, 11, 70,  3, 3, 3, 3, 3,     0, 2, 2 },  // 3 hard
};
static const char* LEVEL_NAMES[4] = { "very easy", "easy", "medium", "hard" };

// ---------------------------------------------------------------- bot ------
struct bot_t {
	ob_bot_host* h;
	const ob_host_api* api;
	int race = R_UNKNOWN, enemy_race = R_UNKNOWN;
	const bo_step* opening = nullptr;
	int bo_done = 0;              // steps completed (advance pointer)
	int32_t bo_step_since = -1;   // frame the current step became actionable (-1 = not yet)
	int32_t bo_early_since = -1;
	int32_t bo_stuck = 0;         // frames the current step was affordable but still not placed  // frame the current step became "one supply early" (-1 = not)
	int32_t last_build_frame = 0;
	int32_t last_attack_frame = 0;
	int32_t base_hp_last = -1;
	int32_t defend_until = 0;
	int32_t last_expand_log = -100000;   // rate-limit the expansion diagnostics
	int32_t expand_issued = -100000;     // frame the last depot build was accepted (re-issue guard)
	int32_t expand_moved = -100000;      // frame the scout was last sent to the site
	int32_t last_place_log = -100000;    // rate-limit the home-placement diagnostics
	int32_t map_tw = 0, map_th = 0;      // map size in tiles (ring probes stay inside it)
	// A building we ordered is "pending" until the worker actually starts it (it
	// then shows up in our unit list) or ~25 s pass. Re-placing a pending type
	// re-targets the walking worker so it never starts (measured: Academy
	// re-issued every cooldown, bord=30 forever), and spending its minerals
	// starves it -- so while pending we neither re-place nor un-save.
	int pending_type = -1; int32_t pending_since = -100000; int pending_cnt = 0;
	int bo_want = -1, bo_afford = 0, bo_saving = 0, builder_order = -1;   // telemetry
	bool announced = false, warned_no_base = false;
	// situational flags (recomputed each tick)
	bool rush = false, enemy_expanded = false, enemy_air = false;
	int my_army = 0, enemy_army = 0;
	// difficulty layer
	const level_t* L = &LEVELS[2]; int level_id = 2;
	level_t Lcopy;                                           // only used by the botmatch micro A/B override
	uint32_t rng = 0x9e3779b9u;                              // bot-side LCG for the idle handicap (not the sim's)
	int32_t rush_since = -1, air_since = -1, exp_since = -1;  // react_delay bookkeeping per flag
	bool scout_sent = false; int32_t scout_sent_frame = -1;   // worker scout (levels with scout >= 2)
	int orders_left = 0;                                      // per-tick army order budget (max_orders)
	int workers_max_seen = 0;                                 // DIAG: detect worker losses
	int tel_cc = 0, tel_rax = 0, tel_near = 0, tel_hold = 0;  // DIAG: why production did not happen this tick
	// The MAIN is where we started -- not "the leftmost base" (after an expansion to
	// the west the old rule moved every home placement, the rally point and the
	// expansion search to the new base).
	int32_t home_x = -1, home_y = -1;
	int32_t nat_scout_frame = -1;                             // early natural scout (levels with scout >= 1); -1 = not sent
	int32_t want_expand_frame = -100000;                      // last tick the macro asked for a new base
	int32_t last_transfer_frame = -100000;                    // worker transfer to an under-saturated base
	// Overlord posts (Zerg): where an overlord was sent and when, keyed by position
	// (unit handles do not survive a frame). `danger_until` blacklists a post whose
	// overlord came home hurt.
	struct post_t { int32_t x, y, sent, danger_until; };
	post_t posts[40]; int nposts = 0;
	int32_t last_ovie_order = -100000;
	struct bad_t { int32_t x, y, until; };
	bad_t bad[8]; int nbad = 0;                               // expansion sites that failed (see site_blacklist)
	int32_t exp_site_x = 0, exp_site_y = 0, exp_site_since = -1; int exp_fail = 0;   // the site being taken + how long / how often it failed
	int bases_at_issue = 0; int32_t expand_wait = 600;
	int tel_threat = 0;
	int32_t last_scan = -100000, storm_frame = -100000, storm_x = 0, storm_y = 0;
	int ab_siege = 0, ab_stim = 0, ab_burrow = 0, ab_storm = 0, ab_scan = 0;   // DIAG: abilities issued
	bool enemy_cloak = false;            // the enemy has shown cloak / burrow tech (DT, lurker, arbiter, ghost)
	int32_t last_escort_order = -100000;
	// army group state (micro >= 1)
	bool attacking = false; int32_t atk_since = -100000, atk_x = 0, atk_y = 0, last_army_order = -100000;
	int tel_out = 0, tel_en_near = 0;
	int atk_peak = 0, atk_enemy0 = 0;   // most units the current attack had out / enemy army when it left
	int tel_plan = 0, tel_ring = 0;                           // DIAG: placements by the planner vs the ring fallback
};

extern "C" int openbw_bot_abi(void) { return 5; }
// "any": the host keeps the launcher's slot race; we detect it from our base.
extern "C" int openbw_bot_race(void) { return -1; }

extern "C" void* openbw_bot_create(ob_bot_host* h, const ob_host_api* api) {
	bot_t* b = new bot_t();
	b->h = h;
	b->api = api;
	{ int32_t w = 0, hh = 0; if (api->map_size && api->map_size(h, &w, &hh)) { b->map_tw = w / 32; b->map_th = hh / 32; } }
	// The level comes from the launcher through the v4 host; a v3 host has no
	// difficulty() (its table is shorter -- never touch v4 fields without the
	// version check) and gets medium = UnifiedLite.
	int lvl = (api->abi_version >= 4 && api->difficulty) ? api->difficulty(h) : 2;
	// A/B hook for botmatch only (the launcher never sends >= 100): level
	// + 100 * (1 + micro) overrides the level's micro knob, so "hard with micro"
	// can be played against "hard without" and the micro layer measured alone.
	int micro_override = -1;
	if (lvl >= 100) { micro_override = lvl / 100 - 1; lvl %= 100; }
	if (lvl < 0) lvl = 0;
	if (lvl > 3) lvl = 3;
	b->level_id = lvl; b->L = &LEVELS[lvl];
	if (micro_override >= 0) { b->Lcopy = LEVELS[lvl]; b->Lcopy.micro = micro_override > 2 ? 2 : micro_override; b->L = &b->Lcopy; }
	char s[128];
	snprintf(s, sizeof s, "UnifiedAI ready (level %d: %s; all races, detecting from base)", lvl, LEVEL_NAMES[lvl]);
	api->log(h, s);
	return b;
}
extern "C" void openbw_bot_destroy(void* p) { delete (bot_t*)p; }

// Ring-search placement around a tile. Zerg needs creep (hatchery ring, every
// tile); Protoss needs pylon power (first pylon by the nexus, same ring);
// Terran is free. `step` 2 skips alternate tiles for the bigger T/P footprints.
static int try_build(bot_t* b, intptr_t worker, int32_t type, int32_t ctx, int32_t cty, int32_t rmin, int32_t rmax, int step) {
	for (int32_t r = rmin; r <= rmax; ++r) {
		for (int32_t dy = -r; dy <= r; dy += step) {
			for (int32_t dx = -r; dx <= r; dx += step) {
				if (abs(dx) != r && abs(dy) != r) continue; // ring only
				int32_t tx = ctx + dx, ty = cty + dy;
				// Probing outside the map is a guaranteed reject that the host logs as
				// BOT_INVALID_CMD (edge expansions rang 5 warnings per attempt); skip it.
				if (tx < 0 || ty < 0 || (b->map_tw && (tx + 4 > b->map_tw || ty + 3 > b->map_th))) continue;
				if (b->api->build(b->h, worker, type, tx, ty)) return 1;
			}
		}
	}
	return 0;
}

// Census of one player's units into a fixed histogram + a few handles.
struct census_t {
	int cnt[256];        // completed + in-progress, by type id (0..255)
	int done[256];       // completed only
	ob_unit* base;       // main base (leftmost completed)
	int bases;           // base-type buildings, any state
	int workers, army, air;
	int str;             // army strength (unit_str sum, completed fighters)
	int sdef;            // static defence strength (completed bunkers / cannons / sunkens)
	int32_t base_hp;
	int larvae;
	ob_unit* by_type[256]; // a completed instance of each type (tech/producers)
};
static void census(ob_unit* u, int n, census_t& c, bool mine) {
	memset(&c, 0, sizeof c);
	for (int i = 0; i != n; ++i) {
		ob_unit& x = u[i];
		int t = x.type;
		if (t < 0 || t >= 256) continue;
		++c.cnt[t];
		if (x.completed) { ++c.done[t]; if (!c.by_type[t]) c.by_type[t] = &x; }
		if (is_base(t)) { ++c.bases; if (x.completed && (!c.base || x.x < c.base->x)) c.base = &x; c.base_hp += x.hp; }
		if (is_worker(t) && x.completed) ++c.workers;
		if (is_army(t) && x.completed) { ++c.army; c.str += unit_str(t); }
		if (x.completed) { if (t == T_BUNKER) c.sdef += 8; else if (t == P_CANNON || t == Z_SUNKEN) c.sdef += 5; }
		if (is_air_unit(t) && x.completed && t != Z_OVERLORD && t != P_OBSERVER) ++c.air;
		if (t == Z_LARVA) ++c.larvae;
		if (mine && (t == T_BARRACKS || t == T_DEPOT || P_GATEWAY == t || t == P_PYLON)) c.base_hp += x.hp;
	}
}

// Does the opening's current step need a building we don't have yet?
// Returns the unit id to build (or -1) and advances past satisfied steps.
// `early` is set when the step is ONE supply away: not yet time to place, but time
// to start banking for it (real "9 depot" play stops spending at 8 -- otherwise the
// bot reaches 9 with 0 minerals, then supply-blocks while it saves; measured L3 TvP:
// first depot done ~f2400, 20/20 block, whole economy late).
static int bo_next(bot_t* b, census_t& me, int supply_full, int32_t frame, bool& early) {
	early = false;
	if (!b->opening) return -1;
	for (;;) {
		const bo_step& s = b->opening[b->bo_done];
		if (s.at_supply < 0) return -1;                 // exhausted -> reactive macro
		if (me.cnt[s.unit] >= s.count) { ++b->bo_done; b->bo_step_since = -1; b->bo_stuck = 0; continue; } // satisfied
		if (is_base(s.unit) && me.bases >= b->L->max_bases) { ++b->bo_done; b->bo_step_since = -1; b->bo_stuck = 0; continue; } // level caps bases
		int at = s.at_supply + b->L->opening_delay_supply;
		// The delay must stay reachable: with no army yet, supply only grows with
		// workers, and the level's worker target caps that (measured L0: barracks
		// at 11+6=17 while 10 workers stop supply at 11 -> nothing ever built).
		if (at > b->L->workers_per_base) at = s.at_supply > b->L->workers_per_base ? s.at_supply : b->L->workers_per_base;
		if (supply_full < at) {
			if (supply_full < at - 1) { b->bo_early_since = -1; return -1; }   // not yet time (slower levels build later)
			// One supply early: bank for it. If supply then stalls here (worker cap
			// reached, no producers yet -- measured L0: 10/10 workers, "early" forever,
			// 1500 minerals floating) the step is treated as due after ~30 s.
			if (b->bo_early_since < 0) b->bo_early_since = frame;
			if (frame - b->bo_early_since < 720) { early = true; return s.unit; }
		}
		b->bo_early_since = -1;
		// A step that cannot be actioned (an unsupported morph/addon, no room to
		// place, unaffordable for too long) must never deadlock the whole opening:
		// give it ~1 minute once it is actionable, then skip it and move on. A
		// depot step gets longer: it has to bank 300-400 AND scout the site first.
		// The clock only runs while the step is AFFORDABLE and still not placed
		// (bo_stuck, counted by the caller). Timing out a step we simply could not
		// pay for yet dropped the spawning pool itself -- measured: an SCV scout had
		// the drones chasing it, income dipped, the pool "timed out" at 3:00 and the
		// bot had no army for ten minutes. A step unpaid for 5 minutes is still
		// skipped, so a real deadlock cannot hang the opening.
		int limit = is_base(s.unit) ? 4800 : 2400;
		if (b->bo_stuck > limit || (b->bo_step_since >= 0 && frame - b->bo_step_since > 7200)) { ++b->bo_done; b->bo_step_since = -1; b->bo_stuck = 0; continue; }
		if (b->bo_step_since < 0) b->bo_step_since = frame;
		return s.unit;
	}
}

// Expansion sites. start_locations only lists the player STARTS, so on a (2) map
// both are taken and a start-location-based expander never expands (the Lite
// bots' latent 1-base bug). Instead cluster the neutral mineral fields; a cluster
// with a town hall (ours or theirs) next to it is taken. Works on any map, any race.
struct cl_t { int64_t sx, sy; int cnt; int32_t cx, cy; bool taken; };
static cl_t g_cls[64]; static int g_ncls = 0;
static void collect_clusters(ob_unit* mine, int n, ob_unit* theirs, int ne, ob_unit* neutral, int nn) {
	int nc = 0;
	for (int k = 0; k != nn; ++k) {
		if (!is_mineral(neutral[k].type)) continue;
		int32_t x = neutral[k].x, y = neutral[k].y; int best = -1; int64_t bd = -1;
		for (int c = 0; c != nc; ++c) { int64_t d = d2(x, y, g_cls[c].cx, g_cls[c].cy); if (d < (int64_t)320 * 320 && (best < 0 || d < bd)) { best = c; bd = d; } }
		if (best < 0) { if (nc == 64) continue; best = nc++; g_cls[best] = cl_t{0, 0, 0, x, y, false}; }
		g_cls[best].sx += x; g_cls[best].sy += y; ++g_cls[best].cnt;
		g_cls[best].cx = (int32_t)(g_cls[best].sx / g_cls[best].cnt); g_cls[best].cy = (int32_t)(g_cls[best].sy / g_cls[best].cnt);
	}
	// "Taken" = a town hall mines THIS cluster, i.e. sits within 7 tiles of one of
	// its FIELDS. (The old test measured to the cluster CENTROID: on Challenger the
	// main Nexus is 7.5 tiles from its own line's centroid, so the main's minerals
	// counted as a free site and the "expansion" was a second Nexus in the main --
	// found with botmatch.) A neighbouring natural's fields are >8 tiles from
	// the main's town hall, so it stays free.
	for (int c = 0; c != nc; ++c) g_cls[c].taken = g_cls[c].cnt < 3;   // a stray field is not a base site
	for (int k = 0; k != nn; ++k) {
		if (!is_mineral(neutral[k].type)) continue;
		int best = -1; int64_t bd = -1;
		for (int c = 0; c != nc; ++c) { int64_t d = d2(neutral[k].x, neutral[k].y, g_cls[c].cx, g_cls[c].cy); if (best < 0 || d < bd) { best = c; bd = d; } }
		if (best < 0 || g_cls[best].taken) continue;
		for (int i = 0; i != n; ++i) if (is_base(mine[i].type) && d2(neutral[k].x, neutral[k].y, mine[i].x, mine[i].y) < (int64_t)224 * 224) { g_cls[best].taken = true; break; }
		for (int i = 0; i != ne && !g_cls[best].taken; ++i) if (is_base(theirs[i].type) && d2(neutral[k].x, neutral[k].y, theirs[i].x, theirs[i].y) < (int64_t)224 * 224) g_cls[best].taken = true;
	}
	g_ncls = nc;
}
// A site we failed to take (island, blocked, enemy army on it) is skipped for a while.
static bool site_blacklisted(const bot_t* b, int32_t x, int32_t y, int32_t frame) {
	for (int i = 0; i != b->nbad; ++i) if (frame < b->bad[i].until && d2(x, y, b->bad[i].x, b->bad[i].y) < (int64_t)160 * 160) return true;
	return false;
}
static void site_blacklist(bot_t* b, int32_t x, int32_t y, int32_t until) {
	int slot = b->nbad < 8 ? b->nbad++ : 0;
	for (int i = 0; i != b->nbad; ++i) if (d2(x, y, b->bad[i].x, b->bad[i].y) < (int64_t)160 * 160) { slot = i; break; }
	b->bad[slot].x = x; b->bad[slot].y = y; b->bad[slot].until = until;
}
// Next base: the free cluster closest to HOME, leaning away from the enemy (the
// natural first, then outward on our side of the map).
static int find_expansion(const bot_t* b, int32_t frame, int32_t hx, int32_t hy, int32_t ex, int32_t ey, bool have_enemy, int32_t& ox, int32_t& oy) {
	int pick = -1; int64_t bs = 0;
	for (int c = 0; c != g_ncls; ++c) {
		if (g_cls[c].taken || site_blacklisted(b, g_cls[c].cx, g_cls[c].cy, frame)) continue;
		int64_t s = (int64_t)sqrt((double)d2(g_cls[c].cx, g_cls[c].cy, hx, hy));
		if (have_enemy) s -= (int64_t)sqrt((double)d2(g_cls[c].cx, g_cls[c].cy, ex, ey)) / 3;
		if (pick < 0 || s < bs) { pick = c; bs = s; }
	}
	if (pick < 0) return 0;
	ox = g_cls[pick].cx; oy = g_cls[pick].cy;
	return 1;
}

// ============================================================ base planner ===
// Where a building goes is decided here, not by "first free tile in a ring
// around the town hall". Measured with botmatch (host harness) before this
// existed: Protoss stacked every pylon in one touching column (nearest-neighbour
// 2 tiles, all "clumped"), 6 of 9 buildings hugged the Nexus, 9 touching pairs --
// no lanes for units, and too little power field for later gateways. A player
// lays a base out by ROLE:
//   production  toward the front (enemy side), in rows with an exit lane
//   supply      at the back, depots in pairs
//   tech        off to the side, near the town hall
//   pylons      spread so their fields cover the base (and one per expansion)
//   defence     at the front
// and keeps the mineral line, the worker lanes, addon slots and a one-tile gap
// between buildings free. The planner scores candidate tiles on a bot-side grid
// (terrain + everything we can see) and lets the HOST validate the best ones
// (power field, creep, units in the way) -- so it can only ever be as legal as
// the old ring search, never less.
static const int MAXT = 256 * 256;
enum { G_BLOCK = 1, G_BLD = 2, G_RES = 4, G_UNEXPL = 8, G_CREEP = 16, G_DEPOT = 32, G_PAIRED = 64 };
static uint8_t g_grid[MAXT];
static uint8_t g_tmp[MAXT];

// Building footprints in build tiles (units.dat placement box / 32).
static bool bld_size(int t, int& w, int& h) {
	switch (t) {
	case T_CC: case T_BARRACKS: case T_FACTORY: case T_STARPORT: case T_EBAY: case 116 /*science facility*/:
	case P_NEXUS: case P_GATEWAY: case P_STARGATE:
	case 130 /*infested CC*/: case Z_HATCH: case Z_LAIR: case Z_HIVE:
		w = 4; h = 3; return true;
	case T_REFINERY: case P_ASSIM: case Z_EXTRACTOR: case N_GEYSER: case 136 /*defiler mound*/:
		w = 4; h = 2; return true;
	case T_TURRET: case P_PYLON: case P_CANNON: case Z_CREEP_COLONY: case Z_SPORE: case Z_SUNKEN: case Z_SPIRE:
	case 137 /*greater spire*/: case 134 /*nydus*/: case T_COMSAT: case 108 /*nuke silo*/: case 115 /*control tower*/:
	case 117 /*covert ops*/: case 118 /*physics lab*/: case T_MACHINE_SHOP:
		w = 2; h = 2; return true;
	case N_MIN1: case N_MIN2: case N_MIN3:
		w = 2; h = 1; return true;
	default:
		if (t >= 106 && t <= 172) { w = 3; h = 2; return true; }
		return false;
	}
}
enum { ROLE_PROD, ROLE_SUPPLY, ROLE_TECH, ROLE_PYLON, ROLE_DEFENCE };
static int role_of(int t) {
	switch (t) {
	case T_BARRACKS: case T_FACTORY: case T_STARPORT: case P_GATEWAY: case P_ROBO: case P_STARGATE: return ROLE_PROD;
	case T_DEPOT: return ROLE_SUPPLY;
	case P_PYLON: return ROLE_PYLON;
	case T_BUNKER: case T_TURRET: case P_CANNON: case Z_CREEP_COLONY: return ROLE_DEFENCE;
	default: return ROLE_TECH;
	}
}
static bool has_addon_slot(int t) { return t == T_CC || t == T_FACTORY || t == T_STARPORT || t == 116; }

struct plan_ctx {
	bot_t* b;
	ob_unit* mine; int n;
	ob_unit* theirs; int ne;
	ob_unit* neutral; int nn;
	int W, H;
	int hx, hy;          // main town hall centre, tiles
	int btlx, btly;      // main town hall top-left tile (lattice origin)
	int fx, fy;          // unit vector toward the front (enemy / map centre), x1000
	int mx, my;          // unit vector toward the mineral line, x1000 (0,0 = none)
};
static inline void tl_of(const ob_unit& u, int w, int h, int& x0, int& y0) { x0 = (u.x - w * 16) / 32; y0 = (u.y - h * 16) / 32; }
static inline void g_set(const plan_ctx& c, int x, int y, uint8_t f) { if (x >= 0 && y >= 0 && x < c.W && y < c.H) g_grid[y * c.W + x] |= f; }
static inline uint8_t g_get(const plan_ctx& c, int x, int y) { return (x >= 0 && y >= 0 && x < c.W && y < c.H) ? g_grid[y * c.W + x] : (uint8_t)G_BLOCK; }

static bool build_grid(plan_ctx& c) {
	bot_t* b = c.b; const ob_host_api* api = b->api;
	c.W = b->map_tw; c.H = b->map_th;
	if (c.W <= 0 || c.H <= 0 || c.W * c.H > MAXT) return false;
	if (!(api->abi_version >= 3 && api->map_tiles && api->map_tiles(b->h, g_tmp, MAXT))) return false;
	int N = c.W * c.H;
	for (int i = 0; i != N; ++i) g_grid[i] = (uint8_t)(((g_tmp[i] & OB_TILE_BUILDABLE) ? 0 : G_BLOCK) | ((g_tmp[i] & OB_TILE_HAS_CREEP) ? G_CREEP : 0));
	if (api->abi_version >= 4 && api->explored_tiles && api->explored_tiles(b->h, g_tmp, MAXT))
		for (int i = 0; i != N; ++i) if (!g_tmp[i]) g_grid[i] |= G_UNEXPL;
	auto mark = [&](ob_unit* u, int cnt) {
		for (int i = 0; i != cnt; ++i) {
			int w, h; if (!bld_size(u[i].type, w, h)) continue;
			int x0, y0; tl_of(u[i], w, h, x0, y0);
			uint8_t f = (uint8_t)(G_BLD | (u[i].type == T_DEPOT ? G_DEPOT : 0));
			for (int y = y0; y != y0 + h; ++y) for (int x = x0; x != x0 + w; ++x) g_set(c, x, y, f);
		}
	};
	mark(c.mine, c.n); mark(c.theirs, c.ne); mark(c.neutral, c.nn);
	for (int i = 0; i != c.n; ++i) {
		ob_unit& u = c.mine[i];
		int w, h; if (!bld_size(u.type, w, h)) continue;
		int x0, y0; tl_of(u, w, h, x0, y0);
		// worker circulation ring around a town hall
		if (is_base(u.type)) for (int y = y0 - 2; y != y0 + h + 2; ++y) for (int x = x0 - 2; x != x0 + w + 2; ++x) g_set(c, x, y, G_RES);
		// addon slot (2x2 at the right-bottom of the parent)
		if (has_addon_slot(u.type)) for (int y = y0 + h - 2; y != y0 + h; ++y) for (int x = x0 + w; x != x0 + w + 2; ++x) g_set(c, x, y, G_RES);
		// exit lane under a production building (units pop out at the bottom)
		if (role_of(u.type) == ROLE_PROD) for (int x = x0 - 1; x != x0 + w + 1; ++x) g_set(c, x, y0 + h, G_RES);
		// a depot that already touches a building is "paired": no third one joins the row
		if (u.type == T_DEPOT) {
			bool touch = false;
			for (int y = y0 - 1; y != y0 + h + 1 && !touch; ++y) for (int x = x0 - 1; x != x0 + w + 1; ++x) {
				if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) continue;
				if (g_get(c, x, y) & G_BLD) { touch = true; break; }
			}
			if (touch) for (int y = y0; y != y0 + h; ++y) for (int x = x0; x != x0 + w; ++x) g_set(c, x, y, G_PAIRED);
		}
	}
	// mining lanes: the strip between each town hall and its minerals / gas
	for (int i = 0; i != c.n; ++i) {
		if (!is_base(c.mine[i].type)) continue;
		int bx = c.mine[i].x, by = c.mine[i].y;
		auto lane = [&](int rx, int ry) {
			if (d2(bx, by, rx, ry) > (int64_t)320 * 320) return;
			for (int s = 0; s <= 16; ++s) {
				int px = (bx + (rx - bx) * s / 16) / 32, py = (by + (ry - by) * s / 16) / 32;
				for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) g_set(c, px + dx, py + dy, G_RES);
			}
		};
		for (int k = 0; k != c.nn; ++k) if (is_mineral(c.neutral[k].type) || c.neutral[k].type == N_GEYSER) lane(c.neutral[k].x, c.neutral[k].y);
		for (int k = 0; k != c.n; ++k) if (c.mine[k].type == T_REFINERY || c.mine[k].type == P_ASSIM || c.mine[k].type == Z_EXTRACTOR) lane(c.mine[k].x, c.mine[k].y);
	}
	return true;
}

// Is the footprint (and its margin) free? `g` = tiles of clearance to other buildings.
static bool spot_ok(const plan_ctx& c, int tx, int ty, int w, int h, int type, int g) {
	if (tx < 0 || ty < 0 || tx + w > c.W || ty + h > c.H) return false;
	bool zerg = c.b->race == R_ZERG;
	for (int y = ty; y != ty + h; ++y) for (int x = tx; x != tx + w; ++x) {
		uint8_t v = g_grid[y * c.W + x];
		if (v & (G_BLOCK | G_BLD | G_RES | G_UNEXPL)) return false;
		if (zerg ? !(v & G_CREEP) : (v & G_CREEP)) return false;
	}
	for (int y = ty - g; y != ty + h + g; ++y) for (int x = tx - g; x != tx + w + g; ++x) {
		if (x >= tx && x < tx + w && y >= ty && y < ty + h) continue;
		uint8_t v = g_get(c, x, y);
		if (!(v & G_BLD) || x < 0 || y < 0 || x >= c.W || y >= c.H) continue;
		// depots may pair up with ONE other, not-yet-paired depot; nothing else touches
		if (type == T_DEPOT && (v & G_DEPOT) && !(v & G_PAIRED)) continue;
		return false;
	}
	return true;
}

struct cand_t { int s, tx, ty; };
static const int KCAND = 24;
static void cand_push(cand_t* k, int& nk, int s, int tx, int ty) {
	if (nk == KCAND && s <= k[nk - 1].s) return;
	int i = nk < KCAND ? nk++ : KCAND - 1;
	while (i > 0 && k[i - 1].s < s) { k[i] = k[i - 1]; --i; }
	k[i] = cand_t{s, tx, ty};
}

// Place `type` in a base by role. Returns 1 when the host accepted a build order.
static int plan_build(plan_ctx& c, intptr_t worker, int type) {
	bot_t* b = c.b;
	int w, h; if (!bld_size(type, w, h)) return 0;
	int role = role_of(type);
	int g = (role == ROLE_DEFENCE) ? 0 : 1;
	bool protoss_powered = b->race == R_PROTOSS && type != P_PYLON;
	cand_t k[KCAND]; int nk = 0;
	// static defence belongs at the FRONT base: the completed town hall nearest the enemy side
	int front_base = -1; { int64_t best = 0;
		for (int a = 0; a != c.n; ++a) if (is_base(c.mine[a].type) && c.mine[a].completed) {
			int64_t proj = (int64_t)(c.mine[a].x / 32 - c.hx) * c.fx + (int64_t)(c.mine[a].y / 32 - c.hy) * c.fy;
			if (front_base < 0 || proj > best) { front_base = a; best = proj; }
		} }
	// anchors: the main first; other bases only matter for Zerg creep / a pylon per expansion
	for (int a = 0; a != c.n; ++a) {
		if (!is_base(c.mine[a].type) || !c.mine[a].completed) continue;
		int ax = c.mine[a].x / 32, ay = c.mine[a].y / 32;
		bool is_main = abs(ax - c.hx) <= 2 && abs(ay - c.hy) <= 2;
		int R = is_main ? 18 : (b->race == R_ZERG ? 10 : 9);
		if (!is_main && b->race != R_ZERG && role != ROLE_PYLON && role != ROLE_DEFENCE) continue;
		// pylon bookkeeping for this anchor
		bool anchor_has_pylon = false;
		if (role == ROLE_PYLON) for (int i = 0; i != c.n; ++i) if (c.mine[i].type == P_PYLON && d2(c.mine[i].x, c.mine[i].y, c.mine[a].x, c.mine[a].y) < (int64_t)288 * 288) { anchor_has_pylon = true; break; }
		for (int ty = ay - R; ty <= ay + R; ++ty) for (int tx = ax - R; tx <= ax + R; ++tx) {
			if (!spot_ok(c, tx, ty, w, h, type, g)) continue;
			int dx = 2 * tx + w - 2 * ax, dy = 2 * ty + h - 2 * ay;          // half tiles from the anchor centre
			int len = (int)sqrt((double)(dx * dx + dy * dy)); if (len < 1) len = 1;
			int d10 = len * 5;                                                // tiles x10
			int dotM = (dx * c.mx + dy * c.my) / len / 10;                    // -100..100 (mineral side > 0)
			int dotF = (dx * c.fx + dy * c.fy) / len / 10;                    // front side > 0
			int s = 0;
			if (role == ROLE_PYLON) {
				int mind = 99;
				for (int i = 0; i != c.n; ++i) if (c.mine[i].type == P_PYLON) {
					int px = c.mine[i].x / 32 - 1, py = c.mine[i].y / 32 - 1;
					int ddx = px - tx, ddy = py - ty; int d = (int)sqrt((double)(ddx * ddx + ddy * ddy));
					if (d < mind) mind = d;
				}
				s = (mind > 8 ? 8 : mind) * 30;
				if (mind < 5) s -= 300;                    // never next to another pylon
				s -= abs(d10 - 70);                        // ~7 tiles from the town hall
				if (dotM > 0) s -= dotM * 2;
				if (!is_main) s += anchor_has_pylon ? -150 : 250;   // one pylon per expansion first
			} else {
				int ideal = role == ROLE_PROD ? 80 : role == ROLE_SUPPLY ? 90 : 60;
				s = -abs(d10 - ideal) * 2;
				if (role != ROLE_DEFENCE && dotM > 0) s -= dotM * 2;
				if (role == ROLE_PROD) s += dotF * 6 / 10;
				else if (role == ROLE_DEFENCE) s += dotF;
				else if (role == ROLE_SUPPLY) s -= dotF * 6 / 10;
				// rows and columns: snap to a lattice anchored on the town hall
				int px = w + (role == ROLE_SUPPLY ? 0 : 1) + (has_addon_slot(type) ? 2 : 0) + (role == ROLE_PROD ? 1 : 0);
				int py = h + 1 + (role == ROLE_PROD ? 1 : 0);
				if (role != ROLE_DEFENCE) {
					if ((((tx - c.btlx) % px) + px) % px == 0) s += 25;
					if ((((ty - c.btly) % py) + py) % py == 0) s += 25;
				}
				if (protoss_powered) {
					// inside a completed pylon's field? (16x10-tile ellipse; the host has the last word)
					bool pw = false;
					for (int i = 0; i != c.n && !pw; ++i) if (c.mine[i].type == P_PYLON && c.mine[i].completed) {
						int ddx = (2 * tx + w) * 16 - c.mine[i].x, ddy = (2 * ty + h) * 16 - c.mine[i].y;
						if ((int64_t)ddx * ddx * 25 + (int64_t)ddy * ddy * 64 <= (int64_t)256 * 256 * 25) pw = true;
					}
					if (!pw) continue;
				}
				if (role == ROLE_DEFENCE) { if (a != front_base) s -= 150; }
				else if (!is_main) s -= 100;
			}
			cand_push(k, nk, s, tx, ty);
		}
	}
	for (int i = 0; i != nk; ++i) if (b->api->build(b->h, worker, type, k[i].tx, k[i].ty)) return 1;
	return 0;
}

// Town-hall spot at a mineral cluster: the free tile that minimises the total
// walk to the cluster's minerals and geyser (what a player eyeballs), keeping
// BW's 3-tile resource clearance. Returns 1 placed, 0 no legal spot, -1 the
// site is still unexplored (caller scouts it).
static int plan_expansion(plan_ctx& c, intptr_t worker, int type, int cx, int cy) {
	bot_t* b = c.b;
	int ctx = cx / 32, cty = cy / 32;
	int rx[40], ry[40], nr = 0;
	for (int i = 0; i != c.nn && nr != 40; ++i) {
		int t = c.neutral[i].type;
		if (!(is_mineral(t) || t == N_GEYSER)) continue;
		if (d2(c.neutral[i].x, c.neutral[i].y, cx, cy) > (int64_t)(t == N_GEYSER ? 448 : 320) * (t == N_GEYSER ? 448 : 320)) continue;
		rx[nr] = c.neutral[i].x; ry[nr] = c.neutral[i].y; ++nr;
	}
	if (!nr) return 0;
	cand_t k[KCAND]; int nk = 0; int unexplored = 0, seen = 0;
	for (int ty = cty - 11; ty <= cty + 11; ++ty) for (int tx = ctx - 11; tx <= ctx + 11; ++tx) {
		if (tx < 0 || ty < 0 || tx + 4 > c.W || ty + 3 > c.H) continue;
		bool bad = false, unex = false;
		for (int y = ty; y != ty + 3 && !bad; ++y) for (int x = tx; x != tx + 4; ++x) {
			uint8_t v = g_grid[y * c.W + x];
			if (v & (G_BLOCK | G_BLD)) { bad = true; break; }
			if (v & G_UNEXPL) unex = true;
		}
		if (bad) continue;
		// BW rule: no town hall within 3 tiles of a mineral field / geyser
		for (int y = ty - 3; y != ty + 6 && !bad; ++y) for (int x = tx - 3; x != tx + 7; ++x) {
			if (x < 0 || y < 0 || x >= c.W || y >= c.H) continue;
			if (g_grid[y * c.W + x] & G_BLD) { bad = true; break; }
		}
		if (bad) continue;
		++seen;
		if (unex) { ++unexplored; continue; }
		int px = tx * 32 + 64, py = ty * 32 + 48; int64_t sum = 0;
		for (int i = 0; i != nr; ++i) sum += (int64_t)sqrt((double)d2(px, py, rx[i], ry[i]));
		cand_push(k, nk, -(int)(sum / nr), tx, ty);
	}
	for (int i = 0; i != nk; ++i) if (b->api->build(b->h, worker, type, k[i].tx, k[i].ty)) return 1;
	if (seen && unexplored * 2 >= seen) return -1;
	return nk ? 0 : (unexplored ? -1 : 0);
}

extern "C" void openbw_bot_frame(void* p) {
	bot_t* b = (bot_t*)p;
	const ob_host_api* api = b->api;
	ob_bot_host* h = b->h;
	int32_t frame = api->current_frame(h);
	const level_t* L = b->L;
	if (frame % L->tick != 0) return;
	// Idle handicap: the easiest levels simply skip a share of their decision
	// ticks (a distracted player). Bot-side LCG, nothing to do with the sim's RNG.
	if (L->handicap_idle) { b->rng = b->rng * 1664525u + 1013904223u; if ((int)((b->rng >> 16) % 100) < L->handicap_idle) return; }
	b->orders_left = L->max_orders;   // army order budget for this tick (0 = unlimited)

	int32_t me = api->my_player(h);
	static ob_unit mine[700], neutral[700], theirs[700];
	int32_t n = api->units(h, me, mine, 700);
	int32_t nn = api->units(h, 11, neutral, 700);
	int32_t enemy = (api->abi_version >= 2 && api->enemy_player) ? api->enemy_player(h) : -1;
	int32_t ne = enemy >= 0 ? api->units(h, enemy, theirs, 700) : 0;

	int32_t minerals = api->minerals(h, me), gas = api->gas(h, me);
	int32_t su = api->supply_used(h, me), sa = api->supply_available(h, me);
	int supply_full = su / 2;
	bool v2 = api->abi_version >= 2 && api->research;

	static census_t M, E;
	census(mine, n, M, true);
	census(theirs, ne, E, false);

	// ---- race detection (once) + opening selection -------------------------
	if (b->race == R_UNKNOWN) {
		for (int i = 0; i != n; ++i) { int r = race_of_base(mine[i].type); if (r >= 0) { b->race = r; break; } }
		if (b->race == R_UNKNOWN) {   // no base yet? infer from the worker
			for (int i = 0; i != n; ++i) { int t = mine[i].type; if (t == T_SCV) b->race = R_TERRAN; else if (t == P_PROBE) b->race = R_PROTOSS; else if (t == Z_DRONE) b->race = R_ZERG; if (b->race >= 0) break; }
		}
	}
	if (b->enemy_race == R_UNKNOWN && ne) {
		for (int i = 0; i != ne; ++i) { int r = race_of_base(theirs[i].type); if (r >= 0) { b->enemy_race = r; break; } }
		if (b->enemy_race == R_UNKNOWN)
			for (int i = 0; i != ne; ++i) { int t = theirs[i].type; if (t == T_SCV) b->enemy_race = R_TERRAN; else if (t == P_PROBE) b->enemy_race = R_PROTOSS; else if (t == Z_DRONE) b->enemy_race = R_ZERG; if (b->enemy_race >= 0) break; }
	}
	if (b->race == R_UNKNOWN) {
		if (!b->warned_no_base && frame > 240) { b->warned_no_base = true; api->log(h, "UnifiedAI: no base/worker found -- cannot detect race"); }
		return;
	}
	if (!b->opening || (!b->announced && b->enemy_race != R_UNKNOWN)) {
		b->opening = pick_opening(b->race, b->enemy_race, L->opening_kind);
		b->bo_done = 0;
		if (b->enemy_race != R_UNKNOWN || frame > 600) {
			b->announced = true;
			char s[192];
			const char* op = L->opening_kind < 2 ? (L->opening_kind == 0 ? "generic (slow)" : "generic")
			       : b->race == R_TERRAN ? (b->enemy_race == R_PROTOSS ? "factory/tanks" : b->enemy_race == R_TERRAN ? "bio+tank" : "bio+academy")
			       : b->race == R_PROTOSS ? (b->enemy_race == R_TERRAN ? "1gate core->DT" : b->enemy_race == R_PROTOSS ? "2gate mix" : "2gate zealot+forge")
			       : (b->enemy_race == R_PROTOSS ? "12hatch hydra" : b->enemy_race == R_ZERG ? "9pool" : "12hatch spire");
			snprintf(s, sizeof s, "UnifiedAI L%d (%s): I am %s vs %s -> opening %s", b->level_id, LEVEL_NAMES[b->level_id], race_name(b->race), race_name(b->enemy_race), op);
			api->log(h, s);
		}
	}

	if (b->home_x < 0 && M.base) { b->home_x = M.base->x; b->home_y = M.base->y; }
	ob_unit* base = nullptr;
	{
		int64_t bd = -1;
		for (int i = 0; i != n; ++i) if (is_base(mine[i].type) && mine[i].completed) { int64_t d = d2(mine[i].x, mine[i].y, b->home_x, b->home_y); if (!base || d < bd) { base = &mine[i]; bd = d; } }
	}
	if (!base) { if (!b->warned_no_base && frame > 240) { b->warned_no_base = true; api->log(h, "UnifiedAI: base lost"); } return; }
	int32_t ctx = base->x / 32, cty = base->y / 32;

	// ---- situational flags ---------------------------------------------------
	// rush: enemy army near our base early; enemy_expanded: >1 base; enemy_air.
	// Each flag must PERSIST for react_delay frames before we act on it -- the
	// slower levels notice things late; hard reacts at once.
	auto delayed = [&](bool raw, int32_t& since) -> bool {
		if (!raw) { since = -1; return false; }
		if (since < 0) since = frame;
		return frame - since >= L->react_delay;
	};
	b->my_army = M.army; b->enemy_army = E.army;
	b->enemy_air = delayed(E.air > 0, b->air_since);
	b->enemy_expanded = delayed(E.bases > 1, b->exp_since);
	{
		int near = 0;
		for (int i = 0; i != ne; ++i) if (is_army(theirs[i].type) && d2(theirs[i].x, theirs[i].y, base->x, base->y) < (int64_t)640 * 640) ++near;
		b->rush = delayed(near >= 2 || (near >= 1 && frame < 4000), b->rush_since);
		b->tel_near = near;
	}
	// Army orders honour the per-tick budget (a limited player can't re-task every
	// unit at once); economy orders never count against it.
	auto amove = [&](intptr_t id, int32_t x, int32_t y) {
		if (L->max_orders && b->orders_left <= 0) return;
		api->order_attack_move(h, id, x, y);
		if (L->max_orders) --b->orders_left;
	};
	if (b->base_hp_last >= 0 && M.base_hp < b->base_hp_last - 8) b->defend_until = frame + 720;
	b->base_hp_last = M.base_hp;

	// ---- expansion site (needed by the worker rules below) --------------------
	int32_t exp_x = 0, exp_y = 0;
	int32_t est_x = 0, est_y = 0;
	bool have_enemy_start = api->enemy_start(h, &est_x, &est_y) != 0;
	collect_clusters(mine, n, theirs, ne, neutral, nn);
	bool have_exp = find_expansion(b, frame, base->x, base->y, est_x, est_y, have_enemy_start, exp_x, exp_y) != 0;

	// ---- 1. workers: idle -> gas (staffed by worker count) else nearest mineral --
	int wt = b->race == R_TERRAN ? T_SCV : b->race == R_PROTOSS ? P_PROBE : Z_DRONE;
	int gt = b->race == R_TERRAN ? T_REFINERY : b->race == R_PROTOSS ? P_ASSIM : Z_EXTRACTOR;
	int on_gas = 0;
	// The whole gas cycle counts (81 MoveToGas .. 84 ReturnGas) -- counting only two
	// of its orders under-counted the crew, so every idle worker kept being added
	// (measured: 6 on one refinery, each vanishing into it in turn = phantom losses).
	for (int i = 0; i != n; ++i) if (mine[i].type == wt && mine[i].completed && mine[i].order >= 81 && mine[i].order <= 84) ++on_gas;
	// Gas staffing grows with the economy: 3 per geyser only once the mineral
	// line can spare them. With ~10 workers, 3 on gas starved minerals to ~0
	// while gas piled up (measured L3 TvP: min 2-48, gas 512, factory step
	// never affordable/actionable).
	int gas_per = M.workers >= 16 ? 3 : M.workers >= 12 ? 2 : M.workers >= 9 ? 1 : 0;
	// ...and shrinks when gas piles up while minerals are short (measured: gas 1204,
	// minerals 27 -- three probes kept mining gas nobody could spend). Surplus gas
	// workers on their way TO the geyser (81 MoveToGas / 82 WaitForGas) go back to
	// the mineral line; one inside the refinery or carrying gas finishes its trip.
	if (gas > 400 && minerals < 300 && gas_per > 1) gas_per = 1;
	if (gas > 600 && minerals < 300) gas_per = 0;
	if (M.done[gt] && on_gas > M.done[gt] * gas_per) {
		int surplus = on_gas - M.done[gt] * gas_per;
		for (int i = 0; i != n && surplus > 0; ++i) {
			ob_unit& u = mine[i];
			if (u.type != wt || !u.completed || (u.order != 81 && u.order != 82)) continue;
			ob_unit* best = nullptr; int64_t bd = -1;
			for (int k = 0; k != nn; ++k) if (is_mineral(neutral[k].type)) { int64_t d = d2(u.x, u.y, neutral[k].x, neutral[k].y); if (!best || d < bd) { best = &neutral[k]; bd = d; } }
			if (best) { api->order_right_click(h, u.id, best->id); --surplus; --on_gas; }
		}
	}
	// Under-staffed gas is filled from the MINERAL line, not only from idle workers.
	// (Zerg drones are never idle and, while the army comes first, no new ones are
	// made: measured Z3 -- extractor up at 3:30, gas 0 until 6:00, the hydralisk
	// den step starved and timed out, 53 zerglings and one hydralisk at 11 minutes.)
	if (M.done[gt] && on_gas < M.done[gt] * gas_per) {
		int need = M.done[gt] * gas_per - on_gas;
		for (int k = 0; k != n && need > 0; ++k) {
			if (mine[k].type != gt || !mine[k].completed) continue;
			// nearest workers walking to / mining minerals (85, 87): not carrying, not building
			for (int r = 0; r != gas_per && need > 0; ++r) {
				ob_unit* pick = nullptr; int64_t bd = -1;
				for (int i = 0; i != n; ++i) {
					ob_unit& u = mine[i];
					if (u.type != wt || !u.completed || (u.order != 85 && u.order != 87)) continue;
					int64_t d = d2(u.x, u.y, mine[k].x, mine[k].y);
					if (d < (int64_t)480 * 480 && (!pick || d < bd)) { pick = &u; bd = d; }
				}
				if (!pick) break;
				api->order_right_click(h, pick->id, mine[k].id);
				pick->order = 81; ++on_gas; --need;
			}
		}
	}
	for (int i = 0; i != n; ++i) {
		ob_unit& u = mine[i];
		if (u.type != wt || !u.completed || !u.idle) continue;
		// A worker that went idle FAR from home (the scout arriving at the enemy
		// base) must walk back, not start mining the nearest -- enemy -- minerals.
		// The EXPANSION scout idling at its site is the exception: the placer will
		// use it there next tick (sending it home would ping-pong it forever).
		if (d2(u.x, u.y, base->x, base->y) > (int64_t)1200 * 1200) {
			if (have_exp && frame - b->want_expand_frame < 240 && d2(u.x, u.y, exp_x, exp_y) < (int64_t)400 * 400) continue;
			api->order_move(h, u.id, base->x, base->y); continue;
		}
		if (M.done[gt] && on_gas < M.done[gt] * gas_per) {
			for (int k = 0; k != n; ++k) if (mine[k].type == gt && mine[k].completed) { api->order_right_click(h, u.id, mine[k].id); ++on_gas; break; }
			continue;
		}
		// Mine at the nearest OWN base, not at whatever mineral is nearest to the
		// worker: a scout that went idle at the natural used to start long-distance
		// mining its (base-less) mineral line.
		ob_unit* nb = base; int64_t nbd = d2(u.x, u.y, base->x, base->y);
		for (int k = 0; k != n; ++k) if (is_base(mine[k].type) && mine[k].completed) { int64_t d = d2(u.x, u.y, mine[k].x, mine[k].y); if (d < nbd) { nb = &mine[k]; nbd = d; } }
		ob_unit* best = nullptr; int64_t bd = -1;
		for (int k = 0; k != nn; ++k) {
			if (!is_mineral(neutral[k].type) || d2(nb->x, nb->y, neutral[k].x, neutral[k].y) > (int64_t)320 * 320) continue;
			int64_t d = d2(u.x, u.y, neutral[k].x, neutral[k].y);
			if (!best || d < bd) { best = &neutral[k]; bd = d; }
		}
		if (!best) for (int k = 0; k != nn; ++k) {   // that base is mined out: anything
			if (!is_mineral(neutral[k].type)) continue;
			int64_t d = d2(u.x, u.y, neutral[k].x, neutral[k].y);
			if (!best || d < bd) { best = &neutral[k]; bd = d; }
		}
		if (best) api->order_right_click(h, u.id, best->id);
	}
	// Worker transfer ("maynard"): a fresh expansion with an empty mineral line
	// gets a share of the main's workers instead of waiting for its own.
	// (Not while the enemy army is the bigger one or is in a base: the transfer
	// kept feeding drones to the zealots standing in the natural.)
	if (L->scout >= 1 && M.bases >= 2 && frame - b->last_transfer_frame >= 720 && b->tel_threat == 0 && E.str <= M.str) {
		ob_unit* poor = nullptr; int poor_n = 0, rich_n = 0; ob_unit* rich = nullptr;
		for (int k = 0; k != n; ++k) {
			if (!is_base(mine[k].type) || !mine[k].completed) continue;
			int cnt = 0, patches = 0;
			for (int i = 0; i != n; ++i) if (mine[i].type == wt && mine[i].completed && d2(mine[i].x, mine[i].y, mine[k].x, mine[k].y) < (int64_t)352 * 352) ++cnt;
			for (int i = 0; i != nn; ++i) if (is_mineral(neutral[i].type) && d2(neutral[i].x, neutral[i].y, mine[k].x, mine[k].y) < (int64_t)320 * 320) ++patches;
			if (patches >= 4 && (!poor || cnt < poor_n)) { poor = &mine[k]; poor_n = cnt; }
			if (!rich || cnt > rich_n) { rich = &mine[k]; rich_n = cnt; }
		}
		if (poor && rich && poor != rich && rich_n - poor_n >= 8) {
			ob_unit* patch = nullptr; int64_t pd = -1;
			for (int i = 0; i != nn; ++i) if (is_mineral(neutral[i].type)) { int64_t d = d2(neutral[i].x, neutral[i].y, poor->x, poor->y); if (d < (int64_t)320 * 320 && (!patch || d < pd)) { patch = &neutral[i]; pd = d; } }
			int moved = 0, quota = (rich_n - poor_n) / 2; if (quota > 6) quota = 6;
			for (int i = 0; i != n && patch && moved < quota; ++i) {
				ob_unit& u = mine[i];
				// mineral workers only (85 MoveToMinerals .. 87 MiningMinerals): not carrying, not gas, not building
				if (u.type != wt || !u.completed || u.order < 85 || u.order > 87) continue;
				if (d2(u.x, u.y, rich->x, rich->y) > (int64_t)352 * 352) continue;
				api->order_right_click(h, u.id, patch->id); ++moved;
			}
			if (moved) { b->last_transfer_frame = frame; char s[96]; snprintf(s, sizeof s, "UA:transfer %d workers to the new base (%d vs %d)", moved, rich_n, poor_n); api->log(h, s); }
		}
	}

	// ---- expansion site + a DEDICATED scout ----------------------------------
	// The expansion worker must not be the home builder: while it walks to the
	// (unexplored) site the reactive macro keeps placing depots/rax at home with
	// "the first worker", which recalled it every cooldown so it never arrived
	// (Terran run: two "scouting" logs, no "expanding"). Unit handles are only
	// valid within a frame, so identity is kept by POSITION: the scout is the
	// worker nearest the site -- the same worker once it starts walking -- and
	// home builds skip it. (exp_x/exp_y were computed above, before the worker rules.)
	// A worker already walking to place / constructing a building (BW orders 30
	// PlaceBuilding .. 33 ConstructingBuilding) must never be the builder OR the
	// scout: a new order re-targets a placing SCV (it never starts) and makes a
	// constructing SCV ABANDON its building (measured L3 TvP: the refinery's SCV was
	// the nearest to the expansion site, got sent scouting, the refinery never
	// finished, gas stayed 0, the factory was never affordable). A worker in the
	// gas cycle (81..84, HarvestGas=83 is INSIDE the refinery) can't take orders
	// either. Mineral workers / idle only.
	auto worker_busy = [](const ob_unit& u) { return (u.order >= 30 && u.order <= 33) || (u.order >= 81 && u.order <= 84); };
	ob_unit* scout = nullptr;
	if (have_exp) {
		int64_t bd = -1;
		for (int i = 0; i != n; ++i) if (mine[i].type == wt && mine[i].completed && !worker_busy(mine[i])) { int64_t d = d2(mine[i].x, mine[i].y, exp_x, exp_y); if (!scout || d < bd) { scout = &mine[i]; bd = d; } }
	}
	// ---- builder + cooldown ------------------------------------------------
	bool cooldown = frame - b->last_build_frame < 96;
	ob_unit* builder = nullptr; int wct = 0;
	for (int i = 0; i != n; ++i) if (mine[i].type == wt && mine[i].completed) { ++wct; if (!builder && &mine[i] != scout && !worker_busy(mine[i])) builder = &mine[i]; }
	if (b->race == R_ZERG && wct <= 4) builder = nullptr;   // a drone is consumed by the morph
	// Worker scout (levels with scout >= 2): one mineral worker goes to the enemy
	// start early; the idle rule above walks it home once it arrives. (Its intel is
	// what the pseudo-fog layer will consume; today it is honest behaviour first.)
	// Early natural scout (scout >= 1): BW cannot place a building on unexplored
	// tiles, so a bot that never looks at its natural can only start exploring it
	// when the money for the town hall is already banked -- minutes late. One
	// worker walks there around 10 supply, then goes back to mining (idle rule).
	if (L->scout >= 1 && b->race != R_ZERG && have_exp && scout && wct >= 9 && b->nat_scout_frame < 0 && M.bases == 1) {
		api->order_move(h, scout->id, exp_x, exp_y);
		b->nat_scout_frame = frame;
		api->log(h, "UA:scout sent to the natural expansion");
	}
	if (L->scout >= 2 && !b->scout_sent && frame >= 1440 && wct >= 8) {
		int32_t ex, ey;
		if (api->enemy_start(h, &ex, &ey)) {
			for (int i = 0; i != n; ++i) {
				if (mine[i].type != wt || !mine[i].completed || &mine[i] == scout || &mine[i] == builder || worker_busy(mine[i])) continue;
				api->order_move(h, mine[i].id, ex, ey);
				b->scout_sent = true; b->scout_sent_frame = frame;
				api->log(h, "UA:scout sent to the enemy start");
				break;
			}
		}
	}
	// pending bookkeeping (see bot_t): cleared once the building appears or times out
	if (b->pending_type >= 0 && (frame - b->pending_since >= 600 || M.cnt[b->pending_type] > b->pending_cnt)) b->pending_type = -1;
	auto mark_placed = [&](int type) { b->last_build_frame = frame; cooldown = true; b->pending_type = type; b->pending_since = frame; b->pending_cnt = M.cnt[type]; };
	// cost of the buildings we may place (minerals, gas) -- gate placement on it
	auto can_afford = [&](int type) -> bool {
		switch (type) {
		case T_DEPOT: case P_PYLON: case T_BARRACKS: case P_GATEWAY: case T_ACADEMY: case P_FORGE: return minerals >= 150;
		case T_REFINERY: case P_ASSIM: return minerals >= 100;
		case Z_EXTRACTOR: return minerals >= 50;
		case T_EBAY: return minerals >= 125;
		case T_FACTORY: return minerals >= 200 && gas >= 100;
		case T_STARPORT: return minerals >= 150 && gas >= 100;
		case T_SCIENCE_FACILITY: return minerals >= 100 && gas >= 150;
		case P_ROBO: return minerals >= 200 && gas >= 200;
		case P_OBSERVATORY: return minerals >= 50 && gas >= 100;
		case T_MACHINE_SHOP: return minerals >= 50 && gas >= 50;
		case T_BUNKER: return minerals >= 100;
		case T_TURRET: return minerals >= 75;
		case P_CORE: return minerals >= 200;
		case P_CANNON: return minerals >= 150;
		case P_CITADEL: return minerals >= 150 && gas >= 100;
		case P_ARCHIVES: return minerals >= 150 && gas >= 200;
		case Z_POOL: return minerals >= 200;
		case Z_HYDRA_DEN: return minerals >= 100 && gas >= 50;
		case Z_EVO: return minerals >= 75;
		case Z_LAIR: return minerals >= 150 && gas >= 100;
		case Z_SPIRE: return minerals >= 200 && gas >= 150;
		case T_CC: return minerals >= 400;
		case P_NEXUS: return minerals >= 400;
		case Z_HATCH: return minerals >= 300;
		default: return minerals >= 150;
		}
	};
	// gas part of a building's price (0 = minerals only) -- drives "refinery first"
	auto gas_cost = [](int type) -> int {
		switch (type) {
		case T_FACTORY: case P_CITADEL: case Z_LAIR: case T_STARPORT: case P_OBSERVATORY: return 100;
		case T_SCIENCE_FACILITY: return 150;
		case P_ROBO: return 200;
		case T_MACHINE_SHOP: case Z_HYDRA_DEN: return 50;
		case P_ARCHIVES: return 200;
		case Z_SPIRE: return 150;
		default: return 0;
		}
	};
	// The planner's view of the map, built at most once per tick and only when
	// something is actually placed.
	plan_ctx pc; bool grid_tried = false, grid_ok = false;
	auto ensure_grid = [&]() -> bool {
		if (grid_tried) return grid_ok;
		grid_tried = true;
		pc.b = b; pc.mine = mine; pc.n = n; pc.theirs = theirs; pc.ne = ne; pc.neutral = neutral; pc.nn = nn;
		pc.hx = base->x / 32; pc.hy = base->y / 32;
		pc.btlx = (base->x - 64) / 32; pc.btly = (base->y - 48) / 32;
		// front = toward the enemy start (map centre when unknown)
		int32_t tx = have_enemy_start ? est_x : b->map_tw * 16, ty = have_enemy_start ? est_y : b->map_th * 16;
		double fl = sqrt((double)d2(tx, ty, base->x, base->y)); if (fl < 1) fl = 1;
		pc.fx = (int)((tx - base->x) * 1000 / fl); pc.fy = (int)((ty - base->y) * 1000 / fl);
		// mineral side = toward the centroid of the main's mineral line
		int64_t sx = 0, sy = 0; int cnt = 0;
		for (int k = 0; k != nn; ++k) if (is_mineral(neutral[k].type) && d2(neutral[k].x, neutral[k].y, base->x, base->y) < (int64_t)320 * 320) { sx += neutral[k].x; sy += neutral[k].y; ++cnt; }
		pc.mx = pc.my = 0;
		if (cnt) {
			double mxd = (double)sx / cnt - base->x, myd = (double)sy / cnt - base->y, ml = sqrt(mxd * mxd + myd * myd);
			if (ml >= 1) { pc.mx = (int)(mxd * 1000 / ml); pc.my = (int)(myd * 1000 / ml); }
		}
		grid_ok = build_grid(pc);
		return grid_ok;
	};
	auto place = [&](int type, int rmax) -> bool {
		if (b->pending_type == type) return false;   // in flight -- never re-order it
		if (is_base(type)) {   // expansion: best free mineral cluster, via the scout
			b->want_expand_frame = frame;
			// Once a depot build is accepted the worker still has to WALK there before
			// the new building shows up in our unit list; until then the BO step looks
			// unmet. Don't re-issue for ~25 s (a re-order could restart a morph / a CC
			// under construction) -- if the worker died on the way we retry after that.
			if (frame - b->expand_issued < b->expand_wait) return false;
			if (!have_exp || !scout) {
				if (frame - b->last_expand_log > 720) { b->last_expand_log = frame; api->log(h, "expand: no free mineral cluster"); }
				return false;
			}
			// Track the site we are trying to take: an accepted order that never
			// became a base (worker killed, blocked) or a site that will not reveal
			// (island) is blacklisted for a while and the next-best cluster is used.
			if (b->exp_site_since < 0 || d2(exp_x, exp_y, b->exp_site_x, b->exp_site_y) > (int64_t)160 * 160) { b->exp_site_x = exp_x; b->exp_site_y = exp_y; b->exp_site_since = frame; b->exp_fail = 0; }
			if (b->expand_issued > b->exp_site_since && M.bases <= b->bases_at_issue) {
				b->exp_site_since = b->expand_issued + 1;   // count each failed issue once
				if (++b->exp_fail >= 2) { site_blacklist(b, exp_x, exp_y, frame + 4800); api->log(h, "expand: site failed twice -> trying another"); b->exp_site_since = -1; return false; }
			}
			if (b->exp_fail == 0 && frame - b->exp_site_since > 2400 && b->expand_issued < b->exp_site_since) {
				site_blacklist(b, exp_x, exp_y, frame + 7200); api->log(h, "expand: site never revealed (island?) -> trying another"); b->exp_site_since = -1; return false;
			}
			// Only place once we can pay; the scout below goes ahead REGARDLESS, so the
			// site is already revealed by the time the minerals are banked.
			int res = -1;
			if (can_afford(type)) {
				if (ensure_grid()) { res = plan_expansion(pc, scout->id, type, exp_x, exp_y); if (res == 1) ++b->tel_plan; }
				// ring fallback: outward from the cluster centre (r starts at 3: the host
				// enforces BW's resource-distance rule)
				if (res != 1 && try_build(b, scout->id, type, exp_x / 32, exp_y / 32, 3, 12, 1)) { res = 1; ++b->tel_ring; }
				if (res == 1) {
					// the worker has to WALK there: wait in proportion to the distance before
					// calling the order failed (a far site took > 600 frames and was re-issued,
					// then blacklisted, while its worker was still on the way)
					b->expand_wait = 480 + (int32_t)(sqrt((double)d2(scout->x, scout->y, exp_x, exp_y)) * 0.5);
					mark_placed(type); b->expand_issued = frame; b->bases_at_issue = M.bases; api->log(h, "expanding"); return true;
				}
			}
			// The site is still in the fog. BW forbids placing a building on UNEXPLORED
			// tiles (can_place_building rejects tile.explored), and a fresh map's
			// expansions are black -- measured on the host (aidump zexpand): every free
			// cluster rejected, own base placeable; clearing the explored bit makes them
			// all placeable. Do what a player does: send the scout ahead so its sight
			// reveals the site; the retry places once explored. Its own timer keeps the
			// move from re-issuing every tick without touching the home builder's cooldown.
			if (frame - b->expand_moved >= 240) { api->order_move(h, scout->id, exp_x, exp_y); b->expand_moved = frame; }
			if (frame - b->last_expand_log > 720) {
				b->last_expand_log = frame;
				char s[96]; snprintf(s, sizeof s, "expand: site tile (%d,%d) not ready (fog / money) -> worker on its way", exp_x / 32, exp_y / 32); api->log(h, s);
			}
			return false;
		}
		if (!builder || cooldown) return false;
		int step = b->race == R_ZERG ? 1 : 2, rmin = b->race == R_ZERG ? 2 : 3;
		if (type == gt) {   // gas: a free geyser at one of our bases, the main's first
			ob_unit* gz = nullptr; int64_t gd = -1;
			for (int k = 0; k != nn; ++k) {
				if (neutral[k].type != N_GEYSER) continue;   // a geyser with a refinery on it is no longer in the neutral list
				int64_t near = -1;
				for (int i = 0; i != n; ++i) if (is_base(mine[i].type) && mine[i].completed) { int64_t d = d2(mine[i].x, mine[i].y, neutral[k].x, neutral[k].y); if (d < (int64_t)384 * 384 && (near < 0 || d < near)) near = d; }
				if (near < 0) continue;
				int64_t d = d2(base->x, base->y, neutral[k].x, neutral[k].y);
				if (!gz || d < gd) { gz = &neutral[k]; gd = d; }
			}
			if (gz && api->build(h, builder->id, type, gz->x / 32 - 2, gz->y / 32 - 1)) { mark_placed(type); return true; }
			return false;
		}
		if (ensure_grid() && plan_build(pc, builder->id, type)) { mark_placed(type); ++b->tel_plan; return true; }
		// Fallback: the old ring search (never less capable than before the planner).
		bool ok = false;
		if (b->race == R_PROTOSS && type != P_PYLON) {
			for (int i = 0; i != n && !ok; ++i) {
				if (mine[i].type != P_PYLON || !mine[i].completed) continue;
				if (try_build(b, builder->id, type, mine[i].x / 32, mine[i].y / 32, 2, 5, 1)) ok = true;
			}
		} else if (try_build(b, builder->id, type, ctx, cty, rmin, rmax, step)) ok = true;
		if (ok) { mark_placed(type); ++b->tel_ring; return true; }
		if (frame - b->last_place_log > 720) {   // DIAG: why did a home placement fail?
			b->last_place_log = frame;
			char s[128]; snprintf(s, sizeof s, "place: no room for type %d near base tile (%d,%d) r%d..%d step %d; builder order %d", type, ctx, cty, rmin, rmax, step, builder->order);
			api->log(h, s);
		}
		return false;
	};

	// ---- 3. rush defence: static defence + army-first -----------------------
	// rush_defense: 0 ignore (very easy), 1 army home only, 2+ static defence + worker pull
	if (b->rush && L->rush_defense > 0) {
		if (L->rush_defense >= 2) {
			if (b->race == R_TERRAN && M.done[T_BARRACKS] && M.cnt[T_BUNKER] == 0 && can_afford(T_BUNKER)) place(T_BUNKER, 8);
			if (b->race == R_PROTOSS && M.done[P_FORGE] && M.cnt[P_CANNON] < 2 && can_afford(P_CANNON)) place(P_CANNON, 8);
		}
	}
	// DEFEND: any enemy fighter near ANY of our bases is answered, rush or not.
	// Measured (botmatch T3 vs the native Protoss AI): one zealot at a time walked
	// into the mineral line and killed 20 SCVs over 4000 frames while the marines
	// stood at the barracks -- "rush" needs 2+ intruders, so nothing reacted.
	// The home army (not the units out on an attack) converges on the intruder;
	// with too few fighters the nearest workers gang up on it, as a player would.
	if (L->rush_defense > 0) {
		// Enemy WORKERS in our base count too. Play-test on the tablet: SCVs
		// walked into the bot's mineral line and killed probes 13 -> 4 while five
		// zealots stood at the staging point -- only "army" types were a threat, so a
		// worker rush / harassment (or a scout that starts hitting things) was
		// invisible. Each defender goes for the intruder nearest to IT and attacks
		// that unit directly (an attack-move at its old position chases air).
		ob_unit* threat = nullptr; int threat_n = 0; ob_unit* tbase = nullptr;
		static ob_unit* thr[48]; int nthr = 0;
		for (int i = 0; i != ne; ++i) {
			if (!(is_army(theirs[i].type) || is_worker(theirs[i].type)) || !theirs[i].completed) continue;
			for (int k = 0; k != n; ++k) if (is_base(mine[k].type) && d2(theirs[i].x, theirs[i].y, mine[k].x, mine[k].y) < (int64_t)544 * 544) {
				++threat_n; if (nthr < 48) thr[nthr++] = &theirs[i];
				if (!threat || (is_army(theirs[i].type) && !is_army(threat->type))) { threat = &theirs[i]; tbase = &mine[k]; }
				break;
			}
		}
		b->tel_threat = threat_n;
		if (threat) {
			for (int i = 0; i != n; ++i) {
				if (!is_army(mine[i].type) || !mine[i].completed || d2(mine[i].x, mine[i].y, tbase->x, tbase->y) >= (int64_t)1600 * 1600) continue;
				ob_unit* t = thr[0]; int64_t td = d2(mine[i].x, mine[i].y, t->x, t->y);
				for (int k = 1; k < nthr; ++k) { int64_t d = d2(mine[i].x, mine[i].y, thr[k]->x, thr[k]->y); if (d < td) { t = thr[k]; td = d; } }
				if (is_worker(t->type)) { if (L->max_orders == 0 || b->orders_left-- > 0) api->order_right_click(h, mine[i].id, t->id); }
				else amove(mine[i].id, t->x, t->y);
			}
			// (A single enemy WORKER is a scout: the mineral line does not chase it --
			// four drones did, every tick, and the economy stalled. Two or more
			// workers, or any fighter, is an attack.)
			if (L->rush_defense >= 2 && M.army < threat_n * 2 && threat_n <= 4 && (is_army(threat->type) || threat_n >= 2)) {
				int want = threat_n * 3 + 1, k = 0;
				for (int i = 0; i != n && k < want; ++i) {
					ob_unit& u = mine[i];
					if (u.type != wt || !u.completed || worker_busy(u) || d2(u.x, u.y, threat->x, threat->y) > (int64_t)320 * 320) continue;
					api->order_right_click(h, u.id, threat->id); ++k;
				}
			}
		}
	}
	// Zerg static defence: a sunken or two at the front once the pool is up (a
	// player's standard answer to early pressure), three under a rush. The colony
	// is placed by the planner (defence role = front side, on creep) and morphed
	// as soon as it finishes.
	if (b->race == R_ZERG && L->rush_defense >= 2 && M.done[Z_POOL]) {
		int want_sunk = b->rush ? 3 : (frame > 4500 ? (L->rush_defense >= 3 ? 2 : 1) : 0);
		for (int i = 0; i != n; ++i) if (mine[i].type == Z_CREEP_COLONY && mine[i].completed && minerals >= 50) api->train(h, mine[i].id, Z_SUNKEN);
		if (M.cnt[Z_CREEP_COLONY] + M.cnt[Z_SUNKEN] < want_sunk && minerals >= 75 && !cooldown) place(Z_CREEP_COLONY, 8);
	}
	// anti-air on enemy air: turrets / cannons (Zerg: hydras handle it)
	// (Dark Templar are answered the same way: turrets and cannons are the detectors.)
	if ((b->enemy_air || E.cnt[P_DT] > 0) && L->anti_air > 0 && !cooldown) {
		// the detector needs its tech building first
		if (b->race == R_TERRAN && M.cnt[T_EBAY] == 0 && M.done[T_BARRACKS] && can_afford(T_EBAY)) place(T_EBAY, 13);
		if (b->race == R_PROTOSS && M.cnt[P_FORGE] == 0 && M.done[P_GATEWAY] && can_afford(P_FORGE)) place(P_FORGE, 13);
		if (b->race == R_TERRAN && M.done[T_EBAY] && M.cnt[T_TURRET] < 3 && can_afford(T_TURRET)) place(T_TURRET, 9);
		if (b->race == R_PROTOSS && M.done[P_FORGE] && M.cnt[P_CANNON] < 3 && can_afford(P_CANNON)) place(P_CANNON, 9);
	}

	// ---- 4. the opening (supply-gated build order) ----------------------------
	// `saving`: a BO building (or a wanted expansion) we cannot yet afford. ARMY
	// production pauses while it is set so the minerals actually bank -- without
	// it the gateways/rax spent every mineral each tick and the 400-cost depot
	// step starved until it timed out (measured: Terran/Protoss never expanded,
	// minerals never crossed 400). Workers keep training; a rush overrides it.
	bool saving = false, saving_for_base = false, hold_workers = false;
	{
		bool early = false;
		int want = bo_next(b, M, supply_full, frame, early);
		b->bo_want = want; b->bo_afford = want >= 0 ? (int)can_afford(want) : -1; b->builder_order = builder ? builder->order : -1;
		if (want >= 0 && early) {
			// One supply early: bank for it now; for a SUPPLY step also pause worker
			// training so the depot/pylon/overlord lands before the block, not after.
			if (!can_afford(want)) { saving = true; if (want == T_DEPOT || want == P_PYLON || want == Z_OVERLORD) hold_workers = true; }
		} else if (want >= 0 && want != Z_OVERLORD) {
			if (b->pending_type != want && can_afford(want)) b->bo_stuck += L->tick;
			if (b->pending_type == want) {
				saving = true;                      // in flight: keep its minerals banked, don't re-order
			} else if (b->race == R_ZERG && want == Z_LAIR) {
				// a Lair is a MORPH of the hatchery, not a placement (gas-bound: no saving)
				if (M.done[Z_HATCH] && can_afford(Z_LAIR)) api->train(h, M.by_type[Z_HATCH]->id, Z_LAIR);
			} else if (want == T_MACHINE_SHOP) {
				// An addon needs the PlaceAddon order at the parent's addon tile (host
				// derives it from units.dat addon_position). The old build(...,0,0)
				// went through PlaceBuilding at tile (0,0), which can_place_building
				// always rejected -- so TvP/TvT never got a Machine Shop or tanks.
				// Only the v4 host has build_addon; on v3 the step times out as before.
				if (M.done[T_FACTORY] && can_afford(T_MACHINE_SHOP) && api->abi_version >= 4 && api->build_addon)
					api->build_addon(h, M.by_type[T_FACTORY]->id, T_MACHINE_SHOP);
				else if (!can_afford(T_MACHINE_SHOP)) saving = true;   // bank for it (measured: minerals drained <50 forever without this)
			} else if (is_base(want)) {
				place(want, 6);                     // scouts the site even before we can pay
				saving = !can_afford(want); saving_for_base = saving;
			} else if (can_afford(want)) {
				place(want, 13);
			} else if (gas_cost(want) > 0 && M.cnt[gt] == 0) {
				// The step needs gas and we have no gas building: banking minerals for
				// it is a deadlock (measured: factory 200/100 with gas 0 forever). Take
				// the refinery/assimilator/extractor first, like any player would.
				if (can_afford(gt)) place(gt, 0); else saving = true;
			} else {
				saving = true;                      // bank for the pending BO building
			}
		}
	}

	// ---- 2. supply ahead -- AFTER the opening decision: the build order owns
	// the money; a margin depot only jumps the queue when a block is imminent.
	// (Measured L3 TvP: a 14-supply margin fired a depot every time minerals hit
	// 100, so the refinery step never got its 100 and timed out -> no gas -> the
	// factory was un-affordable forever.)
	{
		int prod = M.done[T_BARRACKS] + M.done[T_FACTORY] + M.done[P_GATEWAY] + M.bases;
		int margin = L->supply_margin_base + prod * L->supply_margin_per_prod;   // easy levels supply-block themselves
		if (b->race != R_ZERG) {   // overlords come from larva (production below)
			int st = b->race == R_TERRAN ? T_DEPOT : P_PYLON;
			int up = M.cnt[st] - M.done[st];
			bool first_pylon = b->race == R_PROTOSS && M.cnt[P_PYLON] == 0;
			bool imminent = sa - su <= 4;
			// The full margin assumes every production building is actually producing.
			// When money is tight it is not -- and a margin depot then eats the very
			// minerals the units needed (measured T3 vs Protoss: 6 depots by 4:30, two
			// marines). So: within 3 supply of the cap a depot is URGENT; further out it
			// is only built from spare money.
			bool urgent = sa - su <= 6;
			bool comfortable = sa - su <= margin && minerals >= 250;
			if (sa < 400 && (first_pylon || ((urgent || comfortable) && up == 0)) && minerals >= 100 && (!saving || imminent)) place(st, 11);
		}
	}

	// ---- 4b. float valve: money piling up is spent on production capacity -------
	// (even mid-opening: a stuck tech step must not leave 2000 minerals idle --
	// measured Z3: 1300-2600 banked with every larva used and the Lair step pending)
	if (L->saving == 2 && minerals >= 600 && !cooldown && builder) {
		if (b->race == R_ZERG) {
			if (M.larvae == 0 && M.cnt[Z_HATCH] + M.cnt[Z_LAIR] + M.cnt[Z_HIVE] < 7 && b->pending_type != Z_HATCH
			    && ensure_grid() && plan_build(pc, builder->id, Z_HATCH)) { mark_placed(Z_HATCH); ++b->tel_plan; }
		} else if (b->race == R_TERRAN) {
			if (M.cnt[T_BARRACKS] < 6) place(T_BARRACKS, 13);
		} else if (M.done[P_PYLON] && M.cnt[P_GATEWAY] < 6) place(P_GATEWAY, 13);
	}

	// ---- 4c. the hydralisk den is not optional ---------------------------------
	// A build-order step that times out is skipped for good until the opening is
	// over; for the den that meant a whole game of zerglings (measured Z3 vs T3:
	// 25 lings, 59 drones, no den at 11 minutes). Past 3:30, with a pool and the
	// gas for it, the den goes down regardless of where the opening is.
	if (b->race == R_ZERG && L->tech_depth >= 1 && b->enemy_race != R_ZERG && frame > 5000 && M.done[Z_POOL] && M.cnt[Z_HYDRA_DEN] == 0
	    && b->pending_type != Z_HYDRA_DEN && !cooldown && can_afford(Z_HYDRA_DEN)) place(Z_HYDRA_DEN, 10);

	// Lurkers are gas (research 200, 100 each): one extractor had them arrive at 11
	// minutes. A second one as soon as the second hatchery stands.
	if (b->race == R_ZERG && L->micro >= 1 && b->enemy_race != R_ZERG && frame > 6000 && M.done[Z_POOL] && M.cnt[Z_EXTRACTOR] == 1
	    && M.done[Z_HATCH] + M.done[Z_LAIR] + M.done[Z_HIVE] >= 2 && M.workers >= 16 && gas < 200 && b->pending_type != Z_EXTRACTOR
	    && !cooldown && can_afford(Z_EXTRACTOR)) place(Z_EXTRACTOR, 0);

	// ---- 5. reactive macro once the opening is exhausted --------------------
	bool bo_over = b->opening && b->opening[b->bo_done].at_supply < 0;
	if (bo_over) {
		// Expansion has priority over more production buildings (an extra base is
		// the biggest macro lever); place() scouts/saves via the dedicated scout.
		int base_t = b->race == R_TERRAN ? T_CC : b->race == R_PROTOSS ? P_NEXUS : Z_HATCH;
		// Base count grows with the game (and the enemy's expansions), capped by the
		// level; the trigger is worker count OR floating minerals (Steamhammer's idea).
		int want_bases = 2 + (b->enemy_expanded ? 1 : 0) + (frame > 12000 ? 1 : 0) + (frame > 20000 ? 1 : 0);
		if (want_bases > L->max_bases) want_bases = L->max_bases;
		bool trigger = M.workers >= L->expand_min_workers || (L->expand_float > 0 && minerals >= L->expand_float);
		if (M.bases < want_bases && trigger) { place(base_t, 6); if (!can_afford(base_t)) { saving = true; saving_for_base = true; } }
	}
	if (bo_over && !cooldown) {
		int workers = M.workers;
		int td = L->tech_depth;   // 0 none, 1 core (academy/ebay/core/forge/den), 2 full (factory...), 3 full
		// a second gas once a second base is up and saturated enough to staff it
		int want_gas = (M.bases >= 2 && workers >= 24 && gas < 300) ? 2 : 1;
		if (b->race == R_TERRAN) {
			int want_rax = workers >= 10 ? (minerals > 300 ? 5 : 3) : 1;
			bool tanks = td >= 2 && (b->enemy_race == R_PROTOSS || b->enemy_race == R_TERRAN);
			if (tanks && M.cnt[T_FACTORY] < (minerals > 400 ? 3 : 2) && can_afford(T_FACTORY)) place(T_FACTORY, 13);
			else if (M.cnt[T_BARRACKS] < want_rax && can_afford(T_BARRACKS)) place(T_BARRACKS, 13);
			else if (M.cnt[T_REFINERY] < want_gas && can_afford(T_REFINERY)) place(T_REFINERY, 0);
			else if (td >= 1 && M.done[T_BARRACKS] >= 2 && M.cnt[T_ACADEMY] == 0 && can_afford(T_ACADEMY)) place(T_ACADEMY, 13);
			else if (td >= 1 && M.done[T_BARRACKS] >= 2 && M.cnt[T_EBAY] == 0 && can_afford(T_EBAY)) place(T_EBAY, 13);
		} else if (b->race == R_PROTOSS) {
			int want_gates = workers >= 10 ? (minerals > 300 ? 5 : 3) : 1;
			if (M.cnt[P_PYLON] && M.cnt[P_GATEWAY] < want_gates && can_afford(P_GATEWAY)) place(P_GATEWAY, 13);
			else if (M.cnt[P_ASSIM] < want_gas && M.cnt[P_GATEWAY] && can_afford(P_ASSIM)) place(P_ASSIM, 0);
			else if (td >= 1 && M.done[P_GATEWAY] && M.cnt[P_CORE] == 0 && can_afford(P_CORE)) place(P_CORE, 13);
			else if (td >= 1 && M.done[P_GATEWAY] >= 2 && M.cnt[P_FORGE] == 0 && can_afford(P_FORGE)) place(P_FORGE, 13);
			// templar tech (storm): Citadel -> Archives once the gateway army is rolling
			else if (td >= 3 && M.done[P_CORE] && M.done[P_GATEWAY] >= 3 && workers >= 20 && M.cnt[P_CITADEL] == 0 && can_afford(P_CITADEL)) place(P_CITADEL, 13);
			else if (td >= 3 && M.done[P_CITADEL] && M.cnt[P_ARCHIVES] == 0 && can_afford(P_ARCHIVES)) place(P_ARCHIVES, 13);
		} else {
			if (M.cnt[Z_POOL] == 0 && workers >= 9 && can_afford(Z_POOL)) place(Z_POOL, 10);
			else if (M.cnt[Z_POOL] && M.cnt[Z_EXTRACTOR] < want_gas && can_afford(Z_EXTRACTOR)) place(Z_EXTRACTOR, 0);
			// Macro hatchery: minerals piling up with no larva to spend them on
			// (measured: 2000+ banked on three bases) -> another hatchery at home.
			else if (minerals >= 500 && M.larvae == 0 && M.cnt[Z_HATCH] + M.cnt[Z_LAIR] + M.cnt[Z_HIVE] < 7 && builder
			         && ensure_grid() && plan_build(pc, builder->id, Z_HATCH)) { mark_placed(Z_HATCH); ++b->tel_plan; }
			else if (td >= 1 && M.done[Z_POOL] && M.cnt[Z_HYDRA_DEN] == 0 && can_afford(Z_HYDRA_DEN)) place(Z_HYDRA_DEN, 10);
		}
	}
	// An ACCEPTED expansion order is not a base yet: BW charges the price when the
	// worker arrives and starts it. Measured (botmatch P2): "expanding" logged six
	// times, bases stayed 2 -- the gateways spent the Nexus money while the probe
	// walked. Keep banking until the town hall appears (or the order times out).
	if (frame - b->expand_issued < b->expand_wait && M.bases <= b->bases_at_issue) { saving = true; saving_for_base = true; }
	// Resource discipline per level: 0 never banks (floats), 1 banks for depots
	// only, 2 full (UnifiedLite: every pending building / expansion).
	if (L->saving == 0) saving = false;
	else if (L->saving == 1 && !saving_for_base) saving = false;
	// While banking for a BO building, worker training pauses too: a 50-mineral
	// SCV every 20 s otherwise "wins the race" against a 50/50 Machine Shop or a
	// 150 Academy forever (measured L3 TvP: minerals never crossed 50 with the
	// shop pending). A real build order is a strict sequence -- the building first.
	// ...but only once the step is actually STUCK (6+ s actionable and still not
	// placed). Holding from the first tick chained into the next step's look-ahead
	// and froze probes and zealots for minutes (measured P3 vs Protoss: "save 1 hold
	// 1" from 1:30 to 3:30, one zealot at 3:00).
	if (saving && L->saving == 2 && b->bo_step_since >= 0 && frame - b->bo_step_since > 144) hold_workers = true;
	// PRESSURE: the enemy already has the bigger army. Units come first -- no
	// banking for tech or an expansion, no worker hold -- and production capacity
	// is added before anything else. Measured (botmatch T3 vs the native Protoss
	// AI): the factory opening sat on 3 marines and one barracks while 6 zealots
	// walked in at 4:40 and the game was over. A player who scouts that makes
	// barracks and a bunker, not a Machine Shop.
	bool pressure = L->rush_defense > 0 && E.str >= 12 && E.str > M.str;   // 3 zealots' worth, and more than we have
	if (pressure) {
		saving = false; saving_for_base = false; hold_workers = false;
		if (!cooldown) {
			if (b->race == R_TERRAN) {
				if (M.cnt[T_BARRACKS] < 3 && can_afford(T_BARRACKS)) place(T_BARRACKS, 13);
				else if (L->rush_defense >= 2 && M.done[T_BARRACKS] && M.cnt[T_BUNKER] < 1 && can_afford(T_BUNKER)) place(T_BUNKER, 8);
			} else if (b->race == R_PROTOSS) {
				if (M.done[P_PYLON] && M.cnt[P_GATEWAY] < 3 && can_afford(P_GATEWAY)) place(P_GATEWAY, 13);
			}
		}
	}
	// A bunker is only worth its 100 minerals with marines inside: idle marines
	// next to it walk in (right-click = enter) while the base is under pressure.
	if (b->race == R_TERRAN && M.done[T_BUNKER] && (pressure || b->rush || b->tel_threat)) {
		for (int k = 0; k != n; ++k) {
			if (mine[k].type != T_BUNKER || !mine[k].completed) continue;
			int sent = 0;
			for (int i = 0; i != n && sent < 4; ++i) {
				if (mine[i].type != T_MARINE || !mine[i].completed || !mine[i].idle) continue;
				if (d2(mine[i].x, mine[i].y, mine[k].x, mine[k].y) > (int64_t)384 * 384) continue;
				api->order_right_click(h, mine[i].id, mine[k].id); ++sent;
			}
		}
	}

	// ---- 5b. mobile detection -----------------------------------------------------
	// Cloaked / burrowed enemies cannot be shot without a detector NEXT TO THE ARMY.
	// Measured (botmatch): a Terran attack walked into Dark Templar and lost 60
	// units for 2. Turrets / cannons (above) only cover the base. So once the enemy
	// shows cloak tech -- or, on hard, simply late enough in the game -- get the
	// race's mobile detector: Protoss Robotics -> Observatory -> Observers, Terran
	// Starport + Control Tower + Science Facility -> Science Vessel. (Zerg already
	// has it: an overlord travels with the army, see the army section.)
	b->enemy_cloak = E.cnt[P_DT] > 0 || E.cnt[Z_LURKER] > 0 || E.cnt[Z_LURKER_EGG] > 0 || E.cnt[P_ARBITER] > 0 || E.cnt[P_ARCHIVES] > 0 || E.cnt[T_GHOST] > 0;
	{
		bool want_detect = L->tech_depth >= 2 && (b->enemy_cloak || (L->tech_depth >= 3 && frame > 12000));
		auto training = [&](const ob_unit* u) -> bool { ob_unit_ex ex; return api->abi_version >= 3 && api->unit_ex && api->unit_ex(h, u->id, &ex) && ex.is_training; };
		if (want_detect && !pressure) {
			int next = -1;   // the building this chain needs next
			if (b->race == R_PROTOSS) {
				if (M.done[P_CORE] && M.cnt[P_ROBO] == 0) next = P_ROBO;
				else if (M.done[P_ROBO] && M.cnt[P_OBSERVATORY] == 0) next = P_OBSERVATORY;
				int want_obs = b->enemy_cloak ? 2 : 1;
				if (M.done[P_OBSERVATORY] && M.by_type[P_ROBO] && M.cnt[P_OBSERVER] < want_obs && minerals >= 25 && gas >= 75 && sa - su >= 2 && !training(M.by_type[P_ROBO]))
					api->train(h, M.by_type[P_ROBO]->id, P_OBSERVER);
			} else if (b->race == R_TERRAN) {
				if (M.cnt[T_REFINERY] == 0) next = T_REFINERY;
				else if (M.done[T_BARRACKS] && M.cnt[T_FACTORY] == 0) next = T_FACTORY;
				else if (M.done[T_FACTORY] && M.cnt[T_STARPORT] == 0) next = T_STARPORT;
				else if (M.done[T_STARPORT] && M.cnt[T_SCIENCE_FACILITY] == 0) next = T_SCIENCE_FACILITY;
				if (b->enemy_cloak && M.done[T_ACADEMY] && M.cnt[T_COMSAT] == 0 && M.by_type[T_CC] && minerals >= 50 && gas >= 50 && api->abi_version >= 5 && api->build_addon && !training(M.by_type[T_CC]))
					api->build_addon(h, M.by_type[T_CC]->id, T_COMSAT);
				if (b->enemy_cloak && M.cnt[T_ACADEMY] == 0 && M.done[T_BARRACKS] && can_afford(T_ACADEMY) && !cooldown) place(T_ACADEMY, 13);
				if (M.done[T_STARPORT] && M.cnt[T_CONTROL_TOWER] == 0 && minerals >= 50 && gas >= 50 && api->abi_version >= 4 && api->build_addon && !training(M.by_type[T_STARPORT]))
					api->build_addon(h, M.by_type[T_STARPORT]->id, T_CONTROL_TOWER);
				int want_sv = b->enemy_cloak ? 2 : 1;
				if (M.done[T_CONTROL_TOWER] && M.done[T_SCIENCE_FACILITY] && M.by_type[T_STARPORT] && M.cnt[T_SCIENCE_VESSEL] < want_sv && minerals >= 100 && gas >= 225 && sa - su >= 4 && !training(M.by_type[T_STARPORT]))
					api->train(h, M.by_type[T_STARPORT]->id, T_SCIENCE_VESSEL);
			}
			if (next >= 0 && b->pending_type != next) {
				if (can_afford(next)) { if (!cooldown) place(next, 13); }
				// the enemy HAS cloaked units: this outranks more army (above the army floor)
				else if (b->enemy_cloak && gas >= gas_cost(next)) saving = true;
			}
		}
	}

	// ---- 6. research / upgrades (upgrades_max caps the level pursued) ---------
	const int UM = L->upgrades_max;
	if (v2 && UM > 0) {
		if (b->race == R_TERRAN) {
			if (M.by_type[T_ACADEMY] && gas >= 100) {
				if (!api->tech_researched(h, me, TECH_STIM)) api->research(h, M.by_type[T_ACADEMY]->id, TECH_STIM);
				else if (api->upgrade_level(h, me, UP_T_U238) == 0) api->upgrade(h, M.by_type[T_ACADEMY]->id, UP_T_U238);
			}
			if (M.by_type[T_EBAY] && gas >= 100) {
				int w = api->upgrade_level(h, me, UP_T_INF_WEAPONS), a = api->upgrade_level(h, me, UP_T_INF_ARMOR);
				if (w <= a && w < UM) api->upgrade(h, M.by_type[T_EBAY]->id, UP_T_INF_WEAPONS); else if (a < UM) api->upgrade(h, M.by_type[T_EBAY]->id, UP_T_INF_ARMOR);
			}
			// Siege Mode at the Machine Shop (tanks still fire un-sieged; sieging itself
			// needs a use_tech ABI -- follow-up), then Ion Thrusters for vultures.
			if (M.by_type[T_MACHINE_SHOP] && gas >= 150) {
				if (!api->tech_researched(h, me, TECH_SIEGE_MODE)) api->research(h, M.by_type[T_MACHINE_SHOP]->id, TECH_SIEGE_MODE);
				else if (api->upgrade_level(h, me, UP_T_ION_THRUSTERS) == 0 && M.cnt[T_VULTURE] >= 4) api->upgrade(h, M.by_type[T_MACHINE_SHOP]->id, UP_T_ION_THRUSTERS);
			}
		} else if (b->race == R_ZERG) {
			// Zerg upgrades (ids verified from the byte-exact table): pool -> ling speed,
			// den -> hydra range then speed, evo -> missile/melee/carapace alternating.
			if (M.by_type[Z_POOL] && gas >= 100 && M.cnt[Z_ZERGLING] >= 6 && api->upgrade_level(h, me, UP_Z_METABOLIC_BOOST) == 0)
				api->upgrade(h, M.by_type[Z_POOL]->id, UP_Z_METABOLIC_BOOST);
			// Lurker Aspect first (needs a Lair): lurkers are the Zerg answer to marine /
			// medic and to zealots -- and, with ABI v5, they can finally burrow.
			if (M.by_type[Z_HYDRA_DEN] && L->micro >= 1 && api->abi_version >= 5 && b->enemy_race != R_ZERG && (M.done[Z_LAIR] || M.done[Z_HIVE])
			    && gas >= 200 && minerals >= 200 && !api->tech_researched(h, me, TECH_LURKER_ASPECT)) api->research(h, M.by_type[Z_HYDRA_DEN]->id, TECH_LURKER_ASPECT);
			if (M.by_type[Z_HYDRA_DEN] && gas >= 150 && M.cnt[Z_HYDRA] >= 4) {
				if (api->upgrade_level(h, me, UP_Z_GROOVED_SPINES) == 0) api->upgrade(h, M.by_type[Z_HYDRA_DEN]->id, UP_Z_GROOVED_SPINES);
				else if (api->upgrade_level(h, me, UP_Z_MUSCULAR_AUGMENTS) == 0) api->upgrade(h, M.by_type[Z_HYDRA_DEN]->id, UP_Z_MUSCULAR_AUGMENTS);
			}
			if (M.by_type[Z_EVO] && gas >= 100) {
				int mi = api->upgrade_level(h, me, UP_Z_MISSILE), me_ = api->upgrade_level(h, me, UP_Z_MELEE), ca = api->upgrade_level(h, me, UP_Z_CARAPACE);
				bool ranged = M.cnt[Z_HYDRA] >= M.cnt[Z_ZERGLING] / 2;
				if (ranged && mi <= ca && mi < UM) api->upgrade(h, M.by_type[Z_EVO]->id, UP_Z_MISSILE);
				else if (!ranged && me_ <= ca && me_ < UM) api->upgrade(h, M.by_type[Z_EVO]->id, UP_Z_MELEE);
				else if (ca < UM) api->upgrade(h, M.by_type[Z_EVO]->id, UP_Z_CARAPACE);
			}
		} else if (b->race == R_PROTOSS) {
			if (M.by_type[P_CORE] && gas >= 150 && api->upgrade_level(h, me, UP_P_SINGULARITY) == 0) api->upgrade(h, M.by_type[P_CORE]->id, UP_P_SINGULARITY);
			if (M.by_type[P_ARCHIVES] && L->micro >= 1 && gas >= 200 && minerals >= 200 && !api->tech_researched(h, me, TECH_PSI_STORM)) api->research(h, M.by_type[P_ARCHIVES]->id, TECH_PSI_STORM);
			if (M.by_type[P_CITADEL] && gas >= 150 && M.cnt[P_ZEALOT] >= 4 && api->upgrade_level(h, me, UP_P_LEG_ENHANCEMENTS) == 0) api->upgrade(h, M.by_type[P_CITADEL]->id, UP_P_LEG_ENHANCEMENTS);
			if (M.by_type[P_FORGE] && gas >= 100) {
				int w = api->upgrade_level(h, me, UP_P_GROUND_WEAPONS), a = api->upgrade_level(h, me, UP_P_GROUND_ARMOR);
				if (w <= a && w < UM) api->upgrade(h, M.by_type[P_FORGE]->id, UP_P_GROUND_WEAPONS); else if (a < UM) api->upgrade(h, M.by_type[P_FORGE]->id, UP_P_GROUND_ARMOR);
			}
		}
	}

	// ---- 7. production (composition per matchup) -----------------------------
	int want_workers = L->workers_per_base + (M.bases - 1) * 8 + M.done[gt] * 3;
	if (want_workers > L->max_workers) want_workers = L->max_workers;
	if (b->race == R_ZERG) {
		// larva priority: overlord (supply) > drone (economy) > army by matchup
		int margin = 4 + M.army / 4;
		// Overlords already morphing count as supply on the way (the old test did not
		// see them, so every larva of a tick became an overlord, or -- after a raid
		// killed the overlord cloud -- only one was rebuilt per pass). Up to 4 at once.
		int pending_ov = 0;
		if (api->abi_version >= 3 && api->unit_ex) for (int i = 0; i != n; ++i) if (mine[i].type == 36 /*egg*/) { ob_unit_ex ex; if (api->unit_ex(h, mine[i].id, &ex) && ex.build_type == Z_OVERLORD) ++pending_ov; }
		int want_ov = 0;
		if (sa < 400) { int eff = sa + 16 * pending_ov; while (eff - su <= margin && want_ov < 4) { ++want_ov; eff += 16; } }
		int mleft = minerals, gleft = gas;
		// Lurkers: morph hydralisks once the aspect is researched -- up to 8 against
		// Terran (bio melts to them), 5 against Protoss.
		bool lurk_ok = L->micro >= 1 && api->abi_version >= 5 && b->enemy_race != R_ZERG && api->tech_researched(h, me, TECH_LURKER_ASPECT);
		int lurk_want = b->enemy_race == R_TERRAN ? 8 : 5;
		int lurk_have = M.cnt[Z_LURKER] + M.cnt[Z_LURKER_EGG];
		if (lurk_ok) for (int i = 0; i != n && lurk_have < lurk_want && mleft >= 50 && gleft >= 100; ++i) {
			if (mine[i].type != Z_HYDRA || !mine[i].completed) continue;
			if (api->train(h, mine[i].id, Z_LURKER)) { ++lurk_have; mleft -= 50; gleft -= 100; }
		}
		// Drones are not the only thing larvae are for. Measured (botmatch Z2 vs the
		// native Terran AI): 28 drones, 800 minerals banked, ZERO army at 6 minutes --
		// the first 13-marine push killed every drone. Once the pool is up keep a
		// standing force sized to what the enemy has shown, before more drones.
		int safety = M.done[Z_POOL] ? 6 : 0;
		// a zergling is half a zealot/marine-with-medic at best: size the standing
		// force in lings per enemy fighter, not one for one
		// (with a den the force is hydralisks: one per enemy fighter plus a few)
		// ...in STRENGTH: a quarter more than the enemy fields, capped.
		if (M.done[Z_POOL] && E.str > 0) {
			int e = E.str + E.str / 4 + 4;
			if (e > 120) e = 120;
			if (e > safety) safety = e;
		}
		if (M.done[Z_POOL] && frame > 7000 && safety < 12) safety = 12;
		if (L->rush_defense == 0) safety = 0;   // very easy: drones first, always
		bool army_first = M.str < safety;
		// "Army first" must not mean "no economy": zerglings are strength 1, so a
		// safety of 50 kept every larva on army for minutes and the drone count sat
		// at 12-16 (measured). Only when we are actually BEHIND the enemy, or someone
		// is in the base, does every larva go to army; otherwise larvae alternate.
		bool army_only = army_first && (M.str < E.str || b->tel_threat > 0 || b->rush);
		int larva_i = 0;
		b->tel_hold = hold_workers ? 1 : 0;
		bool want_muta = M.done[Z_SPIRE] && gas >= 100 && (b->enemy_race == R_TERRAN || b->enemy_air);
		for (int i = 0; i != n; ++i) {
			if (mine[i].type != Z_LARVA) continue;
			intptr_t lv = mine[i].id;
			if (want_ov > 0 && mleft >= 100) { if (api->train(h, lv, Z_OVERLORD)) { --want_ov; mleft -= 100; } continue; }
			if (want_ov > 0) continue;   // supply first: do not spend the overlord's minerals on drones
			if (sa - su < 1) continue;
			bool drone_turn = !army_first || (!army_only && (larva_i++ & 1) == 0);
			if (drone_turn && M.workers < want_workers && mleft >= 50 && !b->rush && !hold_workers) { if (api->train(h, lv, Z_DRONE)) mleft -= 50; continue; }
			if (saving && !b->rush && !army_first) continue;   // bank for the pending building / expansion
			if (want_muta && minerals >= 100) { api->train(h, lv, Z_MUTA); continue; }
			// Hydralisks are a THIRD of the army, not all of it: their explosive damage
			// is halved against small units (marines, zealots, zerglings), so a pure
			// hydra force traded worse than zerglings did (measured: hydra-first won
			// fewer games than ling-heavy). They are there for range, air and big targets.
			bool hydra_turn = M.cnt[Z_HYDRA] * 4 < M.cnt[Z_ZERGLING] + 4 || b->enemy_air || (lurk_ok && lurk_have < lurk_want && M.cnt[Z_HYDRA] < 4);
			if (M.done[Z_HYDRA_DEN] && hydra_turn && mleft >= 75 && gleft >= 25 && (b->enemy_race != R_ZERG || M.cnt[Z_ZERGLING] > 12)) { if (api->train(h, lv, Z_HYDRA)) { mleft -= 75; gleft -= 25; ++M.cnt[Z_HYDRA]; } continue; }
			if (M.done[Z_POOL] && minerals >= 50) { api->train(h, lv, Z_ZERGLING); continue; }
		}
	} else {
		int bt = b->race == R_TERRAN ? T_CC : P_NEXUS;
		// One unit at a time per building (medium / hard). BW charges a unit when it
		// is QUEUED, and train() every tick filled every queue five deep: 250 minerals
		// asleep in each barracks and in the Command Center. Measured (botmatch T3):
		// 19 SCVs, minerals pinned under 100, never the 150 for a second barracks.
		// The easy levels keep the sloppy deep queues -- that IS a beginner's macro.
		auto queued = [&](const ob_unit& u) -> bool {
			if (b->level_id < 2 || api->abi_version < 3 || !api->unit_ex) return false;
			ob_unit_ex ex;
			return api->unit_ex(h, u.id, &ex) && ex.is_training;
		};
		b->tel_cc = 9;   // DIAG: 9 = no completed base found
		for (int i = 0; i != n; ++i) {
			if (mine[i].type != bt || !mine[i].completed) continue;
			if (queued(mine[i])) { b->tel_cc = 7; continue; }   // 7 = already training
			bool save_pylon = b->race == R_PROTOSS && M.cnt[P_PYLON] == 0 && (sa - su) <= 4 && minerals < 150;
			// DIAG reason codes: 0 trained, 1 at worker cap, 2 minerals, 3 supply, 4 pylon-save, 5 hold, 6 train() rejected
			if (M.workers >= want_workers) b->tel_cc = 1;
			else if (minerals < 50) b->tel_cc = 2;
			else if (sa - su < 2) b->tel_cc = 3;
			else if (save_pylon) b->tel_cc = 4;
			else if (hold_workers) b->tel_cc = 5;
			else b->tel_cc = api->train(h, mine[i].id, wt) ? 0 : 6;
		}
		b->tel_hold = hold_workers ? 1 : 0;
		// Bank for the pending building / expansion -- but never below a standing
		// force: what the enemy has shown, and at least a handful after 2:30.
		// (two MORE than the enemy shows -- parity loses the first fight to a
		// committed push -- and six from 2:00 on)
		int army_floor = frame > 3000 ? 16 : 0;          // strength: ~8 marines / 4 zealots
		if (E.str + 8 > army_floor) army_floor = E.str + 8;
		if (L->rush_defense == 0) army_floor = 0;
		bool hold_army = saving && !b->rush && M.str >= army_floor;
		b->tel_rax = hold_army ? 5 : 0;
		if (b->race == R_TERRAN) {
			// Factory first. A tank is 150/100 and a marine 50: with the barracks
			// spending every 50 the moment it arrives, the factory never saw 150
			// (measured T3 vs Protoss: 850 gas banked, zero tanks, marines fed to
			// zealots one at a time). While an idle factory can make a tank, the
			// barracks wait for it -- unless something is in our base right now.
			bool tank_waiting = false;
			int mleft = minerals;
			for (int i = 0; i != n && !hold_army; ++i) {
				if (mine[i].type != T_FACTORY || !mine[i].completed || queued(mine[i]) || sa - su < 4) continue;
				bool tank = M.done[T_MACHINE_SHOP] && gas >= 100 && M.cnt[T_TANK] <= M.cnt[T_VULTURE] * 2 + 1;
				if (tank) {
					if (mleft >= 150) { if (api->train(h, mine[i].id, T_TANK)) mleft -= 150; }
					else tank_waiting = true;
				} else if (mleft >= 75) { if (api->train(h, mine[i].id, T_VULTURE)) mleft -= 75; }
			}
			if (tank_waiting && b->tel_threat == 0) hold_army = true;
			for (int i = 0; i != n; ++i) {
				if (mine[i].type != T_BARRACKS || !mine[i].completed || queued(mine[i])) continue;
				if (hold_army || minerals < 50 || sa - su < 2) { if (!hold_army) b->tel_rax = minerals < 50 ? 2 : 3; break; }
				bool acad = M.done[T_ACADEMY] > 0;
				bool medic = acad && gas >= 25 && M.cnt[T_MEDIC] * 4 < M.cnt[T_MARINE];
				bool bat = acad && gas >= 25 && b->enemy_race == R_ZERG && M.cnt[T_FIREBAT] * 3 < M.cnt[T_MARINE];
				b->tel_rax = api->train(h, mine[i].id, medic ? T_MEDIC : bat ? T_FIREBAT : T_MARINE) ? 0 : 6;
			}
		} else {
			for (int i = 0; i != n; ++i) {
				if (mine[i].type != P_GATEWAY || !mine[i].completed || queued(mine[i])) continue;
				if (hold_army || minerals < 100 || sa - su < 2) break;
				bool dt = M.done[P_ARCHIVES] && gas >= 100 && M.cnt[P_DT] < 4 && b->enemy_race != R_ZERG;
				// a few High Templar once storm is researched (they follow the army and cast)
				if (M.done[P_ARCHIVES] && L->micro >= 1 && gas >= 150 && minerals >= 50 && M.cnt[P_HT] < 4 && api->tech_researched(h, me, TECH_PSI_STORM)) {
					if (api->train(h, mine[i].id, P_HT)) { ++M.cnt[P_HT]; continue; }
				}
				bool goon = M.done[P_CORE] && gas >= 50 && (b->enemy_race == R_TERRAN ? M.cnt[P_DRAGOON] < M.cnt[P_ZEALOT] * 3 : M.cnt[P_DRAGOON] < M.cnt[P_ZEALOT]);
				if (dt && minerals >= 125) api->train(h, mine[i].id, P_DT);
				else if (goon && minerals >= 125) api->train(h, mine[i].id, P_DRAGOON);
				else api->train(h, mine[i].id, P_ZEALOT);
			}
		}
	}

	// ---- 7b. overlords: scouts and sentries, not a blob over the main ----------
	// Play-test report: every overlord sat in one cloud above the hatchery -- the
	// map was never looked at (so expansions stayed unexplored = unplaceable) and
	// one raid on the cloud supply-killed the bot. A player sends the first
	// overlord out, parks the next ones on the natural / the approaches / the
	// expansions and pulls a hurt one back. Posts are keyed by POSITION (unit
	// handles do not survive a frame).
	if (b->race == R_ZERG && L->scout >= 1) {
		// an overlord's idle order is PickupIdle (93, the transport idle) -- older hosts
		// did not report that as idle
		for (int i = 0; i != n; ++i) if (mine[i].type == Z_OVERLORD && mine[i].order == 93) mine[i].idle = 1;
		int32_t mapw = b->map_tw * 32, maph = b->map_th * 32;
		int32_t px[40], py[40]; int np = 0;
		auto add_post = [&](int32_t x, int32_t y) {
			if (np == 40) return;
			if (x < 96) x = 96; if (y < 96) y = 96; if (x > mapw - 96) x = mapw - 96; if (y > maph - 96) y = maph - 96;
			for (int i = 0; i != np; ++i) if (d2(x, y, px[i], py[i]) < (int64_t)192 * 192) return;
			px[np] = x; py[np] = y; ++np;
		};
		double fdx = (have_enemy_start ? est_x : mapw / 2) - base->x, fdy = (have_enemy_start ? est_y : maph / 2) - base->y;
		double fl = sqrt(fdx * fdx + fdy * fdy); if (fl < 1) fl = 1; fdx /= fl; fdy /= fl;
		// (a) hard: the first overlord goes to look at the enemy (stops 8 tiles short)
		if (L->scout >= 3 && have_enemy_start && fl > 640) add_post(est_x - (int32_t)(fdx * 256), est_y - (int32_t)(fdy * 256));
		// (b) our next base site -- seeing it is what makes it placeable
		if (have_exp) add_post(exp_x, exp_y);
		// (c) the approaches to the main: front, then both flanks
		add_post(base->x + (int32_t)(fdx * 352), base->y + (int32_t)(fdy * 352));
		add_post(base->x + (int32_t)(-fdy * 352), base->y + (int32_t)(fdx * 352));
		add_post(base->x + (int32_t)(fdy * 352), base->y + (int32_t)(-fdx * 352));
		// (d) free expansions, nearest first (medium: the 3 nearest; hard: all, even near the enemy)
		if (L->scout >= 2) {
			bool used[64]; memset(used, 0, sizeof used);
			int limit = L->scout >= 3 ? 64 : 3;
			for (int r = 0; r != limit; ++r) {
				int pick = -1; int64_t bd = -1;
				for (int c = 0; c != g_ncls; ++c) {
					if (used[c] || g_cls[c].taken) continue;
					if (L->scout < 3 && have_enemy_start && d2(g_cls[c].cx, g_cls[c].cy, est_x, est_y) < (int64_t)640 * 640) continue;
					int64_t d = d2(g_cls[c].cx, g_cls[c].cy, base->x, base->y);
					if (pick < 0 || d < bd) { pick = c; bd = d; }
				}
				if (pick < 0) break;
				used[pick] = true; add_post(g_cls[pick].cx, g_cls[pick].cy);
			}
		}
		auto post_entry = [&](int32_t x, int32_t y, bool create) -> bot_t::post_t* {
			for (int i = 0; i != b->nposts; ++i) if (d2(x, y, b->posts[i].x, b->posts[i].y) < (int64_t)128 * 128) return &b->posts[i];
			if (!create) return nullptr;
			int slot = 0;
			if (b->nposts < 40) slot = b->nposts++;
			else for (int i = 1; i != 40; ++i) if (b->posts[i].sent < b->posts[slot].sent) slot = i;
			b->posts[slot] = bot_t::post_t{x, y, -100000, -100000};
			return &b->posts[slot];
		};
		// A hurt overlord away from home comes back, and its post is avoided for a while.
		int free_n = 0;
		for (int i = 0; i != n; ++i) {
			ob_unit& u = mine[i];
			if (u.type != Z_OVERLORD || !u.completed) continue;
			int64_t dh = d2(u.x, u.y, base->x, base->y);
			if (L->scout >= 2 && u.hp < 150 && dh > (int64_t)384 * 384) {
				if (u.idle) { api->order_move(h, u.id, base->x, base->y); api->log(h, "UA:overlord hurt -> retreating home"); }
				int best = -1; int64_t bd = -1;
				for (int k = 0; k != np; ++k) { int64_t d = d2(u.x, u.y, px[k], py[k]); if (best < 0 || d < bd) { best = k; bd = d; } }
				if (best >= 0 && bd < (int64_t)320 * 320) post_entry(px[best], py[best], true)->danger_until = frame + 4800;
				continue;
			}
			if (u.idle && dh < (int64_t)288 * 288) ++free_n;
		}
		// One overlord always stays home; every tick at most one free one is sent out.
		if (free_n > 1 && frame - b->last_ovie_order >= 72) {
			for (int k = 0; k != np; ++k) {
				bool covered = false;
				for (int i = 0; i != n && !covered; ++i) if (mine[i].type == Z_OVERLORD && d2(mine[i].x, mine[i].y, px[k], py[k]) < (int64_t)160 * 160) covered = true;
				if (covered) continue;
				bot_t::post_t* e = post_entry(px[k], py[k], false);
				int32_t travel = 300 + (int32_t)(sqrt((double)d2(base->x, base->y, px[k], py[k])) * 1.3);
				if (e && (frame < e->danger_until || frame - e->sent < travel)) continue;
				ob_unit* pick = nullptr; int64_t bd = -1;
				for (int i = 0; i != n; ++i) {
					ob_unit& u = mine[i];
					if (u.type != Z_OVERLORD || !u.completed || !u.idle || d2(u.x, u.y, base->x, base->y) >= (int64_t)288 * 288) continue;
					int64_t d = d2(u.x, u.y, px[k], py[k]);
					if (!pick || d < bd) { pick = &u; bd = d; }
				}
				if (!pick) break;
				api->order_move(h, pick->id, px[k], py[k]);
				e = post_entry(px[k], py[k], true); e->sent = frame;
				b->last_ovie_order = frame;
				char s[96]; snprintf(s, sizeof s, "UA:overlord -> post %d/%d at tile (%d,%d)", k + 1, np, px[k] / 32, py[k] / 32); api->log(h, s);
				break;
			}
		}
	}

	// ---- 8. army -----------------------------------------------------------------
	// micro 0 (very easy / easy): the old behaviour -- idle units are attack-moved at
	// the enemy start whenever a wave is ready; units trickle and never retreat.
	// micro 1 (medium): the army behaves as ONE group -- it gathers at a staging
	//   point in front of the front base, leaves together, regroups when it strings
	//   out, is reinforced in groups (not one unit at a time), keeps going to the
	//   next enemy building once a base is razed, and RUNS home when outnumbered.
	// micro 2 (hard): + focus fire (ranged units shoot the weakest enemy in range),
	//   wounded high-value units pull back, vultures / dragoons kite slower melee.
	// Measured before this existed (botmatch, hard vs the native AI, 36 games): 20
	// wins but 1347 fighters lost for 753 killed -- waves fed in piecemeal.
	const int micro = L->micro;
	if (micro == 0) {
		if (!b->rush) {
			if (frame < b->defend_until) {
				for (int i = 0; i != n; ++i) if (is_army(mine[i].type) && mine[i].completed) amove(mine[i].id, base->x, base->y);
			} else {
				int wave = L->wave_min + (b->race == R_ZERG ? 6 : 0);
				if (b->enemy_expanded) wave += 6;
				bool ahead = M.army * L->ahead_num >= E.army * L->ahead_den;
				if (M.army >= wave && ahead && frame - b->last_attack_frame >= L->attack_cooldown) {
					int32_t ex = 0, ey = 0;
					if (api->enemy_start(h, &ex, &ey)) {
						for (int i = 0; i != n; ++i) if (is_army(mine[i].type) && mine[i].completed && mine[i].idle) amove(mine[i].id, ex, ey);
						b->last_attack_frame = frame;
						api->log(h, "attack wave");
					}
				}
			}
		}
	} else {
		int32_t mapw = b->map_tw * 32, maph = b->map_th * 32;
		// staging point: ~9 tiles in front of the base nearest the enemy
		ob_unit* front = base;
		{
			int64_t best = 0; bool first = true;
			int32_t tx = have_enemy_start ? est_x : mapw / 2, ty = have_enemy_start ? est_y : maph / 2;
			for (int i = 0; i != n; ++i) if (is_base(mine[i].type) && mine[i].completed) {
				int64_t d = d2(mine[i].x, mine[i].y, tx, ty);
				if (first || d < best) { front = &mine[i]; best = d; first = false; }
			}
		}
		double adx = (have_enemy_start ? est_x : mapw / 2) - front->x, ady = (have_enemy_start ? est_y : maph / 2) - front->y;
		double al = sqrt(adx * adx + ady * ady); if (al < 1) al = 1;
		int32_t stage_x = front->x + (int32_t)(adx / al * 288), stage_y = front->y + (int32_t)(ady / al * 288);
		if (stage_x < 64) stage_x = 64; if (stage_y < 64) stage_y = 64;
		if (stage_x > mapw - 64) stage_x = mapw - 64; if (stage_y > maph - 64) stage_y = maph - 64;

		// who is home, who is out on the attack
		auto at_home = [&](const ob_unit& u) -> bool {
			if (d2(u.x, u.y, stage_x, stage_y) < (int64_t)704 * 704) return true;
			for (int k = 0; k != n; ++k) if (is_base(mine[k].type) && d2(u.x, u.y, mine[k].x, mine[k].y) < (int64_t)800 * 800) return true;
			return false;
		};
		static uint8_t out_flag[700];
		int n_out = 0, n_home_idle = 0; int64_t sx = 0, sy = 0;
		for (int i = 0; i != n; ++i) {
			out_flag[i] = 0;
			if (!is_army(mine[i].type) || !mine[i].completed) continue;
			if (at_home(mine[i])) { if (mine[i].idle) ++n_home_idle; continue; }
			out_flag[i] = 1; ++n_out; sx += mine[i].x; sy += mine[i].y;
		}
		int32_t cx = n_out ? (int32_t)(sx / n_out) : stage_x, cy = n_out ? (int32_t)(sy / n_out) : stage_y;
		int64_t spread = 0;
		for (int i = 0; i != n; ++i) if (out_flag[i]) spread += (int64_t)sqrt((double)d2(mine[i].x, mine[i].y, cx, cy));
		if (n_out) spread /= n_out;
		// what the group is facing: fighters within 14 tiles, static defence counts double
		int en_near = 0, en_str = 0, out_str = 0;
		for (int i = 0; i != ne && n_out; ++i) {
			int t = theirs[i].type;
			bool stat = t == T_BUNKER || t == P_CANNON || t == Z_SUNKEN;
			if (!(is_army(t) || stat) || !theirs[i].completed) continue;
			if (d2(theirs[i].x, theirs[i].y, cx, cy) < (int64_t)448 * 448) { en_near += stat ? 2 : 1; en_str += stat ? (t == T_BUNKER ? 8 : 5) : unit_str(t); }
		}
		for (int i = 0; i != n; ++i) if (out_flag[i]) out_str += unit_str(mine[i].type);
		b->tel_out = n_out; b->tel_en_near = en_near;

		bool retreating = frame < b->defend_until;
		if (b->attacking) {
			bool local_bad = en_str >= 12 && out_str * 10 < en_str * 8;
			bool global_bad = L->retreat_pct > 0 && E.str > 0 && M.str * 100 < E.str * L->retreat_pct && su < 340;
			// The attack is being eaten without killing anything: half the group is
			// gone and the enemy army is not smaller than when we left. (Measured:
			// Terran walking into Dark Templar lost 60 units for 2 -- nothing visible
			// was ever "near", so neither test above fired, and the reinforcement
			// stream kept feeding it.) Call it off and wait a full cooldown.
			if (n_out > b->atk_peak) b->atk_peak = n_out;
			bool bleeding = b->atk_peak >= 6 && n_out * 2 <= b->atk_peak && E.str * 5 >= b->atk_enemy0 * 4;
			if ((local_bad || global_bad || bleeding) && frame - b->atk_since > 240) {
				b->defend_until = frame + 600; b->attacking = false; b->last_attack_frame = frame; retreating = true;
				char s[96]; snprintf(s, sizeof s, "UA:retreat -- %d of ours face %d (army %d vs %d)", n_out, en_near, M.army, E.army); api->log(h, s);
			} else if (n_out == 0 && n_home_idle < 4 && frame - b->atk_since > 720) {
				b->attacking = false; b->last_attack_frame = frame;   // the wave is gone
			}
		}

		if (retreating) {
			// RUN (move, not attack-move): units that stop to trade on the way home die
			if (frame - b->last_army_order >= 48) {
				b->last_army_order = frame;
				for (int i = 0; i != n; ++i) if (out_flag[i]) api->order_move(h, mine[i].id, stage_x, stage_y);
				for (int i = 0; i != n; ++i) if ((mine[i].type == T_SCIENCE_VESSEL || mine[i].type == P_OBSERVER || mine[i].type == P_HT) && mine[i].completed && d2(mine[i].x, mine[i].y, stage_x, stage_y) > (int64_t)352 * 352)
					api->order_move(h, mine[i].id, stage_x, stage_y);
			}
		} else if (b->attacking) {
			// Target upkeep: their town hall while it stands, else the enemy building
			// nearest the group -- the old code kept walking to the dead main and the
			// game never ended (15 of 36 hard games ran into the time limit).
			bool have_t = false;
			for (int i = 0; i != ne && !have_t; ++i) if (theirs[i].type >= 106 && theirs[i].type < 203 && d2(theirs[i].x, theirs[i].y, b->atk_x, b->atk_y) < (int64_t)320 * 320) have_t = true;
			if (!have_t) {
				ob_unit* nb = nullptr; int64_t bd = -1;
				for (int i = 0; i != ne; ++i) if (theirs[i].type >= 106 && theirs[i].type < 203) { int64_t d = d2(theirs[i].x, theirs[i].y, cx, cy); if (!nb || d < bd) { nb = &theirs[i]; bd = d; } }
				if (!nb) for (int i = 0; i != ne; ++i) { int64_t d = d2(theirs[i].x, theirs[i].y, cx, cy); if (!nb || d < bd) { nb = &theirs[i]; bd = d; } }
				if (nb) { b->atk_x = nb->x; b->atk_y = nb->y; b->last_army_order = -100000; }
				else { b->attacking = false; b->last_attack_frame = frame; }
			}
			if (b->attacking && frame - b->last_army_order >= 72) {
				b->last_army_order = frame;
				// Leash, not a rally: only units far AHEAD of the body wait for it. (The first
				// version pulled everyone to the centroid whenever the group was spread --
				// with reinforcements streaming in from home the centroid sat between the
				// two and the army walked back and forth for 4000 frames, 43 units "out"
				// and none at the target. Measured with botmatch.) The body is the MEDIAN
				// distance to the target, so a reinforcement tail cannot drag it back.
				static int32_t dist_t[700]; static int32_t sorted[700]; int nd = 0;
				ob_unit* mid = nullptr;
				for (int i = 0; i != n; ++i) if (out_flag[i]) { dist_t[i] = (int32_t)sqrt((double)d2(mine[i].x, mine[i].y, b->atk_x, b->atk_y)); sorted[nd++] = dist_t[i]; }
				int32_t dm = 0;
				if (nd) {
					for (int i = 1; i < nd; ++i) { int32_t v = sorted[i]; int j = i - 1; while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; } sorted[j + 1] = v; }
					dm = sorted[nd / 2];
					for (int i = 0; i != n; ++i) if (out_flag[i] && (!mid || abs(dist_t[i] - dm) < abs(dist_t[mid - mine] - dm))) mid = &mine[i];
				}
				for (int i = 0; i != n; ++i) {
					if (!out_flag[i]) continue;
					bool leader = nd >= 6 && en_near == 0 && mid && dist_t[i] + 352 < dm && dm > 480;
					if (leader) amove(mine[i].id, mid->x, mid->y);
					else if (mine[i].idle) amove(mine[i].id, b->atk_x, b->atk_y);
				}
				// The detector travels WITH the body of the army: every Science Vessel /
				// Observer we own, and for Zerg one overlord (two against cloak tech).
				if (mid && frame - b->last_escort_order >= 96) {
					b->last_escort_order = frame;
					int want_ov = b->race == R_ZERG ? (b->enemy_cloak ? 2 : 1) : 0;
					for (int r = 0; r != want_ov; ++r) {
						ob_unit* pick = nullptr; int64_t bd = -1;
						for (int i = 0; i != n; ++i) {
							ob_unit& u = mine[i];
							if (u.type != Z_OVERLORD || !u.completed || u.hp < 150 || u.order == 255) continue;
							int64_t d = d2(u.x, u.y, mid->x, mid->y);
							if (!pick || d < bd) { pick = &u; bd = d; }
						}
						if (!pick) break;
						if (bd > (int64_t)96 * 96) api->order_move(h, pick->id, mid->x, mid->y);
						pick->order = 255;   // taken this tick
					}
					for (int i = 0; i != n; ++i) if ((mine[i].type == T_SCIENCE_VESSEL || mine[i].type == P_OBSERVER || mine[i].type == P_HT) && mine[i].completed && d2(mine[i].x, mine[i].y, mid->x, mid->y) > (int64_t)96 * 96)
						api->order_move(h, mine[i].id, mid->x, mid->y);
				}
				// reinforcements leave home as a group and join the army, not the enemy base
				int need = n_out ? 6 : 1;
				if (n_home_idle >= need && b->tel_threat == 0 && (n_out == 0 || n_out * 10 >= b->atk_peak * 6)) {
					for (int i = 0; i != n; ++i) if (is_army(mine[i].type) && mine[i].completed && mine[i].idle && !out_flag[i])
						amove(mine[i].id, mid ? mid->x : b->atk_x, mid ? mid->y : b->atk_y);
				}
			}
		} else {
			// gather at the staging point (a walking unit is not idle, so this fires once per unit)
			if (b->tel_threat == 0 && frame - b->last_army_order >= 48) {
				b->last_army_order = frame;
				for (int i = 0; i != n; ++i) {
					ob_unit& u = mine[i];
					if (!is_army(u.type) || !u.completed || !u.idle) continue;
					if (d2(u.x, u.y, stage_x, stage_y) > (int64_t)224 * 224) amove(u.id, stage_x, stage_y);
				}
			}
			// wave_min is in marine-equivalents (strength 2 each). The attacker walks
			// into the enemy's army PLUS its static defence, so "ahead" is measured
			// against both.
			int wave = L->wave_min * 2;
			if (b->enemy_expanded) wave += 12;
			bool ahead = M.str * L->ahead_num >= (E.str + E.sdef) * L->ahead_den;
			// ...but a stand-off must end. Measured after the strength rule went in:
			// 126 fighters against 79 and not one shot fired in 20000 frames, both
			// sides waiting to be "ahead". So: near the supply cap the army goes
			// (there is nothing left to wait for), and from ~10 minutes on parity is
			// enough -- the side with the bigger economy re-maxes faster.
			if (su >= 340) ahead = true;
			else if (frame > 14000 && M.str >= E.str + E.sdef) ahead = true;
			if (!b->rush && b->tel_threat == 0 && M.str >= wave && ahead && frame - b->last_attack_frame >= L->attack_cooldown) {
				int32_t ex = 0, ey = 0; bool got = false;
				if (b->enemy_expanded) {   // hit the enemy's newest base (the one farthest from their start)
					ob_unit* far_base = nullptr; int64_t bd = -1;
					for (int i = 0; i != ne; ++i) if (is_base(theirs[i].type)) { int64_t d = d2(theirs[i].x, theirs[i].y, est_x, est_y); if (d > bd) { bd = d; far_base = &theirs[i]; } }
					if (far_base) { ex = far_base->x; ey = far_base->y; got = true; }
				}
				if (!got && have_enemy_start) { ex = est_x; ey = est_y; got = true; }
				if (got) {
					b->attacking = true; b->atk_since = frame; b->atk_x = ex; b->atk_y = ey; b->last_army_order = frame;
					b->atk_peak = 0; b->atk_enemy0 = E.str;
					for (int i = 0; i != n; ++i) if (is_army(mine[i].type) && mine[i].completed) amove(mine[i].id, ex, ey);
					api->log(h, b->enemy_expanded ? "attack wave -> enemy expansion" : "attack wave");
				}
			}
		}

		// ---- unit-level micro (hard) ---------------------------------------------
		if (micro >= 2 && ne > 0 && api->abi_version >= 3 && api->unit_ex) {
			auto range_px = [](int t) -> int {
				switch (t) {
				case T_MARINE: case P_DRAGOON: case Z_HYDRA: case P_SCOUT: return 128;
				case T_VULTURE: case T_GOLIATH: case T_WRAITH: return 160;
				case T_TANK: return 224;
				case T_BATTLECRUISER: return 192;
				case Z_MUTA: return 96;
				case Z_GUARDIAN: return 256;
				default: return 0;
				}
			};
			auto max_life = [](int t) -> int {   // hp + shields of the units worth saving
				switch (t) {
				case P_DRAGOON: return 180; case T_TANK: return 150; case T_GOLIATH: return 125;
				case Z_MUTA: return 120; case T_VULTURE: case Z_HYDRA: return 80;
				default: return 0;
				}
			};
			auto slow_melee = [](int mine_t, int t) -> bool {
				bool melee = t == P_ZEALOT || t == Z_ZERGLING || t == T_FIREBAT || t == P_DT || t == Z_ULTRA;
				if (!melee) return false;
				if (mine_t == T_VULTURE) return true;                  // a vulture outruns everything on foot
				return mine_t == P_DRAGOON && (t == P_ZEALOT || t == T_FIREBAT || t == P_DT);
			};
			for (int i = 0; i != n; ++i) {
				ob_unit& u = mine[i];
				int r = range_px(u.type);
				if (!r || !u.completed) continue;
				ob_unit* weak = nullptr; int wl = 0; ob_unit* melee = nullptr; int64_t md = -1; bool any_near = false;
				for (int k = 0; k != ne; ++k) {
					ob_unit& e = theirs[k];
					if (!e.completed || !(is_army(e.type) || is_worker(e.type))) continue;
					int64_t d = d2(u.x, u.y, e.x, e.y);
					if (d < (int64_t)288 * 288) any_near = true;
					if (d <= (int64_t)(r + 24) * (r + 24)) { int life = e.hp + e.shields; if (!weak || life < wl) { weak = &e; wl = life; } }
					if (slow_melee(u.type, e.type) && d < (int64_t)224 * 224 && (!melee || d < md)) { melee = &e; md = d; }
				}
				if (!any_near) continue;
				int ml = max_life(u.type);
				bool wounded = ml && (u.hp + u.shields) * 100 < ml * 30;
				if (wounded && !at_home(u)) {   // save it: walk out of the fight
					if (u.order != 6) api->order_move(h, u.id, stage_x, stage_y);
					continue;
				}
				if (retreating) continue;
				ob_unit_ex ex;
				if (!api->unit_ex(h, u.id, &ex)) continue;
				if (melee) {
					// kite: step away while the weapon reloads, turn and fire when it is ready
					if (md < (int64_t)80 * 80 && ex.ground_cooldown > 5 && u.order != 6) {
						double kx = u.x - melee->x, ky = u.y - melee->y, kl = sqrt(kx * kx + ky * ky); if (kl < 1) { kx = stage_x - u.x; ky = stage_y - u.y; kl = sqrt(kx * kx + ky * ky); if (kl < 1) kl = 1; }
						int32_t px = u.x + (int32_t)(kx / kl * 96), py = u.y + (int32_t)(ky / kl * 96);
						if (px < 32) px = 32; if (py < 32) py = 32; if (px > mapw - 32) px = mapw - 32; if (py > maph - 32) py = maph - 32;
						api->order_move(h, u.id, px, py);
						continue;
					}
					if (u.order == 6 && ex.ground_cooldown <= 2) { api->order_right_click(h, u.id, (weak ? weak : melee)->id); continue; }
				}
				// focus fire: everyone in range shoots the weakest target
				if (weak && u.order != 6 && ex.target != weak->id) api->order_right_click(h, u.id, weak->id);
			}
		}
	}

	// ---- 9. abilities (ABI v5) -----------------------------------------------------
	// What a unit can DO beyond move / attack. Before this the bot's tanks never
	// sieged, a lurker (which only attacks burrowed) was a walking target, marines
	// never stimmed, templar never stormed and nothing was ever scanned. Levels:
	// medium and hard use them (the easy levels do not even tech that far).
	if (L->micro >= 1 && api->abi_version >= 5 && api->use_ability && api->cast_at && api->unit_ex) {
		// enemy GROUND things worth shooting at: fighters, workers, static defence
		static ob_unit* eg[700]; int neg = 0;        // ground fighters / workers
		static ob_unit* eb[256]; int neb = 0;        // buildings
		for (int k = 0; k != ne; ++k) {
			ob_unit& e = theirs[k];
			if (e.type >= 106 && e.type < 203) { if (neb < 256) eb[neb++] = &e; continue; }
			if (!e.completed || is_air_unit(e.type)) continue;
			if (is_army(e.type) || is_worker(e.type)) eg[neg++] = &e;
		}
		auto nearest_ground = [&](const ob_unit& u, bool with_buildings) -> int64_t {
			int64_t bd = -1;
			for (int k = 0; k != neg; ++k) { int64_t d = d2(u.x, u.y, eg[k]->x, eg[k]->y); if (bd < 0 || d < bd) bd = d; }
			if (with_buildings) for (int k = 0; k != neb; ++k) { int64_t d = d2(u.x, u.y, eb[k]->x, eb[k]->y); if (bd < 0 || d < bd) bd = d; }
			return bd;
		};

		// -- Terran: siege mode. Siege when something on the ground is inside the
		// siege range band (3..11 tiles), unsiege when nothing is within 14 tiles.
		if (b->race == R_TERRAN && (M.cnt[T_TANK] || M.cnt[T_TANK_SIEGED]) && api->tech_researched(h, me, TECH_SIEGE_MODE)) {
			for (int i = 0; i != n; ++i) {
				ob_unit& u = mine[i];
				if (!u.completed || (u.type != T_TANK && u.type != T_TANK_SIEGED)) continue;
				int64_t d = nearest_ground(u, true);
				if (u.type == T_TANK) {
					if (d >= (int64_t)96 * 96 && d <= (int64_t)352 * 352) { if (api->use_ability(h, u.id, OB_AB_SIEGE)) ++b->ab_siege; }
				} else if (d < 0 || d > (int64_t)448 * 448) api->use_ability(h, u.id, OB_AB_UNSIEGE);
			}
		}
		// -- Terran: stim. Only with medics behind them, only at (near) full health --
		// a stimmed marine is at 30 hp, so this re-stims exactly when it was healed.
		if (b->race == R_TERRAN && M.cnt[T_MEDIC] >= 2 && neg && api->tech_researched(h, me, TECH_STIM)) {
			for (int i = 0; i != n; ++i) {
				ob_unit& u = mine[i];
				if (!u.completed || !((u.type == T_MARINE && u.hp > 30) || (u.type == T_FIREBAT && u.hp > 40))) continue;
				int64_t d = nearest_ground(u, false);
				if (d >= 0 && d <= (int64_t)192 * 192) { if (api->use_ability(h, u.id, OB_AB_STIM)) ++b->ab_stim; }
			}
		}
		// -- Terran: scanner sweep on a cloaked unit that is next to ours with no
		// detector of ours near it.
		if (b->race == R_TERRAN && M.done[T_COMSAT] && frame - b->last_scan >= 300) {
			for (int k = 0; k != ne && frame - b->last_scan >= 300; ++k) {
				ob_unit& e = theirs[k];
				if (!(e.type == P_DT || e.type == Z_LURKER || e.type == T_GHOST || e.type == T_WRAITH) || !e.completed) continue;
				bool near_us = false, covered = false;
				for (int i = 0; i != n; ++i) {
					int64_t d = d2(mine[i].x, mine[i].y, e.x, e.y);
					if ((mine[i].type == T_TURRET || mine[i].type == T_SCIENCE_VESSEL) && mine[i].completed && d < (int64_t)224 * 224) { covered = true; break; }
					if (d < (int64_t)256 * 256 && (is_army(mine[i].type) || is_worker(mine[i].type) || is_base(mine[i].type))) near_us = true;
				}
				if (!near_us || covered) continue;
				for (int i = 0; i != n; ++i) {
					if (mine[i].type != T_COMSAT || !mine[i].completed) continue;
					ob_unit_ex ex;
					if (!api->unit_ex(h, mine[i].id, &ex) || ex.energy < 50) continue;
					if (api->cast_at(h, mine[i].id, OB_ORDER_SCANNER_SWEEP, e.x, e.y)) { b->last_scan = frame; ++b->ab_scan; break; }
				}
			}
		}
		// -- Zerg: lurkers attack only while burrowed. Burrow with ground enemies
		// inside their range (6 tiles), come up when nothing is within 10.
		if (b->race == R_ZERG && M.cnt[Z_LURKER]) {
			for (int i = 0; i != n; ++i) {
				ob_unit& u = mine[i];
				if (u.type != Z_LURKER || !u.completed) continue;
				ob_unit_ex ex;
				if (!api->unit_ex(h, u.id, &ex)) continue;
				int64_t d = nearest_ground(u, true);
				if (!ex.is_burrowed) { if (d >= 0 && d <= (int64_t)176 * 176 && u.order != 116) { if (api->use_ability(h, u.id, OB_AB_BURROW)) ++b->ab_burrow; } }
				else if (d < 0 || d > (int64_t)320 * 320) api->use_ability(h, u.id, OB_AB_UNBURROW);
			}
		}
		// -- Protoss: psionic storm on a clump (4+ enemies inside the storm, at most
		// one of ours), within 9 tiles of a templar with 75 energy. One storm per
		// spot: a second templar does not double up on the same clump.
		if (b->race == R_PROTOSS && M.cnt[P_HT] && neg >= 4 && api->tech_researched(h, me, TECH_PSI_STORM)) {
			for (int i = 0; i != n; ++i) {
				ob_unit& u = mine[i];
				if (u.type != P_HT || !u.completed) continue;
				ob_unit_ex ex;
				if (!api->unit_ex(h, u.id, &ex) || ex.energy < 75) continue;
				ob_unit* best = nullptr; int bc = 0;
				for (int k = 0; k != neg; ++k) {
					ob_unit& e = *eg[k];
					if (d2(u.x, u.y, e.x, e.y) > (int64_t)288 * 288) continue;
					if (frame - b->storm_frame < 96 && d2(e.x, e.y, b->storm_x, b->storm_y) < (int64_t)112 * 112) continue;
					int c = 0, own = 0;
					for (int q = 0; q != neg; ++q) if (d2(e.x, e.y, eg[q]->x, eg[q]->y) <= (int64_t)56 * 56) ++c;
					for (int q = 0; q != n; ++q) if (!is_air_unit(mine[q].type) && (is_army(mine[q].type) || is_worker(mine[q].type)) && d2(e.x, e.y, mine[q].x, mine[q].y) <= (int64_t)64 * 64) ++own;
					if (own <= 1 && c > bc) { best = &e; bc = c; }
				}
				if (best && bc >= 4 && api->cast_at(h, u.id, OB_ORDER_PSI_STORM, best->x, best->y)) {
					b->storm_frame = frame; b->storm_x = best->x; b->storm_y = best->y; ++b->ab_storm;
				}
			}
		}
	}

	b->bo_saving = saving;
	// DIAG: worker losses (measured L3 TvP: wk 15 -> 9 with no attack seen). Log
	// where the survivors are so the cause (scout, expansion site, enemy) shows.
	if (M.workers > b->workers_max_seen) b->workers_max_seen = M.workers;
	else if (M.workers + 2 <= b->workers_max_seen) {
		int far = 0, gasw = 0;
		for (int i = 0; i != n; ++i) if (mine[i].type == wt && mine[i].completed) { if (d2(mine[i].x, mine[i].y, base->x, base->y) > (int64_t)1200 * 1200) ++far; if (mine[i].order >= 81 && mine[i].order <= 84) ++gasw; }
		char s[160]; snprintf(s, sizeof s, "UA:workers dropped %d -> %d (far %d, gas-cycle %d, in-refinery hidden ~%d)", b->workers_max_seen, M.workers, far, gasw, M.done[gt] ? 1 : 0);
		api->log(h, s);
		b->workers_max_seen = M.workers;
	}
	if (frame % 720 == 0) {
		char s[260];
		snprintf(s, sizeof s, "UA f%d L%d %s vs %s bo %d want %d afford %d save %d hold %d cc %d rax %d near %d bord %d wk %d/%d army %d/%d bases %d/%d min %d gas %d sup %d/%d%s%s%s",
		         frame, b->level_id, race_name(b->race), race_name(b->enemy_race), b->bo_done, b->bo_want, b->bo_afford, b->bo_saving, b->tel_hold, b->tel_cc, b->tel_rax, b->tel_near, b->builder_order,
		         M.workers, want_workers, M.army, E.army, M.bases, E.bases,
		         minerals, gas, su, sa, b->rush ? " RUSH" : "", b->enemy_expanded ? " EXP" : "", b->enemy_air ? " AIR" : "");
		api->log(h, s);
		if (L->micro > 0) {
			snprintf(s, sizeof s, "UA:army f%d atk %d out %d facing %d target tile (%d,%d) defend_until %d threat %d | abilities siege %d stim %d burrow %d storm %d scan %d",
			         frame, (int)b->attacking, b->tel_out, b->tel_en_near, b->atk_x / 32, b->atk_y / 32, b->defend_until, b->tel_threat,
			         b->ab_siege, b->ab_stim, b->ab_burrow, b->ab_storm, b->ab_scan);
			api->log(h, s);
		}
	}
}
