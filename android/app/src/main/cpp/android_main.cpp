// OpenBW Android replay viewer entry point.
// Adapted from ui/gfxtest.cpp: same main loop, without the Emscripten
// bindings and the dlmalloc override. Saved states (for the replay
// slider seeking backwards) are capped instead of relying on a custom
// allocator to prune them on allocation failure.

#include "SDL.h"

#include <android/log.h>
#include <jni.h>

#include "ui.h"
#include "cine_hw.h"        // open1161HD Faze 4.7 Variant B: HW WebM cinematics
#include "casc.h"          // open1161HD Faze 2: read-only SC:R CASC source
#include "casc_inflate.h"  // stb_image-backed zlib inflate for BLTE 'Z' (zero-dep)
#include "common.h"
#include "bwgame.h"
#include "replay.h"
// LAN (v0.6) build/platform probe: pulls in the sync layer + standalone
// asio to verify they compile and link on Android. Must live in this TU
// (the OpenBW headers define non-inline globals that can only appear once).
#include "sync.h"
#include "sync_server_asio_tcp.h"
#include "bot_api.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

using namespace bwgame;

using ui::log;

namespace {
FILE* log_file = nullptr;
bwgame::a_string log_file_path;
}

namespace bwgame {
namespace ui {

void log_str(a_string str) {
	__android_log_print(ANDROID_LOG_INFO, "OpenBW", "%s", str.c_str());
	if (!log_file && !log_file_path.empty()) log_file = fopen(log_file_path.c_str(), "wb");
	if (log_file) {
		fwrite(str.data(), str.size(), 1, log_file);
		fflush(log_file);
	}
}

void fatal_error_str(a_string str) {
	__android_log_print(ANDROID_LOG_ERROR, "OpenBW", "fatal error: %s", str.c_str());
	log("fatal error: %s\n", str);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW klaida", str.c_str(), nullptr);
	std::terminate();
}

}
}

namespace {

struct saved_state {
	state st;
	action_state action_st;
	std::array<apm_t, 12> apm;
	std::array<int, 12> units_lost;
};

// Seek states are saved every 10 game-seconds; each one is a full copy of
// the game state, so keep only this many and drop the most redundant one
// (same selection heuristic gfxtest used in its allocator hook).
constexpr size_t max_saved_states = 48;

struct main_t {
	ui_functions ui;

	main_t(game_player player) : ui(std::move(player)) {}

	std::chrono::high_resolution_clock clock;
	std::chrono::high_resolution_clock::time_point last_tick;

	// Live (melee) mode advances the simulation in real time instead of
	// replaying recorded actions; no seek states, no rewind.
	bool live_mode = false;
	int local_player = -1;
	int enemy_player = -1;
	bool game_over = false;
	bool ums_mode = false; // UMS: victory comes from map triggers only, not building counts
	bool game_won = false;
	// Network (lockstep) game: set to sync_functions::next_frame(server).
	// When set, pausing is disabled (a paused client would stall the peer's
	// lockstep barrier until the 60s timeout kicks somebody).
	std::function<void()> net_next_frame;
	// v0.54 (experimental FPS parity): non-blocking network pump + a predicate
	// that is true only when advancing one sim frame will NOT block on the
	// lockstep barrier. update_live() uses them to render the current frame
	// while waiting for the peer instead of stalling the whole loop. This never
	// changes which frames execute or when (the action stream is untouched), so
	// it cannot desync - it only inserts render-only iterations.
	std::function<void()> net_poll;
	std::function<bool()> net_ready;
	std::chrono::steady_clock::time_point last_net_advance;

	// --- v0.24 downloadable bot host ------------------------------------
	// A bot .so (bot_api.h ABI) plays one occupied slot. Its commands go
	// through the same action functions as touch input, so the sim stays
	// deterministic; replay/LAN routing is out of scope for the MVP.
	void* bot_lib = nullptr;
	void* bot_instance = nullptr;
	void (*bot_frame_f)(void*) = nullptr;
	void (*bot_destroy_f)(void*) = nullptr;
	int bot_player = -1;
	int bot_level = 2;   // --bot-level: 0 very easy .. 3 hard (bot_api_difficulty)
	// v1.0.2: the bot's commands in the replay's action encoding (bot_host_impl.h);
	// set while a game is being recorded.
	std::function<void(const uint8_t*, size_t)> bot_action_record;
	bool load_bot(const a_string& path, int slot);

	// DIAG Cat 5 bot-anomaly detector state (read-only; every emit is --diag gated).
	// bot_cmd_count is bumped by every order/train/build API call that passes its guard;
	// the coarse post-frame idle check compares bot_last_cmd_frame against now.
	int64_t bot_cmd_count = 0;
	uint32_t bot_last_cmd_frame = 0;
	uint32_t bot_idle_last_warn = 0;
	void diag_bot_idle_tick();

	a_map<int, std::unique_ptr<saved_state>> saved_states;

	// Keep only `limit` saved states, dropping the most redundant one each
	// iteration (the one closest to its neighbours in frame time). The first
	// and last states are always kept as seek anchors.
	void prune_to(size_t limit) {
		while (saved_states.size() > limit) {
			auto begin = std::next(saved_states.begin());
			auto end = std::prev(saved_states.end());
			auto best = begin;
			int best_score = std::numeric_limits<int>::max();
			for (auto i = begin; i != end; ++i) {
				int score = 0;
				for (auto i2 = begin; i2 != end; ++i2) {
					if (i2 != i) {
						int d = i2->first - i->first;
						score += d * d;
					}
				}
				if (score < best_score) {
					best_score = score;
					best = i;
				}
			}
			saved_states.erase(best);
		}
	}

	// The replay player draws a notch on its timeline for every rewind state.
	void export_seek_frames() {
		ui.replay_seek_frames.clear();
		for (auto& v : saved_states) ui.replay_seek_frames.push_back(v.first);
	}

	void prune_saved_states() {
		prune_to(max_saved_states);
		export_seek_frames();
	}

	// On SDL_APP_LOWMEMORY, drop roughly half of the cached seek states
	// (each is a full game-state copy, ~2-4 MB). Seeking still works, just
	// with coarser rewind granularity until new states accumulate.
	void drop_saved_states_on_low_memory() {
		if (saved_states.size() > 2) prune_to(saved_states.size() / 2);
		export_seek_frames();
	}

	// Live melee: advance the sim at game speed, no replay buffer, no
	// seeking. The local player's commands are injected directly into the
	// action functions elsewhere (touch handling) before next_frame runs.
	void update_live() {
		auto now = clock.now();
		auto tick_speed = std::chrono::milliseconds((fp8::integer(42) / ui.game_speed).integer_part());

		if (net_next_frame && ui.is_paused) ui.is_paused = false;
		if (ui.is_paused || ui.briefing_active) { // v0.26: frozen during briefing
			last_tick = now;
		} else {
			auto tick_t = now - last_tick;
			if (tick_t >= tick_speed * 16) {
				last_tick = now - tick_speed * 16;
				tick_t = tick_speed * 16;
			}
			auto tick_n = tick_speed.count() == 0 ? 128 : tick_t / tick_speed;
			// v0.54 (experimental): pump the network once up front so peer frame
			// counters are current before we decide whether we can advance.
			if (net_poll) net_poll();
			for (auto i = tick_n; i;) {
				--i;
				// FPS parity: in a net game, only advance when it won't block on
				// the lockstep barrier - otherwise stop the sim loop and let the
				// frame render (smooth) while we wait for the peer. A safety
				// fallback forces one (possibly blocking) advance if we have not
				// progressed for a while, so the game can never permanently stall.
				if (net_next_frame && net_ready) {
					bool ready = net_ready();
					if (!ready) {
						bool stalled = clock.now() - last_net_advance >= std::chrono::milliseconds(250);
						if (!stalled) break; // render current frame instead of blocking
					}
					last_tick += tick_speed;
					auto wait_t0 = clock.now();
					net_next_frame();
					// v0.64 item 11: what the HUD reports as latency is the time
					// this peer actually spends blocked on the lockstep barrier,
					// i.e. the delay the player feels, smoothed so the readout
					// does not flicker. It is not an ICMP ping and is not
					// labelled as one.
					{
						int wait_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - wait_t0).count();
						ui.net_latency_ms += (wait_ms - ui.net_latency_ms) / 8;
					}
					last_net_advance = clock.now();
				} else {
					last_tick += tick_speed;
					// Bot commands are injected before the frame advances, the
					// same ordering as local touch input.
					if (bot_instance && !net_next_frame) {
						// DIAG BOT_EXCEPTION (Cat 5b): the bot frame runs inside the outer
						// game-loop try (a throw silently ends the whole game). Wrap it so a
						// bot throw is emitted as structured telemetry first, then re-thrown to
						// preserve the existing "clean end -> back to setup" path (zero behavior
						// change; the emit is --diag gated).
						try {
							bot_frame_f(bot_instance);
						} catch (const std::exception& e) {
							if (bwgame::diag::enabled())
								bwgame::diag::emit((long)ui.st.current_frame, "bot", bwgame::diag::Sev::error,
									"BOT_EXCEPTION", "bot frame threw an exception",
									"\"what\":" + bwgame::diag::jval(e.what()));
							throw;
						} catch (...) {
							if (bwgame::diag::enabled())
								bwgame::diag::emit((long)ui.st.current_frame, "bot", bwgame::diag::Sev::error,
									"BOT_EXCEPTION", "bot frame threw a non-std exception", "\"what\":\"unknown\"");
							throw;
						}
						diag_bot_idle_tick(); // DIAG BOT_IDLE (Cat 5a): macro-stall canary
					}
					// SCBW2 auto-worker assist: injects the same recorded actions a
					// tapping player would, at the same pre-frame point as bot/touch
					// input. No-op unless qol.auto_worker is set (SP only).
					if (!net_next_frame) ui.qol_auto_worker_tick();
					if (net_next_frame) net_next_frame();
					else if (bwgame::diag::enabled() && (uint32_t)ui.st.current_frame > 300) {
							// DIAG SIM_FRAME_SLOW (one-off PERF_HITCH pin): render/asset/memory are
							// ruled out, so time the sim step -- a single next_frame >200ms pins the
							// ~1s stall to sim-core (a deterministic per-frame event at f~7049).
							int64_t _t = std::chrono::duration_cast<std::chrono::microseconds>(clock.now().time_since_epoch()).count();
							ui.state_functions::next_frame();
							int64_t dt = std::chrono::duration_cast<std::chrono::microseconds>(clock.now().time_since_epoch()).count() - _t;
							if (dt > 200000)
								bwgame::diag::emit((long)ui.st.current_frame, "perf", bwgame::diag::Sev::warn, "SIM_FRAME_SLOW",
									"a single next_frame blocked >200ms", "\"us\":" + std::to_string(dt) +
									",\"frame\":" + std::to_string(ui.st.current_frame));
						} else ui.state_functions::next_frame();
				}
				for (auto& v : ui.apm) v.update(ui.st.current_frame);
				if (i % 4 == 3 && clock.now() - now >= std::chrono::milliseconds(50)) break;
			}
			// Melee end: engine victory triggers, or (like mini-openbwapi)
			// no buildings left on one side.
			if (!game_over && local_player >= 0 && enemy_player >= 0 && ui.st.current_frame > 10
					&& local_player < (int)ui.st.building_counts.size() && enemy_player < (int)ui.st.building_counts.size()) {
				bool lost = ui.player_defeated(local_player) || (!ums_mode && ui.st.building_counts.at(local_player) == 0);
				bool won = !lost && (ui.player_won(local_player) || (!ums_mode && ui.st.building_counts.at(enemy_player) == 0));
				if ((won || lost) && !end_hold_started) {
					// v0.83.004: let the last transmission finish. A mission that
					// ends the instant its victory trigger fires cuts the closing
					// line off mid-word -- the trigger that speaks and the trigger
					// that wins are usually the same one. Hold until the voice and
					// its text are done, capped at 7 seconds so a stuck sound can
					// never strand the player in a finished mission.
					end_hold_started = true;
					end_hold_won = won;
					end_hold_deadline = clock.now() + std::chrono::seconds(7);
				}
				if (end_hold_started && !game_over
						&& (!ui.transmission_active() || clock.now() >= end_hold_deadline)) {
					won = end_hold_won;
					game_over = true;
					game_won = won;
					// v0.26: campaign progression - resolve Set Next Scenario
					// into the next mission dir and leave it for the launcher.
					if (won && !campaign_dir.empty()) {
						a_string next;
						size_t nsi = (size_t)ui.st.next_scenario_string;
						if (nsi && nsi < ui.game_st.map_strings.size()) {
							next = ui.game_st.map_strings[nsi];
							for (auto& c : next) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 32; }
						}
						a_string next_dir;
						if (!next.empty()) {
							if (next.find('/') != a_string::npos) next_dir = "campaign/" + next;
							else {
								size_t slash = campaign_dir.rfind('/');
								next_dir = campaign_dir.substr(0, slash + 1) + next;
							}
						}
						a_string path = campaign_data_dir + "/campaign-next.txt";
						FILE* f = fopen(path.c_str(), "wb");
						if (f) {
							fwrite(next_dir.data(), 1, next_dir.size(), f);
							fclose(f);
							log("campaign: next mission '%s'\n", next_dir.c_str());
						}
						// Post-mission cinematic, two sources checked in order:
						//  (1) a STORY-BEAT cinematic keyed on the beaten mission --
						//      the intermediate cutscenes (terran1/2/3, zerg1/2/3,
						//      protoss2/3) that the port never played (only the intro
						//      and the finale were wired, so the same clip appeared to
						//      repeat); and
						//  (2) the race FINALE when the campaign leaves this race
						//      (vanilla smk/<race>4.smk, Brood War x<race>).
						auto race_of = [](const a_string& dir) -> a_string {
							size_t p1 = dir.find('/');
							size_t p2 = dir.rfind('/');
							if (p1 == a_string::npos || p2 == a_string::npos || p2 <= p1) return "";
							return dir.substr(p1 + 1, p2 - p1 - 1);
						};
						a_string cur_race = race_of(campaign_dir);
						a_string mission = campaign_dir.substr(campaign_dir.rfind('/') + 1);
						// (1) Story-beat table -- placements taken from the StarCraft
						//     campaign flow (wiki episode walkthroughs). Terran is
						//     authoritative (Wasteland Patrol after Backwater Station,
						//     The Downing of Norad II after Revolution, Open Rebellion
						//     after The Big Push); Zerg/Protoss follow the same source.
						//     The finale (<race>4) is handled by (2) below.
						static const struct { const char* m; const char* cine; } beats[] = {
							{ "terran02",  "smk/terran1.smk" },   // Wasteland Patrol
							{ "terran05",  "smk/terran2.smk" },   // The Downing of Norad II
							{ "terran08",  "smk/terran3.smk" },   // Open Rebellion
							{ "zerg03",    "smk/zerg1.smk" },
							{ "zerg04",    "smk/zerg2.smk" },
							{ "zerg08",    "smk/zerg3.smk" },
							// Protoss (StarCraft Wiki, Episode III): three mid-episode clips + the
							// finale (protoss4, "The Death of the Overmind", handled by (2) below).
							// Previously 03->protoss2 / 05->protoss3 and no 06 entry -- an
							// off-by-one inherited from the era when protoss1 was (mis)used as the
							// intro; it orphaned protoss1 and skipped "The Warp" entirely.
							{ "protoss03", "smk/protoss1.smk" },  // The Fall of Fenix (after Higher Ground)
							{ "protoss05", "smk/protoss2.smk" },  // The Ambush (after Choosing Sides)
							{ "protoss06", "smk/protoss3.smk" },  // The Warp / Return to Aiur (after Into the Darkness)
						};
						for (auto& b : beats) {
							if (mission == b.m) { pending_cinematic = b.cine; break; }
						}
						// (2) Race finale on race change / campaign end.
						if (pending_cinematic.empty() && !cur_race.empty()
						    && (next_dir.empty() || race_of(next_dir) != cur_race)) {
							pending_cinematic = cur_race.substr(0, 3) == "exp"
							    ? "smk/x" + cur_race.substr(3) + ".smk"
							    : "smk/" + cur_race + "4.smk";
						}
					}
				}
			}
		}
		ui.update();
	}
	// End-of-mission hold (v0.83.004), see update_live().
	bool end_hold_started = false;
	bool end_hold_won = false;
	std::chrono::high_resolution_clock::time_point end_hold_deadline;

	a_string campaign_dir;      // v0.26: set for campaign missions (progression)
	a_string campaign_data_dir; // external files dir for campaign-next.txt
	a_string pending_cinematic; // race finale smk, played before the end screen
	a_string intro_cinematic;   // campaign intro smk, played before frame 0

	void update() {
		if (live_mode) {
			update_live();
			return;
		}
		auto now = clock.now();

		auto tick_speed = std::chrono::milliseconds((fp8::integer(42) / ui.game_speed).integer_part());

		auto next = [&]() {
			int save_interval = 10 * 1000 / 42;
			if (ui.st.current_frame == 0 || ui.st.current_frame % save_interval == 0) {
				auto i = saved_states.find(ui.st.current_frame);
				if (i == saved_states.end()) {
					auto v = std::make_unique<saved_state>();
					v->st = copy_state(ui.st);
					v->action_st = copy_state(ui.action_st, ui.st, v->st);
					v->apm = ui.apm;
					v->units_lost = ui.units_lost;
					saved_states[ui.st.current_frame] = std::move(v);
					prune_saved_states();
				}
			}
			ui.replay_functions::next_frame();
			for (auto& v : ui.apm) v.update(ui.st.current_frame);
		};

		if (!ui.is_done() || ui.st.current_frame != ui.replay_frame) {
			if (ui.st.current_frame != ui.replay_frame) {
				auto i = saved_states.lower_bound(ui.replay_frame);
				if (i != saved_states.begin()) --i;
				if (i != saved_states.end()) {
					auto& v = i->second;
					if (ui.st.current_frame > ui.replay_frame || v->st.current_frame > ui.st.current_frame) {
						ui.st = copy_state(v->st);
						ui.action_st = copy_state(v->action_st, v->st, ui.st);
						ui.apm = v->apm;
						ui.units_lost = v->units_lost;
					}
				}
				if (ui.st.current_frame < ui.replay_frame) {
					for (size_t i = 0; i != 32 && ui.st.current_frame != ui.replay_frame; ++i) {
						for (size_t i2 = 0; i2 != 4 && ui.st.current_frame != ui.replay_frame; ++i2) {
							next();
						}
						if (clock.now() - now >= std::chrono::milliseconds(50)) break;
					}
				}
				last_tick = now;
			} else {
				if (ui.is_paused) {
					last_tick = now;
				} else {
					auto tick_t = now - last_tick;
					if (tick_t >= tick_speed * 16) {
						last_tick = now - tick_speed * 16;
						tick_t = tick_speed * 16;
					}
					auto tick_n = tick_speed.count() == 0 ? 128 : tick_t / tick_speed;
					for (auto i = tick_n; i;) {
						--i;
						last_tick += tick_speed;
						if (!ui.is_done()) next();
						else break;
						if (i % 4 == 3 && clock.now() - now >= std::chrono::milliseconds(50)) break;
					}
					ui.replay_frame = ui.st.current_frame;
				}
			}
		}

		ui.update();
	}
};

bool file_exists(const bwgame::a_string& path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// --- v0.24 bot host: C ABI table over the live ui action functions -------

// The ABI functions themselves live in bot_host_impl.h (shared with the headless
// tools/botmatch harness); only the DIAG hooks below are app-specific.
#define BOT_HOST_T main_t
#define BOT_HOST_LOG(...) log(__VA_ARGS__)
static main_t* bot_m(ob_bot_host* h);

// DIAG Cat 5 bot-command boundary helpers. A command that clears its guard bumps the
// activity counter (feeds BOT_IDLE); a rejected command emits BOT_INVALID_CMD (warn) so a
// silent drop at the host-API boundary becomes greppable instead of vanishing. All emits
// are --diag gated, so a normal run pays only the counter bump + a null-sink branch.
static inline void diag_bot_cmd(main_t& m) {
	++m.bot_cmd_count;
	m.bot_last_cmd_frame = (uint32_t)m.ui.st.current_frame;
}
static inline void diag_bot_invalid(main_t& m, const char* api, const char* reason,
                                    const std::string& extra = "") {
	if (!bwgame::diag::enabled()) return;
	bwgame::diag::emit((long)m.ui.st.current_frame, "bot", bwgame::diag::Sev::warn,
		"BOT_INVALID_CMD", "bot command rejected at the host-API boundary",
		"\"api\":\"" + std::string(api) + "\",\"reason\":\"" + std::string(reason) + "\""
		+ (extra.empty() ? "" : "," + extra));
}
// DIAG BOT_STALE_UNIT (Cat 5d): the analog of the shim UAF fixed in 1cf2400. get_unit(get_unit_id(u))
// returns the SAME unit for a live one but null for a dead/freed pool slot (get_unit checks unit_dead
// + the id generation), so a mismatch means the bot passed a stale Unit*. Pure read-only observer
// (emits, never changes behavior); --diag gated + only reached after the null/owner check, so u is
// already a safe deref. Post-1cf2400 this should never fire -> a regression canary.
static inline void diag_bot_stale(main_t& m, unit_t* u, const char* api) {
	if (!bwgame::diag::enabled() || !u) return;
	if (m.ui.get_unit(m.ui.get_unit_id(u)) == u) return; // live -> round-trips to itself
	bwgame::diag::emit((long)m.ui.st.current_frame, "bot", bwgame::diag::Sev::error,
		"BOT_STALE_UNIT", "bot ordered a dead/freed unit (stale pointer)", "\"api\":\"" + std::string(api) + "\"");
}
// DIAG BOT_IDLE (Cat 5a): the machine-readable form of the macro-stall once chased by
// hand. Coarse cadence (every 30f). A bot that has issued NO order/train/build for IDLE_FRAMES
// *while it still has idle workers* is genuinely stalled -- the idle-worker gate keeps a bot
// that's mid-attack (all workers mining, army already moving) from false-firing. Throttled to
// one warn per window. Read-only; --diag gated.
void main_t::diag_bot_idle_tick() {
	if (!bwgame::diag::enabled() || bot_player < 0) return;
	uint32_t f = (uint32_t)ui.st.current_frame;
	if (f % 30 != 0 || f < 300) return;                 // cadence + startup warmup
	const uint32_t IDLE_FRAMES = 480;                   // ~15 s at fastest speed
	if (f - bot_last_cmd_frame < IDLE_FRAMES) return;
	if (f - bot_idle_last_warn < IDLE_FRAMES) return;   // throttle
	int idle_workers = 0;
	for (auto i = ui.st.player_units[bot_player].begin(); i != ui.st.player_units[bot_player].end(); ++i) {
		unit_t* u = &*i;
		if (!ui.ut_worker(u) || ui.u_gathering(u)) continue;
		int oid = u->order_type ? (int)u->order_type->id : -1;
		if (oid == (int)Orders::PlayerGuard || oid == (int)Orders::Guard
		    || oid == (int)Orders::Nothing || oid == (int)Orders::ResetCollision) ++idle_workers;
	}
	if (idle_workers <= 0) return;                      // no idle workers => not a macro-stall
	bot_idle_last_warn = f;
	bwgame::diag::emit((long)f, "bot", bwgame::diag::Sev::warn, "BOT_IDLE",
		"bot issued no command for " + std::to_string(f - bot_last_cmd_frame) + " frames with idle workers",
		"\"player\":" + std::to_string(bot_player) + ",\"frames_idle\":" + std::to_string(f - bot_last_cmd_frame) +
		",\"idle_workers\":" + std::to_string(idle_workers) +
		",\"minerals\":" + std::to_string(ui.st.current_minerals[bot_player]));
}
#include "bot_host_impl.h"

// v0.58: a bot may declare the race it plays via the optional export
// `int openbw_bot_race(void)` (0 Zerg, 1 Terran, 2 Protoss; anything else =
// "any race"). Queried before the map loads, because starting units are
// created from the slot race - a race-specific bot dropped on the wrong race
// finds none of its buildings and sits idle for the whole game (which is
// exactly what happened to SteamLite before this existed). Bots without the
// export return -1 and keep the launcher's slot race.
static int query_bot_race(const a_string& path) {
	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (!h) return -1;
	auto race_f = (int (*)())dlsym(h, "openbw_bot_race");
	int r = race_f ? race_f() : -1;
	dlclose(h);
	return r >= 0 && r <= 2 ? r : -1;
}

bool main_t::load_bot(const a_string& path, int slot) {
	bot_lib = dlopen(path.c_str(), RTLD_NOW);
	if (!bot_lib) {
		log("bot: dlopen failed: %s\n", dlerror());
		return false;
	}
	auto abi_f = (int (*)())dlsym(bot_lib, "openbw_bot_abi");
	auto create_f = (void* (*)(ob_bot_host*, const ob_host_api*))dlsym(bot_lib, "openbw_bot_create");
	bot_frame_f = (void (*)(void*))dlsym(bot_lib, "openbw_bot_frame");
	bot_destroy_f = (void (*)(void*))dlsym(bot_lib, "openbw_bot_destroy");
	if (!abi_f || !create_f || !bot_frame_f) {
		log("bot: missing exports in %s\n", path.c_str());
		return false;
	}
	int bot_abi = abi_f();
	if (bot_abi < 1 || bot_abi > OPENBW_BOT_ABI_VERSION) {
		log("bot: ABI mismatch (%d, host %d)\n", bot_abi, OPENBW_BOT_ABI_VERSION);
		return false;
	}
	bot_player = slot;
	bot_instance = create_f((ob_bot_host*)this, &bot_host_api_table);
	log("bot: %s loaded, playing slot %d\n", path.c_str(), slot);
	// Re-snapshot module bases so a crash inside the just-loaded bot .so resolves to
	// name+offset in the crash file (bots have a documented SIGSEGV history).
	bwgame::diag::refresh_modules();
	return bot_instance != nullptr;
}

bwgame::a_string find_first_replay(const bwgame::a_string& dir) {
	bwgame::a_string r;
	DIR* d = opendir(dir.c_str());
	if (!d) return r;
	while (dirent* e = readdir(d)) {
		bwgame::a_string name = e->d_name;
		if (name.size() > 4 && name.substr(name.size() - 4) == ".rep") {
			r = dir + "/" + name;
			break;
		}
	}
	closedir(d);
	return r;
}

}

// LAN spike probe: bind an asio TCP acceptor and poll once. Proves the sync
// transport builds and runs on Android before the full lockstep
// integration. Throwaway; remove once real networking lands.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_openbw_android_NetProbe_probe(JNIEnv*, jclass, jint port) {
	try {
		bwgame::sync_server_asio_tcp server;
		server.bind(bwgame::a_string("0.0.0.0"), (int)port);
		server.io_service.poll();
		bwgame::ui::log("net_probe: asio bind on port %d OK\n", (int)port);
		return JNI_TRUE;
	} catch (const std::exception& e) {
		bwgame::ui::log("net_probe failed: %s\n", e.what());
		return JNI_FALSE;
	}
}

// v0.41 Android-overlay lobby bridge. The native LAN lobby loop (SDL thread)
// publishes a JSON snapshot of lobby state here; the Java Activity (UI thread)
// polls it and renders a launcher-style lobby overlay on top of the game
// surface, and pushes the local player's ready/leave actions back. All access
// under g_lobby.mtx.
namespace {
struct lobby_bridge_t {
	std::mutex mtx;
	bool active = false;
	std::string state_json = "{}";
	int pending_ready = -1;   // -1 none, 0/1 desired ready state
	bool pending_leave = false;
};
lobby_bridge_t g_lobby;

std::string json_esc(const std::string& s) {
	std::string r;
	for (char c : s) {
		if (c == '"' || c == '\\') { r += '\\'; r += c; }
		else if ((unsigned char)c < 0x20) r += ' ';
		else r += c;
	}
	return r;
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_openbw_android_OpenBWActivity_nativeLobbyActive(JNIEnv*, jclass) {
	std::lock_guard<std::mutex> lk(g_lobby.mtx);
	return g_lobby.active ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_OpenBWActivity_nativeLobbyStateJson(JNIEnv* env, jclass) {
	std::string s;
	{ std::lock_guard<std::mutex> lk(g_lobby.mtx); s = g_lobby.state_json; }
	return env->NewStringUTF(s.c_str());
}
extern "C" JNIEXPORT void JNICALL
Java_org_openbw_android_OpenBWActivity_nativeLobbySetReady(JNIEnv*, jclass, jboolean r) {
	std::lock_guard<std::mutex> lk(g_lobby.mtx);
	g_lobby.pending_ready = r ? 1 : 0;
}
extern "C" JNIEXPORT void JNICALL
Java_org_openbw_android_OpenBWActivity_nativeLobbyLeave(JNIEnv*, jclass) {
	std::lock_guard<std::mutex> lk(g_lobby.mtx);
	g_lobby.pending_leave = true;
}

// v0.83: how much of the game surface the soft keyboard covers at the bottom
// (v0.125: in 1/10000 of the surface height -- the render buffer is not the
// device resolution). SDL does not resize its surface when the IME opens, so without this
// the chat input line is drawn underneath the keyboard and you cannot see what
// you are typing (found on the S8 Ultra while verifying the chat IME fix).
// Written from the Android UI thread, read from the game thread -- an int, and
// a stale value for one frame is harmless.
std::atomic<int> g_ime_frac{0};

// HUD v3: the S-Pen hovering (OpenBWActivity.installPenHover). Written from the
// Android UI thread, read by the game loop once per frame; the button edge is
// what matters, and a state that is one frame late is harmless.
std::atomic<int> g_pen_air_in{0}, g_pen_air_x{0}, g_pen_air_y{0}, g_pen_air_btn{0};

extern "C" JNIEXPORT void JNICALL
Java_org_openbw_android_OpenBWActivity_nativePenAir(JNIEnv*, jclass, jint in_range, jint xf, jint yf, jint button) {
	g_pen_air_x.store(xf);
	g_pen_air_y.store(yf);
	g_pen_air_btn.store(button);
	g_pen_air_in.store(in_range);
}

extern "C" JNIEXPORT void JNICALL
Java_org_openbw_android_OpenBWActivity_nativeSetImeCover(JNIEnv*, jclass, jint frac) {
	g_ime_frac.store(frac < 0 ? 0 : frac > 10000 ? 10000 : frac);
}

// LAN discovery (v0.6-2). The host, while sitting in the lobby, answers UDP
// probes so the joining tablet's setup screen can auto-list it instead of the
// user typing an IP. Protocol (VCMI recipe): the client broadcasts the ASCII
// string "OPENBW_DISCOVER" to UDP DISCOVERY_PORT; the host replies (unicast)
// with "OPENBW\t<tcp_port>\t<map_basename>". Plain POSIX sockets (non-blocking)
// so it drops straight into the lobby poll loop without asio or a thread.
namespace {
constexpr int DISCOVERY_PORT = 6112;  // UDP; independent of the TCP game port

struct udp_responder {
	int fd = -1;
	a_string reply;
	a_string base; // "OPENBW\t<port>\t<map>"
	a_string info; // "\t<cur>/<target>\t<ver>\t<mods>" (v0.26 lobby browser)

	// v0.26: richer lobby announcements - joiners see player counts,
	// version and active mods in the scan list before connecting.
	void set_players(int cur, int target, const a_string& ver, const a_string& mods, int game_speed) {
		char buf[48];
		std::snprintf(buf, sizeof(buf), "\t%d/%d\t", cur, target);
		char spd[16];
		std::snprintf(spd, sizeof(spd), "\t%d", game_speed);
		info = a_string(buf) + ver + "\t" + (mods.empty() ? a_string("-") : mods) + spd;
		a_string old = reply;
		reply = base + info;
		if (fd >= 0 && reply != old) ui::log("discovery: reply='%s'\n", reply.c_str());
	}

	// tcp_port = the game's TCP port; label = short map name shown to joiners.
	void open(int tcp_port, const a_string& label) {
		fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0) return;
		int one = 1;
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons((uint16_t)DISCOVERY_PORT);
		if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
			::close(fd);
			fd = -1;
			return;
		}
		int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "OPENBW\t%d\t", tcp_port);
		base = a_string(buf) + label;
		reply = base + info;
		ui::log("discovery: listening on udp/%d, reply='%s'\n", DISCOVERY_PORT, reply.c_str());
	}

	// Non-blocking: answer every pending probe, then return.
	void poll() {
		if (fd < 0) return;
		char buf[64];
		sockaddr_in from{};
		socklen_t fromlen = sizeof(from);
		for (;;) {
			ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
			if (n <= 0) break;
			buf[n] = 0;
			if (std::strncmp(buf, "OPENBW_DISCOVER", 15) == 0) {
				::sendto(fd, reply.data(), reply.size(), 0, (sockaddr*)&from, fromlen);
			}
		}
	}

	void close() {
		if (fd >= 0) { ::close(fd); fd = -1; }
	}
};
}  // namespace

// Parse "0,0,1,1" into per-slot team ids. Default (empty) = FFA: each slot is
// its own team, so every player is an enemy.
static std::array<int, 12> parse_teams(const a_string& csv) {
	std::array<int, 12> teams;
	for (int i = 0; i < 12; ++i) teams[i] = i;
	if (csv.empty()) return teams;
	int slot = 0, val = 0; bool have = false;
	for (size_t i = 0; i <= csv.size(); ++i) {
		char c = i < csv.size() ? csv[i] : ',';
		if (c == ',') { if (have && slot < 12) teams[slot] = val; ++slot; val = 0; have = false; }
		else if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); have = true; }
	}
	return teams;
}

// Set alliances + shared vision among the active slots from the team map. Same
// team -> allies (alliance 2 + shared vision); different -> enemies. Applied
// identically on every client (from the same team config) so LAN stays in sync.
template<typename State>
static void apply_team_alliances(State& st, const bwgame::static_vector<size_t, 12>& active, const std::array<int, 12>& teams) {
	for (size_t a : active) {
		for (size_t b : active) {
			if (a == b) continue;
			bool ally = teams[a] == teams[b];
			st.alliances[a][b] = ally ? 2 : 0;
			if (ally) st.shared_vision[a] |= (1u << (uint32_t)b);
		}
	}
}

// zlib inflate provider for SC:R ("seRS") replays, backed by stb_image's zlib
// (pulled in via casc_inflate.h). replay.h keeps this injectable so the sim-core
// never depends on a zlib impl; classic ("reRS") replays never call it.
static bool android_scr_replay_inflate(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
	int n = stbi_zlib_decode_buffer((char*)out, (int)out_size, (const char*)in, (int)in_size);
	return n == (int)out_size;
}

extern "C" int main(int argc, char* argv[]) {

	using namespace bwgame;

	// Enable loading SC:R (Remastered, "seRS") replays -- zlib-compressed container.
	data_loading::scr_replay_inflate() = &android_scr_replay_inflate;

	// Touch handling is done natively in ui/sdl2.cpp (finger events),
	// so turn off SDL's synthetic touch<->mouse translation entirely.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

	const char* storage = SDL_AndroidGetExternalStoragePath();
	if (!storage) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW", "Nepavyko gauti programos duomenu aplanko.", nullptr);
		return 1;
	}
	a_string data_dir = storage;
	log_file_path = data_dir + "/openbw-log.txt";

	for (const char* fn : {"StarDat.mpq", "BrooDat.mpq", "Patch_rt.mpq"}) {
		if (!file_exists(data_dir + "/" + fn)) {
			a_string msg = a_string("Truksta failo: ") + fn + "\nPaleiskite nustatymu ekrana ir importuokite MPQ failus.";
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW", msg.c_str(), nullptr);
			return 1;
		}
	}

	a_string replay_path;
	a_string map_path;
	a_string map_mpq;             // v0.35: bundled map picked from the MPQ chain (internal path)
	a_string net_join;
	bool net_host = false;
	bool net_fast = false; // v0.46: launcher lobby already agreed - auto-start on connect
	int net_port = 6112;
	int volume = 50;
	int music_volume = 40;
	int sfx_volume = 100;    // Faze 7+: per-category submixes (0..100 of master)
	int voice_volume = 100;
	int ui_volume = 100;
	int voice_freq = 100;    // unit voice-response frequency (0..100)
	int render_scale = 2;
	int local_race = 5;  // 5 = pick random from the map slot
	int player_color = -1; // 0..7 chosen BW colour for the local player; -1 = slot default (render-only)
	int enemy_race = 5;
	int show_queue = 1;  // HUD QUEUE toggle visibility (Settings > Controls)
	int game_speed_pct = 100; // v0.45 default game speed (100 = normal/BW fast)
	// SCBW2 house-rules bitmask (bit0 multi_building_select, 1 group_rally,
	// 2 mbs_train, 3 unlimited_selection, 4 smart_cast, 5 auto_worker). bit6 was
	// auto_supply -- abandoned by design.
	// 0 = vanilla BW. Applied to game_state::qol after load, non-replay only.
	int qol_bits = 0;
	int ui_legacy = 1;   // 1 = legacy tconsole HUD, 0 = native HUD v2
	a_string app_version; // versionName, shown in HUD v2
	a_string version_label_path; // the same label as an image in the launcher's font (Java renders it)
	// HUD v3, the touch HUD (Settings > Interface): --hud v3 and its options.
	bool hud_v3 = false;
	int hud3_template = 0, hud3_dock = 0, hud3_scale = 100, hud3_wheel_ms = 300;
	bool hud3_left = false, hud3_pen_wheel = true, hud3_vibrate = true, hud3_labels = false, hud3_allgroups = false, hud3_latch = false;
	int density_milli = 0;   // DisplayMetrics.density x 1000 (dp -> device px)
	int version_label_screen_h = 0;
	int player_count = 2; // single-device melee: number of players to activate (2-8)
	a_string teams_csv;   // per-slot team id (e.g. "0,0,1,1" = 2v2); empty = FFA
	int team_mode = 0;    // 0 = FFA, 1 = two teams (native splits active slots in half)
	a_string mods_csv;    // active mod ids (files/mods/<id>/), later = higher priority
	a_string casc_dir;    // open1161HD Faze 2: SC:R install root (holds data/data); enables read-only CASC asset source. OFF unless set.
	a_string game_mode = "melee"; // melee | ums | <trg name: ctf/greed10000/slaughter30/suddendeath/...>
	a_string campaign_dir;        // e.g. "campaign/terran/terran01" - mission loaded from the MPQ chain
	int vs_ai = 0;                // melee: non-local active slots run the aiscript melee AI
	int rgba_sprites = 0;         // v0.22: true-color sprite pass (HD foundation)
	a_string bot_path;            // v0.24: bot .so playing the enemy slot (overrides vs-ai)
	a_string player_name;         // v0.26: UserID profile, shown as the local player's "Name"
	int player_portrait = -1;     // launcher redesign F4: the profile's portdata id (render-only)
	a_string name_portraits_arg;  // LAN: "id=name|id=name" from the launcher lobby (render-only)
	a_string slot_races_csv;      // v0.43: per active slot race (0 Z,1 T,2 P, else random); index 0 = local
	a_string slot_colors_csv;     // per active slot player colour (0..7); index 0 = local (render-only)
	bool trigger_trace = false;   // v0.62: --trigger-trace, campaign trigger diagnosis
	bool diag_on = false;         // --diag: in-engine bug-catcher (DIAG), read-only
	bool scale_filter_linear = false; // open1161HD Faze 4.1: --scale-filter linear -> bilinear logical->physical upscale (A/B; default nearest)
	int zoom_buttons = 0;         // v0.64: on-screen zoom -/+ buttons (off by default)
	int no_fog = 0;               // v0.64: dev switch, skip drawing the fog overlay
	int resource_counts = 1;      // v0.77: remaining minerals/gas over the world
	int equip_grey = 1;           // v0.82: Equipment icons desaturated over any icon pack
	int saturation = 1;           // v0.82: worker saturation, split from resource_counts
	int bloom_strength = 0;       // open1161HD Faze 7: GPU bloom additive alpha (0 = off)
	bool post_gl_on = false;      // SP shader pipeline (GLES2 present stage)
	int post_effect = -1;         // SP effect: -1 AUTO (sharpen on Adreno), 0 identity, 1 sharpen
	int post_strength = 35;       // SP effect strength 0..100
	int gl_bloom = 0;             // SP Phase 3: real GPU bloom through the GL chain 0..100
	int gl_grade = 0;             // SE: cinematic color-grade 0..100
	int emissive_on = 1;          // open1161HD Faze 7: SC:R emissive self-glow (default on)
	int soft_shadows = 0;         // open1161HD Faze 7: softer unit shadows (default off)
	int sun_shadow = 0;           // SHADOWS Phase 1: directional sun shadow (0 = off)
	int sun_shadow_len = 40;      // sun-shadow lean length 0..100
	int sun_shadow_sign = 1;      // sun azimuth sign: +1 = cast right/SE, -1 = left
	int point_lights = 0;         // SHADOWS Phase 2: dynamic point lights (0 = off)
	int light_strength = 100;     // point-light intensity/radius 0..200 %
	bool hd_anim_diag = false;    // DIAG: log each image's HD-anim load result
	int show_fps = 1;             // Faze 7+: HUD fps readout (Settings > Sąsaja)
	int tap_command = 1;          // modern touch: plain tap = contextual order (Settings > Valdymas)
	int render_interp = 1;        // LAN: blend sprite positions between lockstep ticks (net-only, pure render)
	int bot_level = 2;            // unified AI difficulty 0..3 (launcher "Sunkumas"), read by the bot via difficulty()
	int show_apm = 0;             // Faze 7+: HUD APM readout (default off)
	int health_always = 0;        // Faze 7+: draw health bars on all units, not just damaged
	int camera_speed = 100;       // Faze 7+: edge/key scroll speed, % of classic curve
	a_string lang = "en";         // v0.80: UI language for the screens we author
	a_string gfx_tier = "hd2";    // open1161HD Faze 4.3: sd|hd2|full HD graphics tier (CASC)
	a_string test_cinematic;      // open1161HD Faze 4.7 Variant B: --test-cinematic <smk\name.smk>
	a_string load_savefile;       // v0.86: --load <path> — apply a disk save after the map loads
	int version_code = 0;         // v0.86: --version-code <n> — app versionCode, gates save compat
	int autosave_frame = -1;      // v0.86 test hook: --autosave-at <frame> — write slot 1 then continue
	a_string diag_selftest;       // --diag-selftest <name>: scripted DIAG scenario at frame 40
	for (int i = 1; i < argc; ++i) {
		if (!argv[i] || !argv[i][0]) continue;
		a_string arg = argv[i];
		if (arg == "--volume" && i + 1 < argc) volume = atoi(argv[++i]);
		else if (arg == "--music-volume" && i + 1 < argc) music_volume = atoi(argv[++i]);
		else if (arg == "--sfx-volume" && i + 1 < argc) sfx_volume = atoi(argv[++i]);
		else if (arg == "--voice-volume" && i + 1 < argc) voice_volume = atoi(argv[++i]);
		else if (arg == "--ui-volume" && i + 1 < argc) ui_volume = atoi(argv[++i]);
		else if (arg == "--voice-freq" && i + 1 < argc) voice_freq = atoi(argv[++i]);
		else if (arg == "--scale" && i + 1 < argc) render_scale = atoi(argv[++i]);
		else if (arg == "--scale-filter" && i + 1 < argc) scale_filter_linear = (a_string(argv[++i]) == "linear");
		else if (arg == "--map" && i + 1 < argc) map_path = argv[++i];
		else if (arg == "--map-mpq" && i + 1 < argc) map_mpq = argv[++i];
		else if (arg == "--race" && i + 1 < argc) local_race = atoi(argv[++i]);
		else if (arg == "--player-color" && i + 1 < argc) player_color = atoi(argv[++i]); // 0..7, render-only
		else if (arg == "--enemy-race" && i + 1 < argc) enemy_race = atoi(argv[++i]);
		else if (arg == "--net-host") net_host = true;
		else if (arg == "--net-fast") net_fast = true;
		else if (arg == "--net-join" && i + 1 < argc) net_join = argv[++i];
		else if (arg == "--net-port" && i + 1 < argc) net_port = atoi(argv[++i]);
		else if (arg == "--show-queue" && i + 1 < argc) show_queue = atoi(argv[++i]);
		else if (arg == "--game-speed" && i + 1 < argc) game_speed_pct = atoi(argv[++i]);
		else if (arg == "--qol" && i + 1 < argc) qol_bits = atoi(argv[++i]); // SCBW2 house-rules bitmask
		else if (arg == "--ui-legacy" && i + 1 < argc) ui_legacy = atoi(argv[++i]);
		else if (arg == "--version" && i + 1 < argc) app_version = argv[++i];
		else if (arg == "--version-label" && i + 1 < argc) version_label_path = argv[++i];
		else if (arg == "--version-label-h" && i + 1 < argc) version_label_screen_h = atoi(argv[++i]);
		else if (arg == "--hud" && i + 1 < argc) hud_v3 = std::string(argv[++i]) == "v3";
		else if (arg == "--hud3-template" && i + 1 < argc) hud3_template = atoi(argv[++i]);
		else if (arg == "--hud3-dock" && i + 1 < argc) hud3_dock = atoi(argv[++i]);
		else if (arg == "--hud3-left" && i + 1 < argc) hud3_left = atoi(argv[++i]) != 0;
		else if (arg == "--hud3-scale" && i + 1 < argc) hud3_scale = atoi(argv[++i]);
		else if (arg == "--hud3-wheel-ms" && i + 1 < argc) hud3_wheel_ms = atoi(argv[++i]);
		else if (arg == "--hud3-pen" && i + 1 < argc) hud3_pen_wheel = std::string(argv[++i]) != "shift";
		else if (arg == "--hud3-vibrate" && i + 1 < argc) hud3_vibrate = atoi(argv[++i]) != 0;
		else if (arg == "--hud3-labels" && i + 1 < argc) hud3_labels = atoi(argv[++i]) != 0;
		else if (arg == "--hud3-allgroups" && i + 1 < argc) hud3_allgroups = atoi(argv[++i]) != 0;
		else if (arg == "--hud3-latch" && i + 1 < argc) hud3_latch = atoi(argv[++i]) != 0;
		else if (arg == "--density" && i + 1 < argc) density_milli = atoi(argv[++i]);
		else if (arg == "--players" && i + 1 < argc) player_count = atoi(argv[++i]);
		else if (arg == "--teams" && i + 1 < argc) teams_csv = argv[++i];
		else if (arg == "--team-mode" && i + 1 < argc) team_mode = atoi(argv[++i]);
		else if (arg == "--mods" && i + 1 < argc) mods_csv = argv[++i];
		else if (arg == "--casc" && i + 1 < argc) casc_dir = argv[++i];
		else if (arg == "--gfx-tier" && i + 1 < argc) gfx_tier = argv[++i];
		else if (arg == "--test-cinematic" && i + 1 < argc) test_cinematic = argv[++i];
		else if (arg == "--game-mode" && i + 1 < argc) game_mode = argv[++i];
		else if (arg == "--campaign" && i + 1 < argc) campaign_dir = argv[++i];
		else if (arg == "--vs-ai" && i + 1 < argc) vs_ai = atoi(argv[++i]);
		else if (arg == "--rgba-sprites" && i + 1 < argc) rgba_sprites = atoi(argv[++i]);
		else if (arg == "--bot" && i + 1 < argc) bot_path = argv[++i];
		else if (arg == "--player-name" && i + 1 < argc) player_name = argv[++i];
		else if (arg == "--player-portrait" && i + 1 < argc) player_portrait = atoi(argv[++i]);
		else if (arg == "--name-portraits" && i + 1 < argc) name_portraits_arg = argv[++i];
		else if (arg == "--races" && i + 1 < argc) slot_races_csv = argv[++i];
		else if (arg == "--colors" && i + 1 < argc) slot_colors_csv = argv[++i]; // per-slot player colours (render-only)
		else if (arg == "--trigger-trace") trigger_trace = true;
		// v0.81: same sink, different question -- which aiscript opcodes the
		// running scripts actually reach. Separate flag so an AI investigation
		// is not drowned in per-trigger condition dumps.
		else if (arg == "--ai-trace") trigger_trace = true;
		else if (arg == "--diag") diag_on = true;   // in-engine bug-catcher (DIAG)
		else if (arg == "--zoom-buttons" && i + 1 < argc) zoom_buttons = atoi(argv[++i]);
		else if (arg == "--no-fog" && i + 1 < argc) no_fog = atoi(argv[++i]);
		else if (arg == "--resource-counts" && i + 1 < argc) resource_counts = atoi(argv[++i]);
		else if (arg == "--equip-grey" && i + 1 < argc) equip_grey = atoi(argv[++i]);
		else if (arg == "--saturation" && i + 1 < argc) saturation = atoi(argv[++i]);
		else if (arg == "--bloom" && i + 1 < argc) bloom_strength = atoi(argv[++i]); // Faze 7 GPU bloom
		else if (arg == "--post-gl") post_gl_on = true; // SP shader pipeline (GLES2 present stage)
		else if (arg == "--post-effect" && i + 1 < argc) post_effect = atoi(argv[++i]); // -1 AUTO / 0 identity / 1 sharpen
		else if (arg == "--post-strength" && i + 1 < argc) post_strength = atoi(argv[++i]); // 0..100
		else if (arg == "--gl-bloom" && i + 1 < argc) gl_bloom = atoi(argv[++i]); // SP Phase 3 GPU bloom 0..100
		else if (arg == "--grade" && i + 1 < argc) gl_grade = atoi(argv[++i]); // SE color-grade 0..100
		else if (arg == "--emissive" && i + 1 < argc) emissive_on = atoi(argv[++i]);
		else if (arg == "--soft-shadows" && i + 1 < argc) soft_shadows = atoi(argv[++i]);
		else if (arg == "--sun-shadow" && i + 1 < argc) sun_shadow = atoi(argv[++i]); // directional sun shadow (0/1)
		else if (arg == "--sun-shadow-len" && i + 1 < argc) sun_shadow_len = atoi(argv[++i]); // 0..100
		else if (arg == "--sun-shadow-sign" && i + 1 < argc) sun_shadow_sign = atoi(argv[++i]); // +1/-1
		else if (arg == "--point-lights" && i + 1 < argc) point_lights = atoi(argv[++i]); // 0/1
		else if (arg == "--light-strength" && i + 1 < argc) light_strength = atoi(argv[++i]); // 0..200
		else if (arg == "--hd-anim-diag") hd_anim_diag = true; // DIAG: log HD-anim load results
		else if (arg == "--show-fps" && i + 1 < argc) show_fps = atoi(argv[++i]);
		else if (arg == "--show-apm" && i + 1 < argc) show_apm = atoi(argv[++i]);
		else if (arg == "--health-always" && i + 1 < argc) health_always = atoi(argv[++i]);
		else if (arg == "--camera-speed" && i + 1 < argc) camera_speed = atoi(argv[++i]);
		else if (arg == "--tap-command" && i + 1 < argc) tap_command = atoi(argv[++i]);
		else if (arg == "--render-interp" && i + 1 < argc) render_interp = atoi(argv[++i]);
		else if (arg == "--bot-level" && i + 1 < argc) { bot_level = atoi(argv[++i]); if (bot_level < 0) bot_level = 0; if (bot_level > 3) bot_level = 3; }
		else if (arg == "--lang" && i + 1 < argc) lang = argv[++i];
		else if (arg == "--load" && i + 1 < argc) load_savefile = argv[++i];
		else if (arg == "--version-code" && i + 1 < argc) version_code = atoi(argv[++i]);
		else if (arg == "--autosave-at" && i + 1 < argc) autosave_frame = atoi(argv[++i]);
		else if (arg == "--diag-selftest" && i + 1 < argc) diag_selftest = argv[++i];
		else if (replay_path.empty()) replay_path = arg;
	}
	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;
	if (trigger_trace) bwgame::trigger_trace_sink() = [](const bwgame::a_string& s) { ui::log_str(s); };
	if (diag_on) {
		// Structured events land on the dedicated "OBDIAG" logcat tag and mirror to
		// files/diag-session.jsonl (adb-pullable), next to openbw-log.txt.
		bwgame::diag::sink().set_enabled(true);
		std::string dp = std::string(log_file_path.c_str());
		size_t sl = dp.find_last_of('/');
		dp = (sl == std::string::npos ? std::string() : dp.substr(0, sl + 1)) + "diag-session.jsonl";
		bwgame::diag::sink().set_file(dp);
		bwgame::diag::emit(0, "meta", bwgame::diag::Sev::info, "SESSION_START",
			"diag session start", "\"build\":\"open1161HD\"");
	}
	// Crash capture is installed UNCONDITIONALLY (independent of --diag): even with
	// diag off it yields header + backtrace + module map, which beats today's zero
	// context on a native crash. Only the event/frame breadcrumbs need --diag.
	{
		std::string cp = std::string(log_file_path.c_str());
		size_t sl = cp.find_last_of('/');
		cp = (sl == std::string::npos ? std::string() : cp.substr(0, sl + 1)) + "openbw-crash.jsonl";
		bwgame::diag::install_crash_handler(cp.c_str());
	}
	{
		a_string all_args;
		for (int i = 1; i < argc; ++i) {
			if (!argv[i]) continue;
			if (!all_args.empty()) all_args += ' ';
			all_args += argv[i];
		}
		log("argv: %s\n", all_args.c_str());
	}
	// open1161HD Faze 4.3: FULL HD renders the world at physical resolution -
	// force render_scale=1 (buffer == screen) so world_scale=2 supersampling has
	// the pixels to fill. SD/HD2 keep the user's Resolution choice.
	if (gfx_tier == "full") render_scale = 1;
	native_window::set_render_scale(render_scale);

	const bool campaign = !campaign_dir.empty();
	if (campaign) game_mode = "ums"; // campaign missions run with map settings
	const bool melee = !map_path.empty() || !map_mpq.empty() || campaign;
	if (!melee) {
		if (replay_path.empty()) replay_path = find_first_replay(data_dir + "/replays");
		if (replay_path.empty() || !file_exists(replay_path)) {
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW", "Nerastas replay (.rep) failas. Importuokite ji nustatymu ekrane.", nullptr);
			return 1;
		}
	} else if (!campaign && map_mpq.empty() && !file_exists(map_path)) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW", "Nerastas zemelapio failas.", nullptr);
		return 1;
	}

	log("OpenBW Android starting, data dir: %s\n", data_dir);
	if (melee) log("map: %s\n", map_path);
	else log("replay: %s\n", replay_path);

	// v0.24 bot-loader probe: downloadable bot .so files would live in app
	// data, which W^X rules (API 29+) may forbid loading. If a probe library
	// is present, try dlopen and log the verdict so the feasibility can be
	// checked on any device without special builds.
	{
		auto try_probe = [&](const a_string& probe) {
			if (!file_exists(probe)) return;
			void* h = dlopen(probe.c_str(), RTLD_NOW);
			if (h) {
				void* sym = dlsym(h, "openbw_bot_probe");
				log("bot-probe: dlopen OK: %s (symbol %s)\n", probe.c_str(), sym ? "found" : "missing");
				dlclose(h);
			} else {
				log("bot-probe: dlopen FAILED: %s: %s\n", probe.c_str(), dlerror());
			}
		};
		try_probe(data_dir + "/bot-probe.so");
		// External storage is noexec; internal app storage is the real
		// candidate for downloadable bot libraries.
		const char* internal = SDL_AndroidGetInternalStoragePath();
		if (internal) try_probe(a_string(internal) + "/bot-probe.so");
	}

	// The engine reports unsupported features (computer AI, UMS triggers,
	// unimplemented orders) via bwgame::error() which throws. Catch instead
	// of letting SIGABRT kill the process so the user gets a readable
	// message and lands back on the setup screen.
	try {
		std::chrono::high_resolution_clock clock;
		auto start = clock.now();

		auto load_data_file = data_loading::data_files_directory(data_dir + "/");

		// --- MOD layer (v0.19) -------------------------------------------
		// Active mods live under files/mods/<id>/ and override game data two
		// ways, both ahead of the base MPQ chain:
		//   - <id>/data.mpq   : classic MPQ, injected at the FRONT of the chain
		//   - <id>/data/<path>: loose files (path = engine request, '\'->'/')
		// Later ids in --mods end up with higher priority.
		struct mod_data_loader {
			decltype(load_data_file)* base;
			a_vector<a_string> loose_dirs; // highest priority first
			bwgame::casc::storage* casc = nullptr; // Faze 2: read-only SC:R CASC, between mod overlay and MPQ (nullptr = disabled)
			static a_string normalize(a_string fn) {
				for (auto& c : fn) if (c == '\\') c = '/';
				return fn;
			}
			bool loose_find(const a_string& filename, a_string& out) {
				a_string fn = normalize(filename);
				a_string lower = fn;
				for (auto& c : lower) c = (char)tolower((unsigned char)c);
				for (auto& d : loose_dirs) {
					for (const a_string* v : {&fn, &lower}) {
						a_string p = d + "/" + *v;
						FILE* f = fopen(p.c_str(), "rb");
						if (f) { fclose(f); out = p; return true; }
						if (v == &lower || fn == lower) break;
					}
				}
				return false;
			}
			void operator()(a_vector<uint8_t>& dst, a_string filename) {
				a_string p;
				if (loose_find(filename, p)) {
					FILE* f = fopen(p.c_str(), "rb");
					if (f) {
						fseek(f, 0, SEEK_END);
						long sz = ftell(f);
						fseek(f, 0, SEEK_SET);
						dst.resize((size_t)sz);
						size_t read = fread(dst.data(), 1, (size_t)sz, f);
						fclose(f);
						if (read == (size_t)sz) return;
					}
				}
				// CASC (SC:R native) before the base MPQ chain. SD assets are
				// byte-identical MPQ<->CASC (ASSET-GUIDE 1.1), so this cannot
				// change sim behaviour; names absent from CASC fall through.
				if (casc && casc->has_name(filename.c_str())) {
					std::vector<uint8_t> tmp;
					if (casc->read_name(filename.c_str(), tmp)) {
						dst.resize(tmp.size());
						if (!tmp.empty()) memcpy(dst.data(), tmp.data(), tmp.size());
						return;
					}
				}
				(*base)(dst, std::move(filename));
			}
			bool file_exists(const a_string& filename) {
				a_string p;
				if (loose_find(filename, p)) return true;
				if (casc && casc->has_name(filename.c_str())) return true;
				return base->file_exists(filename);
			}
		};
		// open1161HD Faze 2: optional read-only SC:R CASC source (--casc <install
		// root>). OFF unless requested; MPQ chain stays default. Inflate is the
		// zero-dependency stb_image zlib decoder (host-proven byte-identical).
		bwgame::casc::storage casc_storage;
		bool casc_ok = false;
		if (!casc_dir.empty()) {
			casc_ok = casc_storage.open((casc_dir + "/data/data").c_str(), casc_dir.c_str(), bwgame::casc::stb_inflate());
			log("casc: %s at '%s' (%zu names)\n", casc_ok ? "enabled" : "FAILED", casc_dir.c_str(), casc_ok ? casc_storage.name_count() : (size_t)0);
		}
		mod_data_loader mod_loader{&load_data_file, {}};
		if (casc_ok) mod_loader.casc = &casc_storage;
		a_string hd_anim_dir; // v0.23: set when an active mod ships anim/
		a_string hd_ui_dir;   // v0.23: mod ui/ dir (HD cmdicons.dds.grp)
		a_string icons_png_dir; // v0.23: mod icons/ dir (NNN.png overrides)
		a_string hd_tileset_dir; // v0.23: mod tileset/ dir (<name>.dds.vr4)
		if (!mods_csv.empty()) {
			log("mods: '%s'\n", mods_csv.c_str());
			size_t pos = 0;
			while (pos <= mods_csv.size()) {
				size_t comma = mods_csv.find(',', pos);
				a_string id = mods_csv.substr(pos, comma == a_string::npos ? a_string::npos : comma - pos);
				pos = comma == a_string::npos ? mods_csv.size() + 1 : comma + 1;
				if (id.empty()) continue;
				a_string dir = data_dir + "/mods/" + id;
				a_string mpq_path = dir + "/data.mpq";
				if (file_exists(mpq_path)) {
					try {
						load_data_file.mpqs.emplace_front(mpq_path);
						log("mod '%s': data.mpq attached\n", id.c_str());
					} catch (...) {
						log("mod '%s': data.mpq unreadable, skipped\n", id.c_str());
					}
				}
				a_string loose = dir + "/data";
				if (DIR* d = opendir(loose.c_str())) {
					closedir(d);
					mod_loader.loose_dirs.insert(mod_loader.loose_dirs.begin(), loose);
					log("mod '%s': loose data dir attached\n", id.c_str());
				}
				// v0.23: HD sprite mod - anim/main_NNN.anim (SC:R extract)
				a_string animdir = dir + "/anim";
				if (DIR* d = opendir(animdir.c_str())) {
					closedir(d);
					hd_anim_dir = animdir;
					log("mod '%s': HD anim dir attached\n", id.c_str());
				}
				// v0.23: HD UI (ui/cmdicons.dds.grp) and user icon packs (icons/NNN.png)
				a_string uidir = dir + "/ui";
				if (DIR* d = opendir(uidir.c_str())) {
					closedir(d);
					hd_ui_dir = uidir;
					log("mod '%s': HD ui dir attached\n", id.c_str());
				}
				a_string icondir = dir + "/icons";
				if (DIR* d = opendir(icondir.c_str())) {
					closedir(d);
					icons_png_dir = icondir;
					log("mod '%s': icon pack attached\n", id.c_str());
				}
				a_string tiledir = dir + "/tileset";
				if (DIR* d = opendir(tiledir.c_str())) {
					closedir(d);
					hd_tileset_dir = tiledir;
					log("mod '%s': HD tileset dir attached\n", id.c_str());
				}
			}
		}

		game_player player(mod_loader);

		main_t m(std::move(player));
		auto& ui = m.ui;

		m.ui.load_all_image_data(mod_loader);

		// One lock for ALL mod_loader (MPQ/CASC/loose) reads. mod_loader is not
		// reentrant, and reads now happen off the main thread too -- the HD-anim
		// prefetch worker (load_hd_bytes) and the music preloader (load_data_file).
		// Both lambdas below, and the workers through them, serialise on this. It is
		// a function-static so the lambdas (stored in ui.*) reference it without
		// capture and it outlives them. The lock covers only the read; decode/decompress
		// on the worker runs afterward, off-thread.
		static std::mutex mpq_io_mtx;
		ui.load_data_file = [&](a_vector<uint8_t>& data, a_string filename) {
			std::lock_guard<std::mutex> lk(mpq_io_mtx);
			mod_loader(data, std::move(filename));
		};
		ui.data_file_exists = [&](a_string filename) {
			return mod_loader.file_exists(filename);
		};

		// Optional CD archives (StarCraft.mpq / BroodWar.mpq): the in-game
		// music lives there, not in the installed StarDat/BrooDat (1.15.1+
		// noCD layout). Appended LAST so they can never shadow game data —
		// they only serve files the installed MPQs don't have (music/*.wav).
		for (const char* fn : {"StarCraft.mpq", "BroodWar.mpq"}) {
			a_string path = data_dir + "/" + fn;
			if (!file_exists(path)) continue;
			try {
				load_data_file.add_mpq_file(path);
				log("optional mpq attached: %s\n", fn);
			} catch (...) {
				log("optional mpq unreadable, skipped: %s\n", fn);
			}
		}

		// v0.35 map browser: a bundled map chosen from the MPQ chain. The
		// .scm/.scx is itself an MPQ stored inside the CD archive, so the normal
		// melee loader (which opens a map file on disk) can't read it directly.
		// Extract its bytes from the chain to a temp file and load that.
		if (map_path.empty() && !map_mpq.empty()) {
			try {
				a_vector<uint8_t> mapdata;
				mod_loader(mapdata, map_mpq);
				a_string ext = ".scx";
				size_t dot = map_mpq.find_last_of('.');
				if (dot != a_string::npos) ext = map_mpq.substr(dot);
				a_string tmp = data_dir + "/_bundled_map" + ext;
				FILE* f = fopen(tmp.c_str(), "wb");
				if (f) {
					if (!mapdata.empty()) fwrite(mapdata.data(), 1, mapdata.size(), f);
					fclose(f);
					map_path = tmp;
					log("bundled map: %s (%d bytes) -> %s\n", map_mpq.c_str(), (int)mapdata.size(), tmp.c_str());
				}
			} catch (...) {
				log("bundled map extract failed: %s\n", map_mpq.c_str());
			}
			if (map_path.empty()) {
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW",
					"Nepavyko iskelti zemelapio is MPQ. Ar importuoti StarCraft.mpq / BroodWar.mpq?", nullptr);
				return 1;
			}
		}

		ui.init();
		ui.leave_through_host = true;   // v1.0.3: "leave game" returns through the main loop, which saves the replay
		ui.set_volume(volume);
		ui.set_music_volume(music_volume);
		ui.sfx_volume = sfx_volume < 0 ? 0 : (sfx_volume > 100 ? 100 : sfx_volume);
		ui.voice_volume = voice_volume < 0 ? 0 : (voice_volume > 100 ? 100 : voice_volume);
		ui.ui_volume = ui_volume < 0 ? 0 : (ui_volume > 100 ? 100 : ui_volume);
		ui.voice_freq = voice_freq < 0 ? 0 : (voice_freq > 100 ? 100 : voice_freq);
		ui.queue_button_visible = show_queue != 0;
		ui.show_zoom_buttons = zoom_buttons != 0;
		ui.no_fog = no_fog != 0;
		ui.resource_overlay = resource_counts != 0;
		ui.equip_grey = equip_grey != 0;
		ui.saturation_overlay = saturation != 0;
		ui.lang_lt = lang == "lt";
		// v0.45: default game speed from Settings (percent; 100 = normal). The
		// in-game +/- keys still override it live.
		if (game_speed_pct >= 25 && game_speed_pct <= 800)
			ui.game_speed = fp8::integer(game_speed_pct) / 100;
		ui.ui_legacy = ui_legacy != 0;
		ui.rgba_sprites = rgba_sprites != 0;
		if (!hd_anim_dir.empty()) {
			// HD sprites need the true-color pass; the RGBA toggle is implied.
			ui.hd_anim_dir = hd_anim_dir;
			ui.rgba_sprites = true;
			log("HD sprites enabled from %s\n", hd_anim_dir.c_str());
		}
		if (!hd_ui_dir.empty()) ui.hd_ui_dir = hd_ui_dir;
		if (!icons_png_dir.empty()) ui.icons_png_dir = icons_png_dir;
		if (!hd_tileset_dir.empty()) {
			ui.hd_tileset_dir = hd_tileset_dir;
			ui.rgba_sprites = true;
		}
		// open1161HD Faze 4.2/4.3: when the SC:R CASC is attached, pull HD sprites+
		// terrain+UI+portraits straight from CASC at the chosen graphics tier - no
		// disk mod needed. Disk-mod HD dirs (above) still win if set. The tier maps
		// to the CASC asset prefix: "hd2" -> "HD2/" (scale 2), "full" -> "" (scale
		// 4). "sd" leaves HD off (classic 8-bit world) - the safe fallback if a
		// device can't fit the HD tiers. Probe the tier's own anim so we degrade
		// gracefully when a tier's assets are missing.
		if (casc_ok && gfx_tier != "sd" && hd_anim_dir.empty() && hd_tileset_dir.empty()) {
			bwgame::a_string prefix = (gfx_tier == "full") ? bwgame::a_string("") : bwgame::a_string("HD2/");
			if (casc_storage.has_name((prefix + "anim/main_000.anim").c_str())) {
				ui.hd_casc = true;
				ui.hd_casc_prefix = prefix;
				ui.rgba_sprites = true;
				// Faze 4.3: FULL HD supersamples the world 2x (physical buffer).
				ui.world_scale = (gfx_tier == "full") ? 2 : 1;
				ui.load_hd_bytes = [&mod_loader](const bwgame::a_string& name,
				                                 std::vector<uint8_t>& out) -> bool {
					// perf #2: the HD-anim prefetch worker reaches CASC through this
					// callback. mod_loader is not reentrant, so serialise every byte
					// read on the SAME lock load_data_file uses -- the render thread's
					// tile/portrait/cinematic reads, the main thread's sound/music
					// reads, and both background workers never overlap. The lock covers
					// only the read; the slow anim::parse runs afterward, off-thread.
					std::lock_guard<std::mutex> lk(mpq_io_mtx);
					if (!mod_loader.file_exists(name)) return false;
					bwgame::a_vector<uint8_t> tmp;
					mod_loader(tmp, name);
					if (tmp.empty()) return false;
					out.assign(tmp.begin(), tmp.end());
					return true;
				};
				// Faze 4.6: cheap name existence check (no byte read) for the HD
				// portrait coverage audit + webm variant resolution.
				ui.hd_file_exists = [&mod_loader](const bwgame::a_string& name) -> bool {
					return mod_loader.file_exists(name);
				};
				// Faze 4.7 Variant B: HW WebM cinematic decode callbacks. The temp
				// dir stages the CASC bytes for AMediaExtractor.
				{
					const char* intdir = SDL_AndroidGetInternalStoragePath();
					std::string tmp = intdir ? intdir : data_dir.c_str();
					ui.hd_cine_open = [tmp](const std::vector<uint8_t>& b, int& w, int& h, double& usf) -> void* {
						return (void*)cine_hw::open(b.data(), b.size(), tmp.c_str(), w, h, usf);
					};
					ui.hd_cine_next = [](void* d, std::vector<uint8_t>& rgb) -> bool {
						return cine_hw::next_video((cine_hw::decoder*)d, rgb);
					};
					ui.hd_cine_close = [](void* d) { cine_hw::close((cine_hw::decoder*)d); };
					// Faze 4.7 Variant A: OS video-player path (VideoView). Preferred
					// over the native HW path -- robust on every SoC (handles UBWC/
					// tiled output the CPU-readback path cannot) and gives audio for
					// free. play_hd_cinematic tries this first and only falls back to
					// the hd_cine_* HW/SMK chain if it returns false.
					ui.hd_cine_play_java = [tmp](const std::vector<uint8_t>& b) -> bool {
						return cine_java::play(b.data(), b.size(), tmp.c_str());
					};
				}
				log("HD tier '%s' (prefix '%s') tiles+sprites+UI+portraits enabled from CASC\n",
					gfx_tier.c_str(), prefix.empty() ? "(full)" : prefix.c_str());
				// Faze 4.5: FULL HD auto-fallback for devices that can't sustain it
				// (e.g. an S7+). If FULL HD stays below the threshold for a sustained
				// window of gameplay, drop a marker the launcher reads on the NEXT
				// launch to force HD2 -- no risky mid-game buffer surgery.
				if (gfx_tier == "full") {
					const char* intdir = SDL_AndroidGetInternalStoragePath();
					bwgame::a_string marker = (intdir ? bwgame::a_string(intdir) : data_dir) + "/gfx_auto_hd2";
					ui.perf_fps_threshold = 22;
					ui.perf_downgrade_cb = [marker]() {
						FILE* f = fopen(marker.c_str(), "wb");
						if (f) { fputs("hd2", f); fclose(f); }
						ui::log("perf: FULL HD sustained low FPS -> HD2 auto-downgrade marked for next launch\n");
					};
				}
			} else {
				log("HD tier '%s': CASC lacks %sanim - staying SD\n", gfx_tier.c_str(),
					prefix.empty() ? "(full)" : prefix.c_str());
			}
		}
		if (!app_version.empty()) ui.hud_version = "v" + app_version;
		// HUD v3 (the touch HUD). Pure presentation + input; the sim never sees it.
		ui.hud_v3 = hud_v3;
		if (hud_v3) {
			ui.hud3_template = hud3_template < 0 || hud3_template > 1 ? 0 : hud3_template;
			ui.hud3_dock_mode = hud3_dock < 0 || hud3_dock > 2 ? 0 : hud3_dock;
			ui.hud3_left = hud3_left;
			ui.hud3_scale_pct = hud3_scale < 80 ? 80 : hud3_scale > 140 ? 140 : hud3_scale;
			ui.hud3_wheel_ms = hud3_wheel_ms < 200 ? 200 : hud3_wheel_ms > 600 ? 600 : hud3_wheel_ms;
			ui.hud3_pen_wheel = hud3_pen_wheel;
			ui.hud3_vibrate = hud3_vibrate;
			ui.hud3_labels_everywhere = hud3_labels;
			ui.hud3_all_groups = hud3_allgroups;
			ui.hud3_latch_everywhere = hud3_latch;
			native_window::set_long_press_ms(ui.hud3_wheel_ms);
			// the first v3 game shows a one-screen hint; dismissing it leaves a mark
			ui.hud3_hint_flag_path = data_dir + "/hud3-hint-seen";
			if (FILE* hf = fopen(ui.hud3_hint_flag_path.c_str(), "rb")) fclose(hf);
			else ui.hud3_hint_pending = true;
			ui.hud3_vibrate_fn = [](int kind) {
				// OpenBWActivity.vibrate(int): a predefined click / heavy click / double click
				JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
				jobject act = (jobject)SDL_AndroidGetActivity();
				if (!env || !act) return;
				jclass cls = env->GetObjectClass(act);
				jmethodID mid = cls ? env->GetMethodID(cls, "vibrate", "(I)V") : nullptr;
				if (mid) env->CallVoidMethod(act, mid, (jint)kind);
				if (env->ExceptionCheck()) env->ExceptionClear();
				if (cls) env->DeleteLocalRef(cls);
				env->DeleteLocalRef(act);
			};
		}
		if (!version_label_path.empty()) {
			// "VLBL" w h, then w*h ARGB (little endian), written by OpenBWActivity
			if (FILE* f = fopen(version_label_path.c_str(), "rb")) {
				uint32_t hdr[3] = {};
				if (fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x4c424c56u && hdr[1] > 0 && hdr[1] <= 1024 && hdr[2] > 0 && hdr[2] <= 256) {
					ui.hud_version_img.resize((size_t)hdr[1] * hdr[2]);
					if (fread(ui.hud_version_img.data(), 4, ui.hud_version_img.size(), f) == ui.hud_version_img.size()) {
						ui.hud_version_img_w = (int)hdr[1]; ui.hud_version_img_h = (int)hdr[2];
						ui.hud_version_img_screen_h = version_label_screen_h;
					} else ui.hud_version_img.clear();
				}
				fclose(f);
			}
		}
		ui.hd_bloom = bloom_strength; // Faze 7: GPU bloom strength from --bloom
		ui.hd_post_gl = post_gl_on;   // SP: GLES2 present stage from --post-gl
		ui.hd_post_effect = post_effect;     // SP: effect select (-1 AUTO / 0 / 1)
		ui.hd_post_strength = post_strength; // SP: effect strength 0..100
		ui.hd_gl_bloom = gl_bloom;           // SP Phase 3: GPU bloom 0..100
		ui.hd_grade = gl_grade;              // SE: color-grade 0..100
		ui.hd_emissive = emissive_on != 0;
		ui.hd_shadow_soft = soft_shadows; // 0..100 softness (0 = classic hard)
		ui.hd_shadow_dir = sun_shadow;        // SHADOWS: directional sun shadow (0/1)
		ui.hd_shadow_len = sun_shadow_len;    // sun-shadow lean length 0..100
		ui.hd_shadow_sign = sun_shadow_sign;  // sun azimuth sign +1/-1
		ui.hd_point_lights = point_lights;    // SHADOWS Phase 2: dynamic point lights
		ui.hd_light_strength = light_strength; // point-light strength 0..200
		ui.hd_anim_diag = hd_anim_diag;       // DIAG: HD-anim load logging
		ui.hud_show_fps = show_fps != 0;
		ui.hud_show_apm = show_apm != 0;
		ui.hud_health_always = health_always != 0;
		ui.camera_speed = camera_speed < 25 ? 25 : (camera_speed > 250 ? 250 : camera_speed);
		ui.tap_command = tap_command != 0;
	ui.render_interp = render_interp != 0;

		// Networking objects live for the whole game (referenced by the
		// input-submit and next-frame hooks).
		const bool net_mode = melee && (net_host || !net_join.empty());
		// v0.81.004: peer-loss is decided once. Without this the check would
		// re-fire every frame after the opponent is gone.
		bool peer_lost_handled = false;
		ui.is_net_game = net_mode; // pause menu: LAN doesn't pause, shows "Leave game"
		ui.local_player_name = player_name; // v0.30: chat line prefix
		// Identity portraits (loading cards, score file). Pure presentation: ids only
		// pick which of the user's own portrait clips is drawn.
		ui.local_portrait = player_portrait;
		ui.ai_portrait_level = (bot_path.find("unifiedai") != a_string::npos) ? bot_level : -1;
		{
			size_t sp = 0;
			while (sp < name_portraits_arg.size()) {
				size_t e = name_portraits_arg.find('|', sp);
				if (e == a_string::npos) e = name_portraits_arg.size();
				a_string item = name_portraits_arg.substr(sp, e - sp);
				size_t eq = item.find('=');
				if (eq != a_string::npos && eq > 0)
					ui.name_portraits.emplace_back(item.substr(eq + 1), atoi(item.substr(0, eq).c_str()));
				sp = e + 1;
			}
		}
		game_load_functions load_funcs(ui.st);
		sync_state sync_st;
		sync_functions net(ui.st, ui.action_st, sync_st);
		sync_server_asio_tcp server;

		// Live replay recording: the seed + final player setup are captured at
		// load time (replay_rs.random_seed is set inside the load callbacks),
		// actions while playing (locally via ui.game_action_record, over LAN
		// via sync_st.save_replay which taps every peer), and a BW-compatible
		// .rep is written on exit / game over.
		replay_saver_state replay_rs;
		replay_saver_functions replay_rsf(replay_rs);
		a_vector<uint8_t> replay_map_data;
		const bool record_replay = melee && !campaign;
		// Fills the header + embeds the scenario; returns false (and leaves
		// recording off) if the map file can't be unwrapped.
		auto init_replay_recording = [&](const game_load_functions::setup_info_t& si, int slots) {
			// Replays embed the raw CHK scenario data (not the .scm MPQ container).
			a_vector<uint8_t> scm_data;
			FILE* f = fopen(map_path.c_str(), "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				long sz = ftell(f);
				fseek(f, 0, SEEK_SET);
				scm_data.resize((size_t)sz);
				if (fread(scm_data.data(), 1, (size_t)sz, f) != (size_t)sz) scm_data.clear();
				fclose(f);
			}
			if (scm_data.empty()) return false;
			try {
				data_loading::mpq_data mpq(scm_data.data(), scm_data.size());
				mpq(replay_map_data, "staredit\\scenario.chk");
			} catch (...) {
				replay_map_data = scm_data; // map was already a raw CHK
			}
			replay_rs.map_data = replay_map_data.data();
			replay_rs.map_data_size = replay_map_data.size();
			replay_rs.player_name = player_name.empty() ? "Player" : player_name;
			replay_rs.map_tile_width = ui.game_st.map_tile_width;
			replay_rs.map_tile_height = ui.game_st.map_tile_height;
			replay_rs.active_player_count = slots;
			replay_rs.slot_count = slots;
			replay_rs.game_type = 2; // melee
			replay_rs.tileset = (int)ui.game_st.tileset_index;
			replay_rs.game_name = "OpenBW Android";
			size_t base = map_path.find_last_of("/\\");
			replay_rs.map_name = base == a_string::npos ? map_path : map_path.substr(base + 1);
			replay_rs.setup_info = si;
			replay_rs.players = ui.st.players;
			// v1.0.2 (trailer version 2): the set-up the header cannot carry -- teams
			// as they stand now, before the first frame. Computer slots add their
			// aiscript id where they are started (replay_rs.ai_script).
			replay_rs.has_setup = true;
			for (size_t i = 0; i != 12; ++i) {
				for (size_t j = 0; j != 12; ++j) replay_rs.alliances[i][j] = (uint8_t)ui.st.alliances[i][j];
				replay_rs.shared_vision[i] = (uint32_t)ui.st.shared_vision[i];
			}
			// a bot's commands go into the recording as its slot's actions
			if (m.bot_instance && m.bot_player >= 0) {
				m.bot_action_record = [&](const uint8_t* d, size_t n) {
					replay_rsf.add_action((int)ui.st.current_frame, m.bot_player, d, n);
				};
			}
			// The header stores a color INDEX (0-7) per slot and the loader
			// does source_colors.at(index); closed slots can carry junk color
			// values, which would throw on playback.
			for (size_t k = 0; k != 8; ++k) {
				if ((size_t)replay_rs.players[k].color >= 8) replay_rs.players[k].color = (int)k;
			}
			log("replay recording enabled (seed %u)\n", replay_rs.random_seed);
			return true;
		};

		if (melee && net_mode) {
			// Network melee: load the map with its slots left OPEN - the
			// lobby (sync layer) occupies them. No starting units are
			// created on this load; the map is reloaded when the game
			// starts, with the negotiated slots/races and the shared seed.
			load_funcs.load_map_file(map_path, [&]() {
				load_funcs.setup_info.victory_condition = 1;
				load_funcs.setup_info.starting_units = 1;
				load_funcs.setup_info.resource_type = 1;
				load_funcs.setup_info.starting_minerals = 50;
				// LAN race negotiation: the sync layer only honours a client's
				// picked race when the slot is "user selectable" (race 5). Some
				// melee CHKs ship playable slots as Random (6) or a fixed race, so
				// every device would fall back to start_game's random assignment
				// (the "host reset to random" bug). Force all playable slots to
				// user-selectable so each device's chosen race always sticks; a
				// player who actually wants Random just never sends set_race and
				// the slot stays 5 -> randomized at start, exactly as intended.
				for (size_t i = 0; i != 8; ++i) {
					auto& v = ui.st.players[i];
					if (v.controller == player_t::controller_open ||
					    v.controller == player_t::controller_computer)
						v.race = (bwgame::race_t)5;
				}
			});
			log("net melee: map loaded, %s port %d\n", net_host ? "hosting on" : "joining", net_port);
		} else if (melee) {
			// Game modes (v0.20): a non-melee .trg (ctf/greed/slaughter/sudden
			// death) replaces the Melee.trg victory triggers. The engine reads
			// them from global_st.melee_trg, so swap its contents before the
			// map load. global_state is const from the state's point of view;
			// this glue owns the game and mutates it before the sim starts.
			const bool ums = game_mode == "ums";
			if (!ums && game_mode != "melee") {
				a_string trg = a_string("triggers/") + game_mode + ".trg";
				if (mod_loader.file_exists(trg)) {
					mod_loader(const_cast<global_state&>(*ui.st.global).melee_trg, trg);
					log("game mode: %s\n", game_mode.c_str());
				} else {
					log("game mode '%s': %s not found, falling back to melee\n", game_mode.c_str(), trg.c_str());
				}
			}
			// Melee setup mirrors mini-openbwapi's load_map: pick two of the
			// map's open/computer slots, mark them occupied, apply races, and
			// enable melee victory (starting units + Melee.trg triggers).
			// Both players are "occupied" (human-style) so the engine never
			// hits the unimplemented computer AI path.
			int chosen_local = -1;
			int chosen_enemy = -1;
			bwgame::static_vector<size_t, 12> active_slots;
			std::array<int, 12> teams = parse_teams(teams_csv);
			if (ums) {
				// Use Map Settings (v0.20): the map's own OWNR/units/triggers
				// run as-is. Occupy the first open human slot for the local
				// player; computer slots stay computer (the engine flips them
				// to computer_game at load) so their triggers execute.
				// Campaign missions (v0.20) load their CHK straight from the
				// game MPQ chain (campaign/<race>/<mission>/staredit/scenario.chk).
				a_vector<uint8_t> campaign_chk;
				if (campaign) {
					a_string chk_path = campaign_dir + "/staredit/scenario.chk";
					if (!mod_loader.file_exists(chk_path))
						error("kampanijos misija '%s' nerasta. Kampaniju zemelapiai yra CD "
						      "archyvuose - importuokite StarCraft.mpq (originalios) ir "
						      "BrooDat/BroodWar.mpq (Brood War) NUSTATYMU ekrane; ivestu "
						      "StarDat.mpq + BrooDat.mpq kampaniju NETURI.", campaign_dir.c_str());
					mod_loader(campaign_chk, chk_path);
					log("campaign: %s (%d bytes)\n", campaign_dir.c_str(), (int)campaign_chk.size());
				}
				auto ums_setup = [&]() {
					{
						a_string ownr = "ums: OWNR controllers:";
						for (size_t i = 0; i != 12; ++i) {
							char b[8];
							snprintf(b, sizeof(b), " %d", (int)ui.st.players[i].controller);
							ownr += b;
						}
						log("%s\n", ownr.c_str());
					}
					// Some maps pre-set the human slot as "occupied" instead of
					// "open"; accept either, preferring an already-occupied one.
					for (auto& v : ui.st.players) {
						size_t index = (size_t)(&v - ui.st.players.data());
						if (index >= 8) continue;
						if (v.controller == player_t::controller_occupied && chosen_local < 0) {
							chosen_local = (int)index;
							active_slots.push_back(index);
						}
					}
					for (auto& v : ui.st.players) {
						size_t index = (size_t)(&v - ui.st.players.data());
						if (index >= 8) continue;
						if (v.controller == player_t::controller_open) {
							if (chosen_local < 0) {
								chosen_local = (int)index;
								v.controller = player_t::controller_occupied;
								active_slots.push_back(index);
							} else {
								v.controller = player_t::controller_closed;
							}
						} else if (v.controller == player_t::controller_computer) {
							// Providing setup_f skips the engine's own flip to
							// computer_game, so do it here — triggers and the
							// aiscript VM only run for computer_game players.
							v.controller = player_t::controller_computer_game;
							if (chosen_enemy < 0) chosen_enemy = (int)index;
							active_slots.push_back(index);
						}
					}
					if (chosen_local < 0) error("UMS: map has no open human slot");
					if (chosen_enemy < 0) chosen_enemy = chosen_local;
					ui.st.lcg_rand_state = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
					replay_rs.random_seed = ui.st.lcg_rand_state;
					// setup_info stays at defaults -> use_map_settings = true.
				};
				if (campaign) {
					m.campaign_dir = campaign_dir;
					m.campaign_data_dir = data_dir;
					ui.loading_campaign_dir = campaign_dir; // loading screen: show the mission's real title
					load_funcs.load_map_data(campaign_chk.data(), campaign_chk.size(), ums_setup);
					ui.map_data_prefix = campaign_dir + "/"; // trigger WAVs: staredit/wav/... under the mission dir
					// v0.26: mission briefing from the CHK MBRF chunk. Same
					// 2400-byte trigger records as TRIG; actions play as a
					// sequential slideshow before frame 0.
					{
						auto chk_string = [&](uint32_t index) -> a_string {
							if (!index) return "";
							size_t pos = 0;
							while (pos + 8 <= campaign_chk.size()) {
								uint32_t tag;
								int32_t len;
								memcpy(&tag, campaign_chk.data() + pos, 4);
								memcpy(&len, campaign_chk.data() + pos + 4, 4);
								pos += 8;
								if (memcmp(&tag, "STR ", 4) == 0 && len > 2) {
									const uint8_t* s = campaign_chk.data() + pos;
									uint16_t count;
									memcpy(&count, s, 2);
									if (index > count) return "";
									uint16_t off;
									memcpy(&off, s + index * 2, 2);
									if (off >= (uint32_t)len) return "";
									a_string r;
									for (size_t i = off; i < (size_t)len && s[i]; ++i) r += (char)s[i];
									return r;
								}
								pos += len;
							}
							return "";
						};
						int local_force = chosen_local < 12 ? ui.st.players[chosen_local].force : 0;
						size_t pos = 0;
						while (pos + 8 <= campaign_chk.size()) {
							uint32_t tag;
							int32_t len;
							memcpy(&tag, campaign_chk.data() + pos, 4);
							memcpy(&len, campaign_chk.data() + pos + 4, 4);
							pos += 8;
							if (memcmp(&tag, "MBRF", 4) == 0) {
								int nt = len / 2400;
								for (int ti = 0; ti != nt; ++ti) {
									const uint8_t* t = campaign_chk.data() + pos + (size_t)ti * 2400;
									const uint8_t* en = t + 320 + 64 * 32 + 4;
									if (!en[chosen_local] && !en[17] && !en[18 + local_force]) continue;
									for (int ai = 0; ai != 64; ++ai) {
										const uint8_t* a = t + 320 + ai * 32;
										uint8_t type = a[26];
										if (!type) break;
										uint32_t str, wav, time_ms, slot;
										uint16_t unit;
										memcpy(&str, a + 4, 4);
										memcpy(&wav, a + 8, 4);
										memcpy(&time_ms, a + 12, 4);
										memcpy(&slot, a + 16, 4); // player/group field = portrait WINDOW slot (0-3)
										memcpy(&unit, a + 24, 2);
										bwgame::ui_functions::briefing_action_t ba;
										ba.type = type;
										ba.time_ms = (int)time_ms;
										ba.unit = unit;
										ba.slot = (int)slot;
										if (str) ba.text = chk_string(str);
										if (wav) ba.wav = chk_string(wav);
										ui.briefing_actions.push_back(std::move(ba));
									}
								}
							}
							pos += len;
						}
						{
							// Which action types this map's briefing actually
							// uses. Recorded because a whole class of them
							// (type 8, Transmission) was silently unhandled and
							// nothing surfaced that -- the briefing just had no
							// dialogue.
							int counts[16] = {};
							for (auto& a2 : ui.briefing_actions)
								if (a2.type >= 0 && a2.type < 16) ++counts[a2.type];
							a_string s;
							for (int k = 0; k != 16; ++k)
								if (counts[k]) s += format("t%d=%d ", k, counts[k]);
							log("briefing action types: %s\n", s);
						}
						if (!ui.briefing_actions.empty()) {
							ui.briefing_active = true;
							ui.briefing_reset_cells(); // Faze 4.7: fresh speaker-cell assignment
							// Faze 6: hold the briefing until the initial loading/prewarm
							// completes, so it plays start-to-finish AFTER the loading
							// screen instead of streaming (and fast-forwarding through its
							// wall-clock waits) underneath it.
							ui.briefing_hold = true;
							log("briefing: %d actions\n", (int)ui.briefing_actions.size());
						}
					}
					// v0.26: campaign intro cinematic before the first mission's
					// briefing (vanilla campaigns: smk/<race>1.smk).
					{
						size_t p1 = campaign_dir.find('/');
						size_t p2 = campaign_dir.rfind('/');
						if (p1 != a_string::npos && p2 != a_string::npos && p2 > p1) {
							a_string race = campaign_dir.substr(p1 + 1, p2 - p1 - 1);
							a_string mission = campaign_dir.substr(p2 + 1);
							ui.briefing_is_tutorial = (mission == "tutorial"); // Faze 4.7: Boot Camp skip button
							bool first = mission == "tutorial"
							    || (mission.size() >= 2 && mission.substr(mission.size() - 2) == "01");
							// <race>1.smk is NOT the intro -- it is a mid-campaign story
							// cinematic (terran1 = "Wasteland Patrol", after Backwater
							// Station), now emitted by the post-mission beat table. The
							// real intro is starintr (Brood War: starxintr), only at the
							// very start of a game's Terran campaign. Zerg/Protoss episodes
							// have no separate intro movie. Deferred: surfaces don't exist
							// yet here; the main loop plays it before frame 0.
							if (first) {
								if (race == "terran") m.intro_cinematic = "smk/starintr.smk";
								else if (race == "expterran") m.intro_cinematic = "smk/starxintr.smk";
							}
						}
					}
				} else {
					load_funcs.load_map_file(map_path, ums_setup);
				}
				m.live_mode = true;
				m.local_player = chosen_local;
				m.enemy_player = chosen_enemy;
				ui.game_input = true;
				ui.game_local_player = chosen_local;
				// (Player colours for SP/UMS come from --colors, applied per-slot in the
				// melee setup below; the net path uses --player-color for the local slot.)
				// v0.83.002: UMS and campaign missions were the ONE path that never
				// turned smart rally on -- melee (below) and LAN (net) both did --
				// so rallying a Command Center onto minerals or a refinery did
				// nothing in any campaign mission. Single-player, so no determinism
				// concern; the LAN path stays gated through net.smart_rally.
				ui.smart_rally = true;
				ui.st.game->qol.smart_rally = true;   // v1.0.2: recorded (qol bit7), so playback runs it too
				if (!campaign) ui.map_mpq_path = map_path;
				if (!campaign) ui.loading_map_label = !map_mpq.empty() ? map_mpq : map_path; // loading screen map name
				for (size_t i = 0; i != 12; ++i) ui.action_st.player_id[i] = (int)i;
				m.ums_mode = true;
				log("ums: local player %d (%d active slots)\n", chosen_local, (int)active_slots.size());
				for (int p = 0; p != 12; ++p) {
					int n = 0;
					for (auto i = ui.st.player_units[p].begin(); i != ui.st.player_units[p].end(); ++i) ++n;
					if (n) log("ums: player %d has %d units (controller %d)\n", p, n, (int)ui.st.players[p].controller);
				}
				if (record_replay && init_replay_recording(load_funcs.setup_info, (int)active_slots.size())) {
					ui.game_action_record = [&](const uint8_t* d, size_t n) {
						replay_rsf.add_action((int)ui.st.current_frame, ui.game_local_player, d, n);
					};
				}
			} else {
			// Ask the bot which race it plays before the map (and therefore its
			// starting units) is built; see query_bot_race.
			const int bot_race = (!bot_path.empty() && !net_host && net_join.empty())
					? query_bot_race(bot_path) : -1;
			load_funcs.load_map_file(map_path, [&]() {
				bwgame::static_vector<size_t, 12> available;
				for (auto& v : ui.st.players) {
					size_t index = (size_t)(&v - ui.st.players.data());
					if ((v.controller == player_t::controller_open || v.controller == player_t::controller_computer) && index < 8) {
						available.push_back(index);
					}
					if (v.controller == player_t::controller_open || v.controller == player_t::controller_computer) {
						v.controller = player_t::controller_closed;
					}
				}
				if (available.size() < 2) error("map has fewer than 2 player slots");
				// Single-player melee is not networked, so determinism does
				// not matter; seed from the clock.
				ui.st.lcg_rand_state = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
				// Activate up to player_count of the map's slots. The local player
				// plays; the extra slots are opponents (passive until AI/LAN — a
				// sandbox that exercises the FFA/Teams alliance + vision machinery).
				int n = player_count; if (n < 2) n = 2; if (n > (int)available.size()) n = (int)available.size();
				for (int k = 0; k < n; ++k) {
					size_t slot = available[(size_t)k];
					ui.st.players.at(slot).controller = player_t::controller_occupied;
					active_slots.push_back(slot);
				}
				chosen_local = (int)available[0];
				chosen_enemy = (int)available[1];
				// v0.43: per-slot races from --races (index k = active_slots[k];
				// index 0 = local player). Falls back to --race/--enemy-race when
				// the CSV is absent. A value outside 0..2 means "Random": the slot
				// keeps the map race and the loop below randomizes it.
				a_vector<int> slot_races;
				{
					size_t sp = 0;
					while (sp < slot_races_csv.size()) {
						size_t c = slot_races_csv.find(',', sp);
						if (c == a_string::npos) c = slot_races_csv.size();
						slot_races.push_back(atoi(slot_races_csv.substr(sp, c - sp).c_str()));
						sp = c + 1;
					}
				}
				if (!slot_races.empty()) {
					for (size_t k = 0; k < active_slots.size(); ++k) {
						int r = k < slot_races.size() ? slot_races[k] : -1;
						if (r >= 0 && r <= 2) ui.st.players.at(active_slots[k]).race = (race_t)r;
					}
				} else {
					if (local_race >= 0 && local_race <= 2) ui.st.players.at(chosen_local).race = (race_t)local_race;
					if (enemy_race >= 0 && enemy_race <= 2) ui.st.players.at(chosen_enemy).race = (race_t)enemy_race;
				}
				// #1: per-slot player colours from --colors (index k = active_slots[k]).
				// Render-only (player.color drives only the unit/minimap palette), so no
				// determinism impact -- every AI opponent + the human recolours to its pick.
				{
					size_t sp = 0, k = 0;
					while (sp < slot_colors_csv.size() && k < active_slots.size()) {
						size_t c = slot_colors_csv.find(',', sp);
						if (c == a_string::npos) c = slot_colors_csv.size();
						int col = atoi(slot_colors_csv.substr(sp, c - sp).c_str());
						if (col >= 0 && col < 16) ui.st.players.at(active_slots[k]).color = col;   // 16 rows in tunit.pcx
						sp = c + 1; ++k;
					}
				}
				// A race-specific bot overrides whatever race its slot was given,
				// otherwise it starts with a base it cannot use and never acts.
				if (bot_race >= 0) {
					ui.st.players.at(chosen_enemy).race = (race_t)bot_race;
					log("bot: declares race %d, slot %d race forced to match\n", bot_race, chosen_enemy);
				}
				for (auto& v : ui.st.players) {
					if (v.controller == player_t::controller_occupied && (int)v.race > 2) {
						// Loading screen (render-only): remember this slot picked Random.
						ui.loading_slot_random[(size_t)(&v - ui.st.players.data())] = true;
						ui.st.lcg_rand_state = ui.st.lcg_rand_state * 22695477 + 1;
						v.race = (race_t)((ui.st.lcg_rand_state >> 16) % 3);
					}
				}
				load_funcs.setup_info.victory_condition = 1;
				load_funcs.setup_info.starting_units = 1;
				// Standard melee: 50 starting minerals (only applied when
				// resource_type is 1).
				load_funcs.setup_info.resource_type = 1;
				load_funcs.setup_info.starting_minerals = 50;
				// The rand state at the end of this callback is what the replay
				// loader must restore (replay.h sets it right before the map
				// load creates the starting units).
				replay_rs.random_seed = ui.st.lcg_rand_state;
			});
			if (team_mode == 1) {
				// Two teams: first half of the active slots on team 0, rest on 1.
				int half = ((int)active_slots.size() + 1) / 2;
				for (int k = 0; k < (int)active_slots.size(); ++k) teams[active_slots[(size_t)k]] = k < half ? 0 : 1;
			}
			apply_team_alliances(ui.st, active_slots, teams);
			if (!bot_path.empty() && !net_host && net_join.empty()) {
				// v0.24: a bot .so plays the enemy slot (kept occupied, so it
				// uses the same action path as a human). Overrides vs-ai for
				// that slot.
				m.bot_level = bot_level;   // before create(): the bot reads difficulty() at start
				m.load_bot(bot_path, chosen_enemy);
			} else if (vs_ai) {
				// v0.21: every non-local active slot becomes a computer running
				// the original melee aiscript AI for its race (TMCu/ZMCu/PMCu).
				for (size_t k = 1; k < active_slots.size(); ++k) {
					int slot = (int)active_slots[k];
					ui.st.players[slot].controller = player_t::controller_computer_game;
					race_t r = ui.st.players[slot].race;
					uint32_t id;
					if (r == race_t::zerg) id = (uint32_t)'Z' | ((uint32_t)'M' << 8) | ((uint32_t)'C' << 16) | ((uint32_t)'u' << 24);
					else if (r == race_t::protoss) id = (uint32_t)'P' | ((uint32_t)'M' << 8) | ((uint32_t)'C' << 16) | ((uint32_t)'u' << 24);
					else id = (uint32_t)'T' | ((uint32_t)'M' << 8) | ((uint32_t)'C' << 16) | ((uint32_t)'u' << 24);
					bool ok = ui.ai_start_script(slot, id);
					if (ok && slot < 8) replay_rs.ai_script[(size_t)slot] = id;   // playback restarts it (trailer v2)
					log("vs-ai: slot %d race %d id 0x%08x script %s (aiscript %d bytes, bwscript %d bytes)\n",
					    slot, (int)r, id, ok ? "started" : "NOT FOUND",
					    (int)ui.global_st.aiscript_bin.size(), (int)ui.global_st.bwscript_bin.size());
				}
			}
			m.live_mode = true;
			m.local_player = chosen_local;
			m.enemy_player = chosen_enemy;
			ui.game_input = true;
			ui.game_local_player = chosen_local;
			// (Melee player colours come from --colors, applied per-slot above.)
			ui.map_mpq_path = map_path;
			// Loading screen (render-only): the map's display name (a bundled map
			// is loaded from a temp copy, so prefer its MPQ-internal name) and how
			// the bot slot is labelled. Everything else -- races, colours, teams,
			// start locations, the minimap -- ui.h reads from the loaded game.
			ui.loading_map_label = !map_mpq.empty() ? map_mpq : map_path;
			if (!bot_path.empty() && !net_host && net_join.empty() && chosen_enemy >= 0 && chosen_enemy < 12) {
				a_string bn = bot_path;
				size_t bsl = bn.find_last_of("/\\");
				if (bsl != a_string::npos) bn = bn.substr(bsl + 1);
				if (bn.compare(0, 3, "lib") == 0) bn = bn.substr(3);
				size_t bdot = bn.rfind('.');
				if (bdot != a_string::npos) bn = bn.substr(0, bdot);
				if (bn.find("unifiedai") != a_string::npos) {
					// The built-in AI: the launcher only ever shows it as a difficulty.
					static const char* level_names[4] = {"Very easy", "Easy", "Medium", "Hard"};
					ui.loading_slot_name[(size_t)chosen_enemy] = ui.L("Computer");
					ui.loading_slot_role[(size_t)chosen_enemy] = ui.L(level_names[bot_level < 0 ? 0 : (bot_level > 3 ? 3 : bot_level)]);
				} else {
					ui.loading_slot_name[(size_t)chosen_enemy] = bn; // e.g. "steamhammer"
					ui.loading_slot_role[(size_t)chosen_enemy] = ui.L("Bot");
				}
			}
			ui.smart_rally = true; // melee: rally-to-resource gathers, rally-to-enemy attacks
			ui.st.game->qol.smart_rally = true;   // v1.0.2: recorded (qol bit7), so playback runs it too
			// Action routing for direct action_* calls uses the owner index
			// directly, but set player_id so any raw-action path also works.
			for (size_t i = 0; i != 12; ++i) ui.action_st.player_id[i] = (int)i;
			log("melee: local player %d, enemy %d\n", chosen_local, chosen_enemy);

			if (record_replay && init_replay_recording(load_funcs.setup_info, (int)active_slots.size())) {
				for (size_t k = 0; k != active_slots.size(); ++k) {
					replay_rs.player_names[active_slots[k]] = k == 0
							? (player_name.empty() ? a_string("Player") : player_name)
							: a_string("Opponent");
				}
				ui.game_action_record = [&](const uint8_t* d, size_t n) {
					replay_rsf.add_action((int)ui.st.current_frame, ui.game_local_player, d, n);
				};
			}
			} // !ums
		} else {
			ui.load_replay_file(replay_path);
			// Timeline marks + the Economy tab: scan the whole replay on a worker
			// thread, on its own copy of the frame-0 state (ui.h replay_scanner).
			ui.replay_scan_start();
			if (ui.replay_st.qol_bits)
				log("SCBW2 qol restored from replay trailer: bits=%u (multiselect=%d workerrally=%d)\n",
				    (unsigned)ui.replay_st.qol_bits, (int)ui.st.game->qol.multi_building_select, (int)ui.st.game->qol.worker_rally_split);

			// Replays only record human actions; computer AI players would
			// have to be re-simulated and OpenBW has no aiscript AI (the game
			// would hit error("ComputerAI") a few frames in). Reject such
			// replays with a specific message; anything else unsupported
			// lands in the catch.
			// v1.0.2: an OWN recording carries what the computer slots ran (trailer
			// version 2) and the built-in AI's commands as actions, so it plays back
			// exactly (measured with tools/botmatch --record: native AI, bot vs native,
			// bot vs bot -- end state identical after 12000 frames). Only a FOREIGN
			// replay with computer players stays out of reach: the original's AI
			// decisions are not in the file.
			for (size_t i = 0; i != 8 && !ui.replay_st.has_setup; ++i) {
				if (ui.st.players[i].controller == player_t::controller_computer_game) {
					SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW",
						"Sis replay yra zaidimo pries kompiuteri irasas.\n"
						"AI veiksmai replay faile nesaugomi, o OpenBW AI simuliuoti negali,\n"
						"todel toks replay neatkuriamas. Veikia zmogus-pries-zmogu replay.", nullptr);
					return 1;
				}
			}
		}

		// open1161HD Faze 4.1: logical->physical upscale filter. Default nearest
		// (crisp pixel art); --scale-filter linear makes it bilinear (softer, may
		// help HD2 look less blocky). A/B knob -- must be set before the renderer
		// texture is created. Determinism-neutral (pure presentation).
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_filter_linear ? "1" : "0");

		// SDL on Android ignores the requested size and goes fullscreen
		// immediately, so no resize event follows - query the real render
		// size from the window after creation.
		auto& wnd = ui.wnd;
		wnd.create("OpenBW", 0, 0, 1280, 800);

		int render_width = 0;
		int render_height = 0;
		wnd.get_render_size(&render_width, &render_height);
		size_t screen_width = render_width;
		size_t screen_height = render_height;
		log("render size: %dx%d (scale %d)\n", render_width, render_height, render_scale);
		// HUD v3 sizes are dp: the game surface's height in dp (device px / density)
		if (density_milli > 0) ui.hud3_surface_dp_h = (int)((long long)render_height * render_scale * 1000 / density_milli);
		if (ui.hud_v3) log("HUD v3: surface %d dp high, template %d, dock %d, scale %d%%, wheel %d ms\n",
			ui.hud3_surface_dp_h, ui.hud3_template, ui.hud3_dock_mode, ui.hud3_scale_pct, ui.hud3_wheel_ms);

		ui.resize(screen_width, screen_height);
		ui.screen_pos = {(int)ui.game_st.map_width / 2 - (int)screen_width / 2, (int)ui.game_st.map_height / 2 - (int)screen_height / 2};
		if (melee && m.local_player >= 0) {
			// Start with the camera on the local player's MAIN BASE. Prefer the start
			// location (where the town hall spawns); fall back to the first starting
			// unit. Use center_camera_on: it computes screen_pos in WORLD units
			// (correct at FULL HD, where the buffer is world_scale x the view) and is
			// HUD-aware + map-clamped. The old code subtracted raw buffer pixels, so
			// at FULL HD (view_scale=2) it was off by a full view-width -- which is
			// why the base landed far off to the side.
			xy p = ui.game_st.start_locations[m.local_player];
			if (p == xy()) {
				auto& units = ui.st.player_units.at(m.local_player);
				if (!units.empty()) p = units.front().sprite->position;
			}
			if (p != xy()) ui.center_camera_on(p);
		}

		ui.set_image_data();

		// v1.0.1: a map built with SC:R's extended tilesets, loaded with the 1.16.1
		// tables (no SC:R files set), has null tiles where its ramps and paths are --
		// units cannot get out of their bases and a replay of it cannot play right
		// (measured: somajyj 1007 tiles, selection misses 954 vs 3 with SC:R files).
		// Say so instead of playing a broken game silently.
		if (ui.game_st.tiles_beyond_tileset) {
			log("map: %d tiles are not in the 1.16.1 tileset (SC:R map); casc=%d\n", (int)ui.game_st.tiles_beyond_tileset, (int)casc_ok);
			ui.show_hud_message(ui.L("This map uses StarCraft: Remastered terrain. Set the SC:R files in Settings."));
		}

		// SCBW2 house-rules (SINGLE-PLAYER path): apply the launcher's --qol bitmask to
		// the loaded game_state. NET games take the host's negotiated bitmask instead
		// (task 1): the host broadcasts start_game_qol with the start-game message and
		// every peer applies the identical value in the net reload callback below, so
		// all clients share one game_state.qol and lockstep stays deterministic -- this
		// path is therefore gated !net_mode so the two never double-apply. Replays are
		// still excluded (a recorded .rep carries no house-rules). Sim reads st.game->qol.
		if (replay_path.empty() && !net_mode && qol_bits) {
			auto& q = const_cast<bwgame::game_state&>(ui.game_st).qol;
			q.multi_building_select = (qol_bits >> 0) & 1;
			q.group_rally           = (qol_bits >> 1) & 1;
			q.mbs_train             = (qol_bits >> 2) & 1;
			q.unlimited_selection   = (qol_bits >> 3) & 1;
			q.smart_cast            = (qol_bits >> 4) & 1;
			q.auto_worker           = (qol_bits >> 5) & 1;
			// bit6 was reserved for auto_supply (abandoned by design). Reused for the
			// worker-rally split (SC2-style: a rally onto a resource sets a separate
			// worker-only rally). MUST stay behind this SP-only gate -- it changes the
			// sim interpretation of a rally-onto-resource command, so a replay recorded
			// by a vanilla engine (SC:R .rep) desyncs if it runs during playback. The
			// reptags harness caught exactly that regression (herO_Light sel_null 1->2043).
			q.worker_rally_split    = (qol_bits >> 6) & 1;
			log("SCBW2 qol bits=%d (multiselect=%d grouprally=%d)\n", qol_bits,
			    (int)q.multi_building_select, (int)q.group_rally);
		}

		if (net_mode) {
			// --- LAN lobby -----------------------------------------------
			// Pump the sync layer (heartbeats + lobby messages) while
			// rendering the map with a status line, until the host starts
			// the game. Slots converge on their own: everyone tries the
			// lowest open slot, the occupy handler resolves races.
			ui.exit_on_close = false;
			ui.lobby_mode = true;
			// v0.48: with --net-fast the launcher already ran the whole lobby, so
			// show a plain loading screen (progress bar) instead of the in-engine
			// slot table + the Java overlay.
			ui.loading_mode = net_fast;
			ui.lobby_is_host = net_host;               // v0.39 lobby slot table
			{
				a_string mn = map_path;
				size_t sl = mn.rfind('/');
				if (sl != a_string::npos) mn = mn.substr(sl + 1);
				ui.lobby_map_name = mn;
			}
			// Loading screen (render-only): map display name (a bundled map loads from
			// a temp copy, so prefer its MPQ-internal name) + the local lobby colour.
			ui.loading_map_label = !map_mpq.empty() ? map_mpq : map_path;
			ui.loading_local_color = player_color;
			sync_st.setup_info = &load_funcs.setup_info;
			sync_st.game_type_melee = true;
			// task 1: the HOST negotiates the SCBW2 house-rules. Its --qol bitmask is
			// broadcast with the start-game message so every peer adopts the identical
			// config (see sync.h send_start_game). Only the host's value is sent; a
			// joiner's local qol_bits is ignored so the session always runs the host's
			// rules. Determinism holds because all clients apply the SAME bitmask.
			sync_st.host_qol = (uint32_t)qol_bits;
			net.set_local_client_name(!player_name.empty() ? player_name : (net_host ? "host" : "guest"));
			udp_responder discovery;
			if (net_host) {
				server.bind(a_string("0.0.0.0"), net_port);
				// Announce this lobby over UDP so joiners can auto-find it.
				// Map label = filename without the directory prefix.
				a_string label = map_path;
				size_t slash = label.rfind('/');
				if (slash != a_string::npos) label = label.substr(slash + 1);
				discovery.open(net_port, label);
				{
					int target = player_count < 2 ? 2 : (player_count > 8 ? 8 : player_count);
					discovery.set_players(1, target, app_version, mods_csv, game_speed_pct);
				}
			} else {
				server.connect(net_join, net_port);
			}
			ui.lobby_status = net_host ? "LAUKIAMA ZAIDEJO..." : "JUNGIAMASI...";
			log("net lobby: start\n");

			bool sent_race = false;
			auto lobby_start = std::chrono::steady_clock::now();
			// Joiner connect-retry: asio's async_connect fires once and does not
			// retry, so if the host's TCP server isn't up yet (it launches ~1-2s
			// after the launcher lobby hands off) the single attempt fails and the
			// joiner would just time out. On a LAN a refused connect fails fast, so
			// re-issuing every second until connected is safe (no lingering sockets).
			auto last_connect = lobby_start;
			// LINK SETTLE: the host does not start the instant the last
			// peer takes a slot. It waits LINK_SETTLE_MS with the roster complete while
			// the lobby keeps pumping net.sync -- every connection has then carried a
			// few round trips (game_info / race / ready all delivered and acked) before
			// the start message and the lockstep begin. Only the host decides the start,
			// so every peer is delayed equally. Also gives the loading screen's roster a
			// moment on screen instead of a single frame.
			const int LINK_SETTLE_MS = 1500;
			bool roster_full = false;
			auto roster_full_since = lobby_start;
			while (!sync_st.game_started) {
				if (!net_host && net.connected_player_count() < 2) {
					auto now2 = std::chrono::steady_clock::now();
					if (now2 - last_connect >= std::chrono::seconds(1)) {
						server.connect(net_join, net_port);
						last_connect = now2;
					}
				}
				net.sync(server);
				discovery.poll();
				// v0.39: project the lobby snapshot into the slot table (the UI
				// only renders this; it never edits shared lobby state).
				ui.lobby_slots.clear();
				for (auto& c : sync_st.clients) {
					if (c.player_slot < 0) continue;
					ui.lobby_slots.emplace_back();
					auto& s = ui.lobby_slots.back();
					s.name = c.name.empty() ? a_string("?") : c.name;
					s.slot = c.player_slot;
					s.race = c.player_slot < 12 ? (int)ui.st.players[c.player_slot].race : -1;
					s.is_local = sync_st.local_client && c.player_slot == sync_st.local_client->player_slot;
					s.ready = c.ready;
				}
				// v0.41: publish the lobby snapshot as JSON for the Java overlay
				// lobby, and consume the ready/leave actions it pushes back.
				bool overlay_leave = false;
				if (net_fast) {
					// Launcher already ran the lobby: just advance a loading bar.
					double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - lobby_start).count();
					int connected = net.connected_player_count();
					int target = player_count < 2 ? 2 : (player_count > 8 ? 8 : player_count);
					int slots = 0; for (auto& c : sync_st.clients) if (c.player_slot != -1) ++slots;
					float base = (float)std::min(0.85, el / 2.5);
					if (connected >= 2) base = std::max(base, 0.6f);
					if (slots >= target) {
						// roster complete: the bar runs out the link-settle hold
						float t = roster_full ? (float)std::chrono::duration<double>(std::chrono::steady_clock::now() - roster_full_since).count() * 1000.0f / (float)LINK_SETTLE_MS : 0.0f;
						if (t > 1.0f) t = 1.0f;
						base = std::max(base, 0.80f + 0.20f * t);
					}
					ui.loading_progress = base;
					ui.lobby_status = net_host ? "Laukiama zaideju..." : "Jungiamasi prie hosto...";
				} else {
					std::string j = std::string("{\"host\":") + (net_host ? "true" : "false");
					j += ",\"map\":\"" + json_esc(std::string(ui.lobby_map_name.c_str())) + "\"";
					j += ",\"status\":\"" + json_esc(std::string(ui.lobby_status.c_str())) + "\"";
					// P4: the rules this session will run -- host: its own bitmask; joiner:
					// what game_info carried from the host (0 until the first game_info).
					j += ",\"qol\":" + std::to_string(net_host ? (unsigned)qol_bits : (unsigned)sync_st.lobby_qol);
					j += ",\"players\":[";
					bool first = true;
					for (auto& s : ui.lobby_slots) {
						if (!first) j += ",";
						first = false;
						j += "{\"name\":\"" + json_esc(std::string(s.name.c_str())) + "\",\"race\":" + std::to_string(s.race)
						   + ",\"slot\":" + std::to_string(s.slot) + ",\"ready\":" + (s.ready ? "true" : "false")
						   + ",\"local\":" + (s.is_local ? "true" : "false") + "}";
					}
					j += "]}";
					std::lock_guard<std::mutex> lk(g_lobby.mtx);
					g_lobby.active = true;
					g_lobby.state_json = std::move(j);
					if (g_lobby.pending_ready >= 0) {
						ui.lobby_local_ready = g_lobby.pending_ready != 0;
						ui.lobby_ready_dirty = true;
						g_lobby.pending_ready = -1;
					}
					if (g_lobby.pending_leave) { g_lobby.pending_leave = false; overlay_leave = true; }
				}
				// Ready toggle (native tap or overlay) -> sync; leave -> return.
				if (ui.lobby_ready_dirty) {
					ui.lobby_ready_dirty = false;
					net.set_local_client_ready(server, ui.lobby_local_ready);
				}
				if (ui.lobby_leave_request || overlay_leave) {
					ui.lobby_leave_request = false;
					{ std::lock_guard<std::mutex> lk(g_lobby.mtx); g_lobby.active = false; }
					discovery.close();
					net.leave_game(server);
					return 0;
				}
				// Join connect timeout: if we never see the host (wrong IP,
				// host not started, different Wi-Fi), don't hang on
				// "JUNGIAMASI..." forever - tell the user and bail out.
				if (!net_host && net.connected_player_count() < 2 &&
						std::chrono::steady_clock::now() - lobby_start > std::chrono::seconds(20)) {
					log("net lobby: join timed out\n");
					discovery.close();
					net.leave_game(server);
					// Show the failure on the lobby screen itself (a modal
					// message box is unreliable while lobby_mode renders) and
					// hold it long enough to read before returning to launcher.
					ui.lobby_status = "NEPAVYKO PRISIJUNGTI - PATIKRINKITE IP IR WIFI";
					auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
					while (std::chrono::steady_clock::now() < until && !ui.window_closed) {
						ui.update();
						std::this_thread::sleep_for(std::chrono::milliseconds(42));
					}
					return 0;
				}
				auto* lc = sync_st.local_client;
				if (lc->player_slot == -1) {
					// A new client's id_client_uid resets every slot to -1 and every
					// race to the map default (5), and clears scheduled actions - so
					// whenever we lose our slot we must re-pick it AND re-send our
					// race. Without this only the last-joined player kept their race
					// (the host's got knocked back to random).
					sent_race = false;
					for (int i = 0; i != 8; ++i) {
						if (ui.st.players[i].controller == player_t::controller_open) {
							net.switch_to_slot(server, i);
							break;
						}
					}
				} else if (!sent_race) {
					if (local_race >= 0 && local_race <= 2) net.set_local_client_race(server, (race_t)local_race);
					sent_race = true;
				} else if (local_race >= 0 && local_race <= 2 &&
						(int)ui.st.players[lc->player_slot].race != local_race) {
					// Belt-and-suspenders: if our slot's race drifted from our pick
					// (a late reset we didn't observe as slot==-1), re-assert it.
					net.set_local_client_race(server, (race_t)local_race);
				}
				if (net.connected_player_count() >= 2) {
					int slots = 0;
					for (auto& c : sync_st.clients) {
						if (c.player_slot != -1) ++slots;
					}
					int target = player_count < 2 ? 2 : (player_count > 8 ? 8 : player_count);
					int ready_count = 0;
					for (auto& c : sync_st.clients) if (c.player_slot != -1 && c.ready) ++ready_count;
					if (!net_fast) {
						if (slots < target)
							ui.lobby_status = format("ZAIDEJAI %d/%d - LAUKIAMA...", slots, target);
						else
							ui.lobby_status = format("PASIRUOSE %d/%d - spauskite PASIRUOSTI", ready_count, slots);
					}
					if (net_host) discovery.set_players(slots, target, app_version, mods_csv, game_speed_pct);
					// v0.40: host starts once all expected players joined AND every
					// occupied client toggled ready (replaces the auto-start).
					// net_fast: the launcher lobby already handled ready-up, so start
					// as soon as everyone has connected (no second in-game ready).
					if (slots >= target) {
						if (!roster_full) { roster_full = true; roster_full_since = std::chrono::steady_clock::now(); }
					} else {
						roster_full = false;   // someone left: the settle window restarts
					}
					const bool settled = roster_full
						&& std::chrono::steady_clock::now() - roster_full_since >= std::chrono::milliseconds(LINK_SETTLE_MS);
					if (net_host && settled && (net_fast || net.all_clients_ready())) net.start_game(server);
				}
				ui.update();
				if (ui.window_closed) {
					discovery.close();
					net.leave_game(server);
					return 0;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(42));
			}
			discovery.close();
			ui.lobby_status.clear();
			ui.loading_progress = 1.0f; // bar full at the hand-off to the game
			ui.loading_mode = false;
			ui.lobby_mode = false;
			{ std::lock_guard<std::mutex> lk(g_lobby.mtx); g_lobby.active = false; } // hide overlay

			// --- Game start ----------------------------------------------
			// start_game() already assigned final slots, races and the
			// shared seed identically on both clients. Reset the state and
			// reload the map so create_starting_units runs deterministically
			// on both sides.
			auto players = ui.st.players;
			auto seed = ui.st.lcg_rand_state;
			int local_slot = sync_st.local_client->player_slot;
			bwgame::static_vector<size_t, 12> active_slots;
			int enemy_slot = -1;
			for (auto& c : sync_st.clients) {
				if (c.player_slot != -1) {
					active_slots.push_back((size_t)c.player_slot);
					if (c.player_slot != local_slot && enemy_slot < 0) enemy_slot = c.player_slot;
				}
			}
			if (local_slot < 0 || active_slots.size() < 2) error("lobby ended without two occupied slots");
			log("net game start: local slot %d, %d players, seed %u\n", local_slot, (int)active_slots.size(), (unsigned)seed);

			auto* g = ui.st.global;
			auto* gs = ui.st.game;
			ui.st = bwgame::state();
			*gs = bwgame::game_state();
			ui.st.global = g;
			ui.st.game = gs;
			ui.action_st = bwgame::action_state();
			game_load_functions reload_funcs(ui.st);
			reload_funcs.load_map_file(map_path, [&]() {
				ui.st.players = players;
				ui.st.lcg_rand_state = seed;
				reload_funcs.setup_info.victory_condition = 1;
				reload_funcs.setup_info.starting_units = 1;
				reload_funcs.setup_info.resource_type = 1;
				reload_funcs.setup_info.starting_minerals = 50;
				// task 1: apply the HOST-negotiated SCBW2 house-rules. start_game_qol
				// was broadcast identically to every peer (sync.h), so all clients set
				// the SAME game_state.qol here -> lockstep-deterministic. The reload
				// zeroed qol, so this must run inside the load callback (like the seed).
				// NET-SAFE SUBSET ONLY (mask 0x1E = bits 1..4): group_rally, mbs_train,
				// unlimited_selection, smart_cast are UI-fan/selection rules whose
				// actions flow through the networked action stream, so they are
				// identical on every peer. Excluded on purpose:
				//   bit0 multi_building_select -- persistent sim flag with no replay-
				//     header slot yet; a .rep saved from the net game would desync.
				//   bit5 auto_worker -- its per-frame injector is net-gated off
				//     (qol_auto_worker_tick runs only when !net_next_frame), so the
				//     rule would be an inert, dead button over LAN.
				//   bit6 worker_rally_split -- SP-only by design (v0.91.017): it
				//     reinterprets a rally-onto-resource in the sim.
				//   P5 (v0.101.001): bit0 is now allowed over LAN -- the replay saver
				//     writes the negotiated set into the .rep trailer and playback
				//     restores it, so a net-recorded replay no longer desyncs.
				uint32_t nq = sync_st.start_game_qol & 0x1Fu;
				auto& q = ui.st.game->qol;
				q.multi_building_select = (nq >> 0) & 1;
				q.group_rally           = (nq >> 1) & 1;
				q.mbs_train             = (nq >> 2) & 1;
				q.unlimited_selection   = (nq >> 3) & 1;
				q.smart_cast            = (nq >> 4) & 1;
			});
			if (sync_st.start_game_qol)
				log("SCBW2 qol negotiated over LAN: host bits=%u\n", (unsigned)sync_st.start_game_qol);
			// FFA alliances (each occupied slot its own team). Deterministic and
			// identical on every client, so no team config needs to be synced.
			// Team play over LAN is a follow-up (needs the team map synced).
			apply_team_alliances(ui.st, active_slots, parse_teams(a_string()));

			m.live_mode = true;
			m.local_player = local_slot;
			m.enemy_player = enemy_slot;
			ui.game_input = true;
			ui.game_local_player = local_slot;
			// LAN: the local player's lobby-chosen colour (render-only). The lobby
			// blocks duplicate colours, so every peer's own slot gets a distinct one.
			if (player_color >= 0 && player_color < 16 && local_slot >= 0 && local_slot < 12)
				ui.st.players[local_slot].color = player_color;
			ui.smart_rally = true; // melee/LAN: rally-to-resource gathers, rally-to-enemy attacks
			ui.st.game->qol.smart_rally = true;   // v1.0.2: recorded (qol bit7); every peer sets it
			// v0.44: the LAN sim advances through `net` (a separate state_functions
			// instance over the same state), so smart_rally must be enabled there
			// too - otherwise rally-to-gather silently no-ops in LAN games.
			net.smart_rally = true;
			// v0.52: when a peer leaves this LAN melee, hand their units to the
			// aiscript AI instead of neutralizing them (only fires when the
			// local sim is the sole remaining human - see actions.h).
			net.ai_takeover_on_leave = true;
			for (size_t i = 0; i != 12; ++i) ui.action_st.player_id[i] = (int)i;

			// LAN recording: the reload above restored exactly the players +
			// seed a replay loader would (same semantics as replay.h), so the
			// header can be filled here; sync_st.save_replay then taps every
			// peer's actions as the lockstep layer executes them.
			replay_rs.random_seed = seed;
			// v0.83: the UI needs the same names to attribute incoming chat
			// lines (opcode 92), and it needs them whether or not a replay is
			// being recorded -- so fill them outside that branch.
			for (auto& c : sync_st.clients) {
				if (c.player_slot >= 0 && c.player_slot < 12)
					ui.slot_player_names[(size_t)c.player_slot] = c.name;
			}
			if (record_replay && init_replay_recording(reload_funcs.setup_info, (int)active_slots.size())) {
				for (auto& c : sync_st.clients) {
					if (c.player_slot != -1) replay_rs.player_names[c.player_slot] = c.name;
				}
				sync_st.save_replay = &replay_rs;
			}
			ui.game_action_submit = [&](const uint8_t* d, size_t n) {
				net.input_action(server, d, n);
			};
			m.net_next_frame = [&]() {
				net.next_frame(server);
			};
			// v0.54 (experimental FPS parity): non-blocking pump + "advancing one
			// frame won't block" predicate, so update_live() renders while waiting.
			m.net_poll = [&]() { net.poll(server); };
			m.net_ready = [&]() { return net.ready_to_advance(server); };
			m.last_net_advance = clock.now();
			ui.current_selection_clear();
			// Camera on the local player's base at net-game start (same world-units /
			// FULL HD fix as the melee path above -- prefer the start location).
			xy p = ui.game_st.start_locations[local_slot];
			if (p == xy()) {
				auto& units = ui.st.player_units.at(local_slot);
				if (!units.empty()) p = units.front().sprite->position;
			}
			if (p != xy()) ui.center_camera_on(p);
		}

		log("loaded in %dms\n", std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - start).count());

		// --- v0.86 disk save/load wiring ------------------------------------
		// Single-player only (net games gate save/load in the UI). map_id is a
		// stable per-map id (crc32 of the map reference string) that both a save
		// and a later relaunch compute identically; engine_version is the app
		// versionCode, so a save from an incompatible build is rejected. save_args
		// is the launch argv MINUS --load, so the launcher can relaunch this exact
		// game and then re-apply the save.
		if (melee) {
			a_string map_ref = campaign ? campaign_dir : (!map_mpq.empty() ? map_mpq : map_path);
			a_string map_disp = map_ref;
			size_t sl = map_disp.find_last_of("/\\");
			if (sl != a_string::npos) map_disp = map_disp.substr(sl + 1);
			ui.save_dir = data_dir + "/saves";
			::mkdir(ui.save_dir.c_str(), 0770);
			// Save/load submilestone 1: campaign vs single-player saves live in
			// separate slot namespaces (cslotN vs slotN) so the pause-menu Save/Load
			// never crosses them.
			ui.is_campaign_game = campaign;
			ui.save_engine_version = (uint32_t)version_code;
			ui.save_map_id = data_loading::crc32_t()((const uint8_t*)map_ref.data(), map_ref.size());
			ui.save_map_name = map_disp;
			{
				// Relaunch args = every original arg except --load and --version-code
				// (the launcher re-adds the CURRENT --version-code on each launch, so
				// a save made by an older build is correctly rejected after an update).
				a_string a;
				for (int i = 1; i < argc; ++i) {
					if (!argv[i]) continue;
					a_string cur = argv[i];
					if (cur == "--load" || cur == "--version-code") { ++i; continue; }
					if (!a.empty()) a += '\n';
					a += cur;
				}
				ui.save_args = a;
			}

			// Cross-session load: the normal map load above already rebuilt game_st
			// with the same args, so applying the blob now restores the exact saved
			// state onto the live st (never rebinding st, so the bound game_st/
			// global_st refs stay valid). A loaded game does not record a replay.
			if (!load_savefile.empty()) {
				a_vector<uint8_t> buf;
				FILE* lf = fopen(load_savefile.c_str(), "rb");
				if (lf) {
					fseek(lf, 0, SEEK_END); long sz = ftell(lf); fseek(lf, 0, SEEK_SET);
					if (sz > 0) { buf.resize((size_t)sz); if (fread(buf.data(), 1, (size_t)sz, lf) != (size_t)sz) buf.clear(); }
					fclose(lf);
				}
				size_t off = 0; bool ok = buf.size() >= 20;
				auto ru32 = [&](uint32_t& v){ v = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) | ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24); off += 4; };
				uint32_t magic = 0, ver = 0, frame = 0, mid = 0, eng = 0;
				if (ok) { ru32(magic); ru32(ver); ru32(frame); ru32(mid); ru32(eng); ok = magic == ui.sav_magic && ver == ui.sav_container_version; }
				for (int s = 0; ok && s < 2; ++s) {   // skip map_name, args strings
					if (off + 4 > buf.size()) { ok = false; break; }
					uint32_t n; ru32(n);
					if (off + n > buf.size()) { ok = false; break; }
					off += n;
				}
				if (ok && eng == ui.save_engine_version && mid == ui.save_map_id) {
					auto lr = savegame::load(ui.st, ui.action_st, buf.data() + off, buf.size() - off,
						ui.save_engine_version, ui.save_map_id);
					if (lr == savegame::load_result::ok) {
						log("save loaded: %s (frame %d)\n", load_savefile.c_str(), (int)ui.st.current_frame);
						int lp = m.local_player;
						if (lp >= 0 && lp < 12) {
							xy p = ui.game_st.start_locations[lp];
							auto& us = ui.st.player_units.at(lp);
							if (!us.empty()) p = us.front().sprite->position;
							if (p != xy()) ui.center_camera_on(p);
						}
					} else {
						log("save load FAILED: load_result %d\n", (int)lr);
					}
				} else {
					log("save load REJECTED: ok=%d eng=%u/%u map=%u/%u\n",
						(int)ok, eng, ui.save_engine_version, mid, ui.save_map_id);
				}
			}
		}

		// Writes the recorded game to replays/LastGame.rep (overwritten every
		// game, like BW's LastReplay.rep). Only meaningful when recording is
		// active and at least one action landed.
		auto save_recorded_replay = [&]() {
			// A game resumed from a disk save starts recording mid-game, so its
			// action history would produce an invalid partial .rep -- skip it.
			if (!load_savefile.empty()) return;
			if (!record_replay || !replay_rs.map_data) return;
			// (an empty action history is still a game: the player may only have watched)
			if (replay_rs.history.empty() && ui.st.current_frame < 24) return;
			try {
				// P5: persist the house-rules the game ran with (SP --qol or the
				// host-negotiated LAN set) so playback re-applies the sim-side ones.
				{
					auto& q = ui.st.game->qol;
					replay_rs.qol_bits = (q.multi_building_select ? 1u : 0u) | (q.group_rally ? 2u : 0u)
						| (q.mbs_train ? 4u : 0u) | (q.unlimited_selection ? 8u : 0u) | (q.smart_cast ? 16u : 0u)
						| (q.auto_worker ? 32u : 0u) | (q.worker_rally_split ? 64u : 0u) | (q.smart_rally ? 128u : 0u);
				}
				a_string path = data_dir + "/replays/LastGame.rep";
				data_loading::file_writer<> w(path);
				replay_rsf.save_replay((int)ui.st.current_frame, w);
				// the end state, so a playback of this file can be checked against the game
				// (`aidump replay` prints the same two numbers)
				int live_units = 0;
				for (int p = 0; p != 12; ++p) for (const unit_t* u : ptr(ui.st.player_units[p])) { (void)u; ++live_units; }
				log("replay saved: %s (%d frames) lcg_rand_state=%u units=%d\n", path.c_str(), (int)ui.st.current_frame, (unsigned)ui.st.lcg_rand_state, live_units);
			} catch (const std::exception& e) {
				log("replay save failed: %s\n", e.what());
			}
		};

		// RenderPresent (vsync) inside ui.update() paces frames; the sleep
		// only caps the rate if vsync is unavailable (software renderer
		// fallback).
		// Faze 4C: the campaign intro cinematic MUST play after the first
		// m.update() -- that is when rgba_surface/wnd are created. An older comment
		// here claimed the surfaces already existed before the loop and played the
		// cinematic pre-loop; measured otherwise (play_cinematic returned false at
		// its !rgba_surface guard -> "skipped", intro never shown). Same surf=0
		// timing the Faze 4.7 prewarm fix uncovered. Defer it (like test_cinematic)
		// and play it on the first iteration BEFORE prewarm/briefing, so the order
		// stays cinematic -> loading -> briefing -> start with no briefing flash.
		bool intro_cine_pending = !m.intro_cinematic.empty();
		a_string intro_cine = m.intro_cinematic;
		m.intro_cinematic.clear();
		// Faze 4.7 Variant B: debug hook to play a cinematic in any game (no
		// campaign navigation needed) -- e.g. --test-cinematic "smk\\zerg1.smk".
		bool test_cine_pending = !test_cinematic.empty(); // Variant B: play after first update (surface exists)
		// Faze 4.7: warm the HD portraits/UI AFTER the first m.update() -- that is when
		// rgba_surface is created. Before it, hd_prewarm's surf guard fails and it
		// silently skips (why no loading screen showed + briefing idle clips never
		// cached). The first update only runs the briefing's opening Wait, so warming
		// there caches every speaker before any dialogue.
		bool prewarm_pending = true;
		bool prev_briefing = ui.briefing_active; // Faze 4.7: warm again on briefing->mission
		while (true) {
			// Keep the chat bar clear of the soft keyboard (see the JNI setter).
			ui.ime_frac = g_ime_frac.load();
			if (ui.hud_v3) ui.hud3_pen_air(g_pen_air_in.load() != 0,
				(int)((long long)g_pen_air_x.load() * (int)ui.screen_width / 10000),
				(int)((long long)g_pen_air_y.load() * (int)ui.screen_height / 10000), g_pen_air_btn.load() != 0);
			m.update();
			// v0.86 test hook: --autosave-at <frame> writes slot 1 once the sim
			// reaches that frame, so the disk save + cross-session --load path can
			// be validated on-device without touching the in-game pause menu.
			if (autosave_frame >= 0 && !ui.briefing_active && (int)ui.st.current_frame >= autosave_frame) {
				bool ok = ui.disk_save(1);
				log("autosave-at %d: slot 1 %s (%s)\n", autosave_frame, ok ? "written" : "FAILED",
					ui.save_feedback.c_str());
				autosave_frame = -1;
			}
			// DIAG self-test hook: at frame 40 (game settled) run a scripted scenario
			// once, so SCBW2 behaviours can be verified over adb without a touch gesture.
			// Diagnostics-only, never in normal play (--diag-selftest gates it).
			if (!diag_selftest.empty() && !ui.briefing_active && (int)ui.st.current_frame >= 40) {
				ui.run_diag_selftest(diag_selftest.c_str());
				diag_selftest.clear();
			}
			// Faze 4C: intro cinematic first (surface now exists), before the
			// loading/prewarm and briefing, so it isn't preceded by a briefing flash.
			if (intro_cine_pending) {
				intro_cine_pending = false;
				bool ok = ui.play_cinematic(intro_cine);
				log("cinematic: %s -> %s\n", intro_cine.c_str(), ok ? "played" : "skipped");
			}
			// Faze 4.7: NOT in a LAN game -- the warm blocks the loop, which would
			// stall the lockstep sync (peers wait on us) and could miss heartbeats and
			// disconnect. LAN peers start immediately and decode lazily, so no one is
			// left behind / disadvantaged. (LAN is melee-only, no briefing.)
			if (prewarm_pending) {
				prewarm_pending = false;
				if (!net_mode) ui.hd_prewarm(m.local_player);
				// Faze 6: loading done -> release the briefing so it plays from the top
				// with a fresh wall-clock (no fast-forward through the prewarm's elapsed
				// seconds). Cleared unconditionally: harmless when there is no briefing.
				ui.briefing_hold = false;
			}
			// Faze 4.7 (user request): when the briefing hands off to the mission,
			// re-run the warm behind the loading screen so gameplay assets are ready
			// and the map doesn't pop in (cached clips skip instantly).
			if (prev_briefing && !ui.briefing_active && !net_mode) ui.hd_prewarm(m.local_player);
			prev_briefing = ui.briefing_active;
			// Faze 4.7 (user request): SKIP TUTORIAL in the Boot Camp briefing --
			// advance the campaign to the first Terran mission without playing the
			// tutorial, then hand control back to the launcher (which loads
			// campaign-next.txt on resume, exactly like a won mission).
			if (ui.briefing_skip_tutorial) {
				ui.briefing_skip_tutorial = false;
				size_t slash = campaign_dir.rfind('/');
				a_string next_dir = (slash != a_string::npos ? campaign_dir.substr(0, slash + 1)
				                                             : a_string("campaign/terran/")) + "terran01";
				a_string path = m.campaign_data_dir + "/campaign-next.txt";
				FILE* f = fopen(path.c_str(), "wb");
				if (f) { fwrite(next_dir.data(), 1, next_dir.size(), f); fclose(f); }
				log("skip tutorial -> %s\n", next_dir.c_str());
				save_recorded_replay();
				return 0;
			}
			if (test_cine_pending) {
				test_cine_pending = false;
				bool ok = ui.play_cinematic(test_cinematic);
				log("test-cinematic: %s -> %s\n", test_cinematic.c_str(), ok ? "played" : "skipped");
			}
			// Cheats "there is no cow level" / "game over man" force the outcome.
			if (ui.cheat_force_win) { ui.cheat_force_win = false; m.game_over = true; m.game_won = true; }
			if (ui.cheat_force_lose) { ui.cheat_force_lose = false; m.game_over = true; m.game_won = false; }
			if (ui.request_exit || ui.window_closed) {
				// Pause-menu "Leave game" / window close: tear down net, return.
				save_recorded_replay();
				if (net_mode) net.leave_game(server);
				return 0;
			}
			if (m.game_over) {
				log("game over: %s\n", m.game_won ? "victory" : "defeat");
				// v0.31: melee/LAN match result for profile statistics. Campaign
				// wins are tracked separately (campaign-next.txt) and excluded.
				// The launcher reads this on resume and updates per-profile stats.
				if (!campaign) {
					int lp = m.local_player;
					int race = (lp >= 0 && lp < 12) ? (int)ui.st.players[lp].race : 5;
					// A downloaded bot is also an AI opponent, so it counts as "ai".
					const char* mode = net_mode ? "lan"
						: (vs_ai || !bot_path.empty()) ? "ai" : "local";
					a_string rpath = data_dir + "/melee-result.txt";
					FILE* rf = fopen(rpath.c_str(), "wb");
					if (rf) {
						fprintf(rf, "won=%d race=%d mode=%s\n", m.game_won ? 1 : 0, race, mode);
						fclose(rf);
						log("melee result: won=%d race=%d mode=%s -> %s\n",
							m.game_won ? 1 : 0, race, mode, rpath.c_str());
					} else {
						log("melee result: FAILED to open %s\n", rpath.c_str());
					}
				}
				// v0.69 item 14: the full after-action statistics, for the
				// launcher's own score screen. Written for campaign missions
				// too (melee-result.txt above stays melee-only, because it
				// drives the melee win/loss profile stats). One line per
				// participating player; the same numbers the in-engine screen
				// reads, so the two can never disagree.
				{
					a_string spath = data_dir + "/score.txt";
					FILE* sf = fopen(spath.c_str(), "wb");
					if (sf) {
						fprintf(sf, "won=%d\nlocal=%d\nframes=%d\ncampaign=%d\n",
							m.game_won ? 1 : 0, m.local_player, (int)ui.st.current_frame, campaign ? 1 : 0);
						for (int p = 0; p != 12; ++p) {
							auto c = ui.st.players[p].controller;
							if (c != player_t::controller_occupied
							    && c != player_t::controller_computer
							    && c != player_t::controller_computer_game) continue;
							int kills = 0, losses = 0;
							for (size_t k = 0; k != 228; ++k) {
								kills += ui.st.unit_kills[p][(UnitTypes)k];
								losses += ui.st.unit_deaths[p][(UnitTypes)k];
							}
							// color= / portrait= / name= (launcher redesign F4): who this row is.
							// name= is LAST and runs to the end of the line (names may hold spaces).
							a_string nm = ui.player_display_name((size_t)p, m.local_player);
							for (auto& ch : nm) if (ch == '\n' || ch == '\r') ch = ' ';
							int prace = (int)ui.st.players[p].race;
							int portrait = ui.player_portrait_id(p == m.local_player,
								ui.player_is_ai((size_t)p, m.local_player), nm, prace);
							fprintf(sf,
								"p=%d race=%d units=%d buildings=%d kills=%d losses=%d "
								"killscore=%d razescore=%d minerals=%d gas=%d color=%d portrait=%d name=%s\n",
								p, prace,
								(int)ui.st.total_non_buildings_ever_completed[p],
								(int)ui.st.total_buildings_ever_completed[p],
								kills, losses,
								(int)ui.st.kills_score[p], (int)ui.st.razings_score[p],
								(int)ui.st.total_minerals_gathered[p],
								(int)ui.st.total_gas_gathered[p],
								(int)ui.st.players[p].color, portrait, nm.c_str());
						}
						fclose(sf);
						log("score written: %s\n", spath.c_str());
					}
				}
				save_recorded_replay();
				if (net_mode) net.leave_game(server);
				// v0.26: race finale cinematic (campaign only), then stats.
				if (m.game_won && !m.pending_cinematic.empty()) {
					if (ui.play_cinematic(m.pending_cinematic))
						log("cinematic: %s played\n", m.pending_cinematic.c_str());
				}
				// v0.34: in-engine after-action score screen (Victory/Defeat +
				// per-player stats table, tap to continue). Replaces the SDL
				// system message box; falls back to it only if there's no window.
				// v0.69 item 14: the score screen moved into the launcher, which
				// reads score.txt on resume and offers Return / Save Replay. The
				// in-engine one is kept only for the case where there is no
				// window to return through, so the player never sees the same
				// numbers twice.
				if (!(ui.rgba_surface && ui.wnd)) {
					SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "OpenBW",
						ui.L(m.game_won ? "VICTORY!" : "Defeat..."), nullptr);
				}
				return 0;
			}
			// Peer lost: both a clean disconnect (asio on_kill) and a desync
			// (hash mismatch -> kill_client) remove the remote client, so the
			// connected count drops below 2.
			//
			// v0.81.004: this used to abort unconditionally, which quietly
			// defeated the whole v0.52 disconnect->AI feature. sync.h's
			// kill_client already synthesises the player-left action (opcode
			// 87), records it in the replay AND applies it, so the takeover has
			// in fact already happened by the time we get here -- and then we
			// threw the game away. Now we only abort when nobody took over.
			if (net_mode && !peer_lost_handled && sync_st.game_started
					&& ui.st.current_frame > 20 && net.connected_player_count() < 2) {
				peer_lost_handled = true;
				int humans = 0;
				for (int i = 0; i != 8; ++i)
					if (ui.st.players[i].controller == bwgame::player_t::controller_occupied) ++humans;
				bool we_survive = ui.game_local_player >= 0 && ui.game_local_player < 8
					&& ui.st.players[ui.game_local_player].controller
						== bwgame::player_t::controller_occupied;
				if (net.ai_takeover_on_leave && humans == 1 && we_survive) {
					// The leaver's units are now driven by aiscript. Keep
					// is_net_game set: it forces fog (v0.76) and blocks cheats,
					// and flipping it mid-game would suddenly reveal the map.
					log("net: peer lost -- aiscript AI took over, game continues\n");
					ui.show_hud_message(ui.L("Opponent left. The computer has taken over their forces."));
				} else {
					log("net: peer lost mid-game, no takeover (%d humans) -- ending\n", humans);
					save_recorded_replay();
					net.leave_game(server);
					SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "OpenBW",
						ui.L("The opponent disconnected or sync was lost.\nReturning to the menu."),
						nullptr);
					return 0;
				}
			}
			if (native_window::consume_low_memory()) {
				m.drop_saved_states_on_low_memory();
				m.ui.drop_hd_caches_on_low_memory();
				log("low memory: dropped seek states\n");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	} catch (const std::exception& e) {
		log("load failed: %s\n", e.what());
		// Neutral wording: this catch handles melee/campaign/replay alike, so it
		// no longer claims "replay" for a campaign or map failure.
		a_string msg = a_string("Nepavyko paleisti: ") + e.what() +
			"\n(Replay atveju dazna priezastis - zaidimas pries kompiuteri arba UMS zemelapis.)";
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenBW", msg.c_str(), nullptr);
		return 1;
	}

	return 0;
}

// --- Replay header parsing for the Java replay library UI ---
// Lives in this translation unit because the OpenBW headers define
// non-inline globals (korean.h tables etc.) and must only be included once.
// Parses only the identifier + 633-byte game info block and returns JSON;
// the actions/map sections are never read, so this is cheap enough to run
// for every file in the list. Field order must match replay.h load_replay().

namespace {

a_string json_escape(const a_string& s) {
	a_string r;
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			r += '\\';
			r += (char)c;
		} else if (c < 0x20) {
			// control characters are never meaningful in BW strings
		} else {
			r += (char)c;
		}
	}
	return r;
}

// BW strings are CP949 or latin-ish; JNI NewStringUTF requires valid UTF-8.
a_string to_utf8(const a_string& s) {
	a_string kn;
	if (korean::korean_locale_to_utf8(s, kn)) return kn;
	a_string r;
	for (unsigned char c : s) {
		if (c < 0x80) r += (char)c;
		else {
			// latin-1 -> utf-8 so the string stays valid
			r += (char)(0xc0 | (c >> 6));
			r += (char)(0x80 | (c & 0x3f));
		}
	}
	return r;
}

}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_ReplayInfo_parseNative(JNIEnv* env, jclass, jstring jpath) {
	const char* path_chars = env->GetStringUTFChars(jpath, nullptr);
	a_string path = path_chars ? path_chars : "";
	if (path_chars) env->ReleaseStringUTFChars(jpath, path_chars);

	try {
		auto file_r = data_loading::file_reader<>(path);
		auto r = data_loading::make_replay_file_reader(file_r);

		uint32_t identifier = r.get<uint32_t>();
		if (identifier != 0x53526572) error("not a replay file");

		std::array<uint8_t, 633> b;
		r.get_bytes(b.data(), b.size());
		data_loading::data_reader_le gir(b.data(), b.data() + b.size());

		// Field order mirrors replay.h load_replay().
		gir.get<uint8_t>(); // is broodwar
		uint32_t frame_count = gir.get<uint32_t>();
		gir.get<uint16_t>(); // campaign id
		gir.get<uint8_t>(); // command byte
		gir.get<uint32_t>(); // random seed
		gir.get<std::array<uint8_t, 8>>(); // player bytes
		gir.get<uint32_t>();
		gir.get<std::array<char, 24>>(); // host player name
		gir.get<uint32_t>(); // game flags
		uint16_t map_width = gir.get<uint16_t>();
		uint16_t map_height = gir.get<uint16_t>();
		gir.get<uint8_t>(); // active player count
		gir.get<uint8_t>(); // slot count
		uint8_t game_speed = gir.get<uint8_t>();
		gir.get<uint8_t>(); // game state
		uint16_t game_type = gir.get<uint16_t>();
		gir.get<uint16_t>(); // game sub type
		gir.get<uint32_t>();
		gir.get<uint16_t>(); // tileset
		gir.get<uint8_t>(); // replay autosaved
		gir.get<uint8_t>(); // computer player count
		auto game_name_arr = gir.get<std::array<char, 25>>();
		auto map_name_arr = gir.get<std::array<char, 32>>();
		gir.get<uint16_t>();
		gir.get<uint16_t>();
		gir.get<uint16_t>();
		gir.get<uint16_t>();
		gir.get<uint8_t>(); // victory condition
		gir.get<uint8_t>(); // resource type
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>();
		gir.get<uint8_t>(); // tournament mode
		gir.get<uint32_t>();
		gir.get<uint32_t>(); // starting minerals
		gir.get<uint32_t>(); // starting gas
		gir.get<uint8_t>();

		auto arr_str = [&](auto& str) {
			a_string s;
			for (auto& v : str) {
				if (!v) break;
				if ((unsigned char)v >= 21) s += v;
			}
			return s;
		};

		a_string game_name = to_utf8(arr_str(game_name_arr));
		a_string map_name = to_utf8(arr_str(map_name_arr));

		a_string players;
		bool has_computer = false;
		for (size_t i = 0; i != 12; ++i) {
			gir.get<uint32_t>(); // slot
			gir.get<uint32_t>(); // player id
			int controller = gir.get<uint8_t>();
			int race = gir.get<uint8_t>();
			gir.get<uint8_t>(); // force
			auto name_arr = gir.get<std::array<char, 25>>();
			a_string name = to_utf8(arr_str(name_arr));
			// controller: 1 = computer (in-game), 2 = human (in-game),
			// 5 = computer (lobby), 6 = open/human (lobby)
			bool computer = controller == 1 || controller == 5;
			if (computer) has_computer = true;
			bool active = (computer || controller == 2 || controller == 6) && !name.empty();
			if (!active) continue;
			if (!players.empty()) players += ",";
			players += a_string("{\"name\":\"") + json_escape(name) + "\",\"race\":" +
				std::to_string(race).c_str() + ",\"computer\":" + (computer ? "true" : "false") + "}";
		}

		a_string json = "{";
		json += a_string("\"frames\":") + std::to_string(frame_count).c_str();
		json += a_string(",\"gameName\":\"") + json_escape(game_name) + "\"";
		json += a_string(",\"mapName\":\"") + json_escape(map_name) + "\"";
		json += a_string(",\"mapWidth\":") + std::to_string(map_width).c_str();
		json += a_string(",\"mapHeight\":") + std::to_string(map_height).c_str();
		json += a_string(",\"gameSpeed\":") + std::to_string(game_speed).c_str();
		json += a_string(",\"gameType\":") + std::to_string(game_type).c_str();
		json += ",\"hasComputer\":";
		json += has_computer ? "true" : "false";
		json += a_string(",\"players\":[") + players + "]";
		json += "}";

		return env->NewStringUTF(json.c_str());
	} catch (const std::exception& e) {
		a_string json = a_string("{\"error\":\"") + json_escape(e.what()) + "\"}";
		return env->NewStringUTF(json.c_str());
	}
}







