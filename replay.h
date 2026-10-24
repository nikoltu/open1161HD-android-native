#ifndef BWGAME_REPLAY_H
#define BWGAME_REPLAY_H

#include "actions.h"
#include "data_loading.h"
#include "korean.h"
#include "bwgame.h"

namespace bwgame {

namespace data_loading {

struct crc32_t {
	std::array<uint32_t, 256> table;
	crc32_t() {
		for (uint32_t i = 0; i != 256; ++i) {
			uint32_t v = i;
			for (size_t b = 0; b != 8; ++b) {
				v = (v >> 1) ^ (v & 1 ? 0xedb88320 : 0);
			}
			table[i] = v;
		}
	}
	uint32_t operator()(const uint8_t* data, size_t data_size) {
		uint32_t r = 0xffffffff;
		const uint8_t* end = data + data_size;
		for (; data != end; ++data) {
			r = (r >> 8) ^ table[(r ^ *data) & 0xff];
		}
		return r;
	}
};

// SC:R ("seRS") replays compress their sections with zlib instead of the classic
// PKWARE-implode. The sim-core must not depend on a zlib implementation (the only
// one available lives under ui/, via stb_image), so -- exactly like ui/casc.h --
// the inflate is INJECTABLE: the host (aidump / android_main) wires it in before
// loading a Remastered replay. Classic ("reRS") replays never touch this path, so
// the melee-determinism gate is unaffected. Returns true on success. C++14-safe
// (function pointer + function-local static; no inline variables, no <functional>).
using scr_replay_inflate_fn = bool (*)(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
inline scr_replay_inflate_fn& scr_replay_inflate() {
	static scr_replay_inflate_fn f = nullptr;
	return f;
}

template<typename base_reader_T, bool default_little_endian = true>
struct replay_file_reader {
	crc32_t crc32;
	base_reader_T& r;
	bool scr = false;   // SC:R seRS container: sections are zlib, not PKWARE-imploded
	replay_file_reader(base_reader_T& r) : r(r) {
	}

	void get_bytes(uint8_t* output, size_t output_size) {
		uint32_t crc32_sum = r.template get<uint32_t>();
		size_t segments = r.template get<uint32_t>();
		
		a_vector<uint8_t> compressed_data;

		size_t output_pos = 0;
		for (size_t i = 0; i != segments; ++i) {
			size_t segment_input_size = r.template get<uint32_t>();
			
			size_t segment_output_size = output_size - output_pos;
			if (segment_output_size > 8192) segment_output_size = 8192;
			
			if (segment_input_size > segment_output_size) error("replay_file_reader: output buffer too small");
			if (segment_input_size == segment_output_size) {
				r.get_bytes(output + output_pos, segment_input_size);
			} else {
				compressed_data.resize(segment_input_size);
				r.get_bytes(compressed_data.data(), segment_input_size);
				if (scr) {
					// SC:R seRS: zlib chunk. decompress() (PKWARE-implode) does not apply.
					auto inflate = scr_replay_inflate();
					if (!inflate) error("replay_file_reader: SC:R (seRS) replay needs a zlib inflate provider (none wired)");
					if (!inflate(compressed_data.data(), segment_input_size, output + output_pos, segment_output_size))
						error("replay_file_reader: SC:R zlib inflate failed");
				} else {
					decompress(compressed_data.data(), segment_input_size, output + output_pos, segment_output_size);
				}
			}
			output_pos += segment_output_size;
		}
		
		if (output_pos != output_size) error("replay_file_reader: read %d bytes, expected %d", output_pos, output_size);
		
		uint32_t calculcated_crc32_sum = crc32(output, output_size);
		if (calculcated_crc32_sum != crc32_sum) error("replay_file_reader: crc32 mismatch: got %08x, expected %08x", calculcated_crc32_sum, crc32_sum);
	}

	template<typename T, bool little_endian = default_little_endian>
	T get() {
		return get_impl<T, little_endian>(*this);
	}
};

template<typename base_reader_T>
auto make_replay_file_reader(base_reader_T& reader) {
	return replay_file_reader<base_reader_T>(reader);
}

// Enter SC:R (seRS) mode: flag the container reader for zlib and skip the extra
// total-size u32 that Remastered replays place after the magic section. Overloaded
// so load_replay compiles for BOTH reader kinds it can be instantiated with -- the
// container reader (does the work) and a raw data_reader (no container; can never
// legitimately reach here, so a guarded error).
template<typename base_reader_T, bool le>
inline void replay_enter_scr(replay_file_reader<base_reader_T, le>& r) {
	r.scr = true;
	r.r.template get<uint32_t>();
}
template<typename reader_T>
inline void replay_enter_scr(reader_T&) {
	error("load_replay: SC:R (seRS) replay requires the container reader");
}

// v1.0.5: the object limits an SC:R game was played with. After the map section an
// SC:R replay carries tagged sections (SKIN, LMTS, BFIX, CCLR, GCFG): a 4-byte tag,
// the size of what follows, then one container block. LMTS holds seven numbers --
// images, sprites, thingies, units, bullets, orders, fog sprites (measured: 10000 /
// 5000 / 1000 / 3400 / 400 / 4000 / 1000 for an "extended limits" game, 5000 / 2500 /
// 500 / 1700 / 100 / 2000 / 500 for one played with the original limits).
struct scr_limits_t {
	bool present = false;
	uint32_t images = 5000, sprites = 2500, thingies = 500, units = 1700, bullets = 100, orders = 2000, fog_sprites = 500;
};
template<typename base_reader_T, bool le>
inline scr_limits_t replay_read_scr_limits(replay_file_reader<base_reader_T, le>& r) {
	scr_limits_t l;
	if (!r.scr) return l;
	// Is the rest of the file, from `pos`, a clean chain of tagged sections?
	auto chain_ok = [&](size_t pos) {
		const size_t end = r.r.size();
		bool any = false;
		while (pos + 8 <= end) {
			r.r.seek(pos);
			uint8_t t[4];
			r.r.get_bytes(t, 4);
			for (uint8_t c : t) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
			size_t size = r.r.template get<uint32_t>();
			if (pos + 8 + size > end) return false;
			pos += 8 + size;
			any = true;
		}
		return any && pos == end;
	};
	try {
		// Between the map and the tagged sections SC:R writes an untagged block (the
		// player-name table; 75 and 83 bytes on the replays measured). Step over up to
		// three such blocks until what remains is a clean chain of tags.
		size_t pos = r.r.tell();
		bool found = chain_ok(pos);
		for (int skipped = 0; !found && skipped != 3; ++skipped) {
			// one container block: checksum, segment count, then each segment's
			// length and bytes (its unpacked size is not stored, so it is stepped
			// over rather than decoded)
			r.r.seek(pos);
			r.r.template get<uint32_t>();
			size_t segments = r.r.template get<uint32_t>();
			if (segments == 0 || segments > 64) break;
			bool bad = false;
			for (size_t k = 0; k != segments; ++k) {
				size_t n = r.r.template get<uint32_t>();
				if (n > r.r.left()) { bad = true; break; }
				r.r.seek(r.r.tell() + n);
			}
			if (bad) break;
			pos = r.r.tell();
			found = chain_ok(pos);
		}
		if (!found) return l;
		r.r.seek(pos);
		while (r.r.left() >= 8) {
			uint32_t tag = r.r.template get<uint32_t>();
			size_t size = r.r.template get<uint32_t>();
			if (size > r.r.left()) break;
			if (tag == 0x53544D4Cu && size == 40) {   // "LMTS"
				std::array<uint8_t, 28> b;
				r.get_bytes(b.data(), b.size());
				auto v = [&](size_t i) { return (uint32_t)b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) | ((uint32_t)b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24); };
				l.images = v(0); l.sprites = v(1); l.thingies = v(2); l.units = v(3); l.bullets = v(4); l.orders = v(5); l.fog_sprites = v(6);
				l.present = true;
			} else {
				a_vector<uint8_t> skip(size);
				if (size) r.r.get_bytes(skip.data(), size);
			}
		}
	} catch (...) {
		// a truncated / unknown tail is not a reason to refuse the replay
		l.present = false;
	}
	return l;
}
template<typename reader_T>
inline scr_limits_t replay_read_scr_limits(reader_T&) { return {}; }

// open1161HD P5: read the optional house-rules trailer that replay_saver.h
// appends after the map section ("OBQL", version, qol bitmask -- raw bytes,
// outside the crc/segment container). Absent (every BW/SC:R replay, and any
// game recorded with all rules off) -> 0, and nothing else in the stream is
// consumed, so the determinism gate replays are byte-for-byte unaffected.
// Version 2 (v1.0.2) adds the game SET-UP the 1.16.1 header cannot carry: the
// aiscript each computer slot ran, and the alliances / shared vision the game
// started with (replay_saver.h has the layout).
struct replay_trailer_t {
	uint32_t version = 0;
	uint32_t qol_bits = 0;
	bool has_setup = false;
	std::array<uint32_t, 8> ai_script{};
	std::array<std::array<uint8_t, 12>, 12> alliances{};
	std::array<uint32_t, 12> shared_vision{};
};
template<typename base_reader_T, bool le>
inline replay_trailer_t replay_read_qol_trailer(replay_file_reader<base_reader_T, le>& r) {
	replay_trailer_t t;
	if (r.scr || r.r.left() < 12) return t;
	if (r.r.template get<uint32_t>() != 0x4C51424Fu) return t;
	t.version = r.r.template get<uint32_t>();
	uint32_t bits = r.r.template get<uint32_t>();
	if (t.version != 1 && t.version != 2) { t.version = 0; return t; }
	t.qol_bits = bits;
	if (t.version == 2 && r.r.left() >= 8 * 4 + 144 + 12 * 4) {
		for (size_t i = 0; i != 8; ++i) t.ai_script[i] = r.r.template get<uint32_t>();
		for (size_t i = 0; i != 12; ++i) for (size_t j = 0; j != 12; ++j) t.alliances[i][j] = r.r.template get<uint8_t>();
		for (size_t i = 0; i != 12; ++i) t.shared_vision[i] = r.r.template get<uint32_t>();
		t.has_setup = true;
	}
	return t;
}
template<typename reader_T>
inline replay_trailer_t replay_read_qol_trailer(reader_T&) { return {}; }

}

struct replay_state {
	a_vector<uint8_t> actions_data_buffer;
	int end_frame = 0;
	a_string map_name;
	std::array<a_string, 12> player_name;
	int game_type = 0;
	// Raw lobby header values, kept for the replay player and the harnesses (they do
	// not feed the simulation by themselves).
	int game_sub_type = 0;
	std::array<uint8_t, 12> header_force{};   // per slot, as stored (lobby force / team number)
	// victory condition, resource type, standard unit stats, fog, starting units,
	// fixed positions, restriction flags, allies enabled, teams enabled, cheats,
	// tournament mode
	std::array<uint8_t, 11> header_rules{};
	uint32_t qol_bits = 0;   // P5: SCBW2 house-rules restored from the trailer (0 = vanilla)
	data_loading::scr_limits_t scr_limits;   // v1.0.5: the SC:R game's object limits (LMTS), if the replay has them
	// v1.0.2: set-up restored from a version 2 trailer (own recordings)
	bool has_setup = false;
	std::array<uint32_t, 8> ai_script{};
};

struct replay_functions: action_functions {
	replay_state& replay_st;
	explicit replay_functions(state& st, action_state& action_st, replay_state& replay_st) : action_functions(st, action_st), replay_st(replay_st) {}
	
	void load_replay_file(a_string filename, bool initial_processing = true, std::vector<uint8_t>* get_map_data = nullptr) {
		auto file_r = data_loading::file_reader<>(std::move(filename));
		load_replay(data_loading::make_replay_file_reader(file_r), initial_processing, get_map_data);
	}
	void load_replay_data(const uint8_t* data, size_t data_size, bool initial_processing = true, std::vector<uint8_t>* get_map_data = nullptr) {
		auto r = data_loading::data_reader_le(data, data + data_size);
		load_replay(data_loading::make_replay_file_reader(r), initial_processing, get_map_data);
	}
	template<typename reader_T>
	void load_replay(reader_T&& r, bool initial_processing = true, std::vector<uint8_t>* get_map_data = nullptr) {
		
		uint32_t identifier = r.template get<uint32_t>();
		if (identifier == 0x53526573) {
			// "seRS" -- SC:R (Remastered) container: zlib chunks + one raw u32
			// (total decompressed size) right after the magic section, before the
			// game-info section. Enter zlib mode + skip that extra u32.
			data_loading::replay_enter_scr(r);
		} else if (identifier != 0x53526572) { // "reRS" -- classic 1.16.1
			error("load_replay: invalid identifier %#x", identifier);
		}
		// seRS replays reference units in their 0x60-0x65 commands via SC:R's wide
		// index|generation tag (decoded in actions.h get_unit_by_scr_tag). Classic
		// reRS leaves this off (never emits 0x60-0x65), so the gate is untouched.
		st.scr_tag_mode = (identifier == 0x53526573);

		std::array<uint8_t, 633> game_info_buffer;
		r.get_bytes(game_info_buffer.data(), game_info_buffer.size());
		
		data_loading::data_reader_le gir(game_info_buffer.data(), game_info_buffer.data() + game_info_buffer.size());
		
		gir.get<uint8_t>(); // is broodwar
		auto frame_count = gir.get<uint32_t>();
		gir.get<uint16_t>(); // campaign id
		gir.get<uint8_t>(); // command byte ?
		uint32_t random_seed = gir.get<uint32_t>();
		gir.get<std::array<uint8_t, 8>>(); // player bytes ?
		gir.get<uint32_t>(); // ?
		auto player_name = gir.get<std::array<char, 24>>();
		gir.get<uint32_t>(); // game flags?
		gir.get<uint16_t>(); // map width
		gir.get<uint16_t>(); // map height
		gir.get<uint8_t>(); // active player acount
		gir.get<uint8_t>(); // slot count
		gir.get<uint8_t>(); // game speed
		gir.get<uint8_t>(); // game state ?
		auto game_type = gir.get<uint16_t>(); // game type ?
		auto game_sub_type = gir.get<uint16_t>(); // game sub type ?
		gir.get<uint32_t>(); // ?
		gir.get<uint16_t>(); // tileset
		gir.get<uint8_t>(); // replay autosaved
		gir.get<uint8_t>(); // computer player count?
		auto game_name = gir.get<std::array<char, 25>>();
		auto map_name = gir.get<std::array<char, 32>>();
		gir.get<uint16_t>(); // game type ?
		gir.get<uint16_t>(); // game sub type ?
		gir.get<uint16_t>(); // sub type display ?
		gir.get<uint16_t>(); // sub type label ?
		auto victory_condition = gir.get<uint8_t>(); // victory condition
		int resource_type = gir.get<uint8_t>(); // resource type
		auto hdr_std_stats = gir.get<uint8_t>(); // use standard unit stats
		auto hdr_fog = gir.get<uint8_t>(); // fog of war enabled
		auto create_initial_units = gir.get<uint8_t>();
		auto hdr_fixed_pos = gir.get<uint8_t>(); // use fixed positions ?
		auto hdr_restrict = gir.get<uint8_t>(); // restriction flags ?
		auto hdr_allies = gir.get<uint8_t>(); // allies enabled
		auto hdr_teams = gir.get<uint8_t>(); // teams enabled
		auto hdr_cheats = gir.get<uint8_t>(); // cheats enabled
		auto tournament_mode = gir.get<uint8_t>(); // tournament mode ?
		gir.get<uint32_t>(); // victory condition value?
		auto starting_minerals = gir.get<uint32_t>(); // starting minerals
		gir.get<uint32_t>(); // starting gas
		gir.get<uint8_t>(); // ?
		
		(void)player_name;
		(void)game_name;
		
		auto arr_str = [&](auto& str) {
			a_string r;
			for (auto& v : str) {
				if (!v) break;
				if ((unsigned char)v >= 21) r += v;
			}
			return r;
		};
		replay_st.map_name = arr_str(map_name);
		a_string kn;
		if (korean::korean_locale_to_utf8(replay_st.map_name, kn)) replay_st.map_name = kn;
		
		std::array<int, 12> slot_player_id;
		std::array<int, 12> slot_controller;
		std::array<int, 12> slot_race;
		std::array<int, 12> slot_force;
		
		for (size_t i = 0; i != 12; ++i) {
			gir.get<uint32_t>(); // slot ?
			slot_player_id[i] = gir.get<uint32_t>(); // player id
			slot_controller[i] = gir.get<uint8_t>(); // controller
			slot_race[i] = gir.get<uint8_t>(); // race
			slot_force[i] = gir.get<uint8_t>(); // force
			auto name = gir.get<std::array<char, 25>>(); // player name
			replay_st.player_name[i] = arr_str(name);
			
			action_st.player_id[i] = slot_player_id[i];
		}
		
		auto player_color = gir.get<std::array<uint32_t, 8>>(); // player colors
		auto create_melee_units_for_player = gir.get<std::array<uint8_t, 8>>();
		
		replay_st.end_frame = frame_count;
		replay_st.game_type = game_type;
		replay_st.game_sub_type = game_sub_type;
		for (size_t i = 0; i != 12; ++i) replay_st.header_force[i] = (uint8_t)slot_force[i];
		replay_st.header_rules = {{(uint8_t)victory_condition, (uint8_t)resource_type, hdr_std_stats, hdr_fog, create_initial_units,
			hdr_fixed_pos, hdr_restrict, hdr_allies, hdr_teams, hdr_cheats, tournament_mode}};
		
		replay_st.actions_data_buffer.resize(r.template get<uint32_t>());
		// v1.0.19: an SC:R replay with no commands at all (a game the player only watched
		// -- measured: a campaign mission run as a custom game, idle) has NO command
		// section after the zero size; the map size follows directly. Our own recorder
		// writes an empty section there, so the classic path is unchanged.
		if (!(st.scr_tag_mode && replay_st.actions_data_buffer.empty()))
			r.get_bytes(replay_st.actions_data_buffer.data(), replay_st.actions_data_buffer.size());
		
		a_vector<uint8_t> map_buffer;
		map_buffer.resize(r.template get<uint32_t>());
		r.get_bytes(map_buffer.data(), map_buffer.size());

		if (get_map_data) *get_map_data = map_buffer;

		// P5: house-rules trailer (after the map section). Only the rules that change
		// how the SIM interprets a recorded action are restored -- the UI-fan rules
		// (group rally, MBS train, wide selection, smart-cast) already baked their
		// effect into the action stream, and auto_worker's injector never runs
		// during playback. Must happen inside the load callback: the map load
		// resets game_state (and with it qol).
		const data_loading::scr_limits_t scr_limits = data_loading::replay_read_scr_limits(r);
		replay_st.scr_limits = scr_limits;
		const data_loading::replay_trailer_t trailer = data_loading::replay_read_qol_trailer(r);
		const uint32_t qol_bits = trailer.qol_bits;
		replay_st.qol_bits = qol_bits;
		replay_st.has_setup = trailer.has_setup;
		replay_st.ai_script = trailer.ai_script;
		// An OpenBW recording made before the version 2 trailer stored the engine's
		// own 0-based force in the header (see the use-map-settings note below).
		const bool own_old_recording = !trailer.has_setup && arr_str(game_name) == "OpenBW Android";

		game_load_functions game_load_funcs(st);
		game_load_funcs.load_map_data(map_buffer.data(), map_buffer.size(), [&]() {
			st.game->qol.multi_building_select = (qol_bits >> 0) & 1;
			st.game->qol.worker_rally_split    = (qol_bits >> 6) & 1;
			st.game->qol.smart_rally           = (qol_bits >> 7) & 1;
			// An SC:R game with larger pools: the two thresholds move with them (the
			// original keeps 20 bullets and 200 orders in reserve). Never below 1.16.1's.
			if (scr_limits.present) {
				if (scr_limits.bullets > 100) st.game->halo_rocket_bullet_cap = (int)scr_limits.bullets - 20;
				if (scr_limits.orders > 2000) st.game->order_queue_cap = (int)scr_limits.orders - 200;
				// v1.0.15: and the pools themselves grow. With 1.16.1's 100 bullets a late
				// fight ran out of them where the recorded game (400) did not: a worker's
				// mining spark was not created, its sound did not draw a random number, and
				// everything after that frame differed. Units keep their pool (tags map to it);
				// sprites and images too -- no replay reaches those limits.
				if (scr_limits.bullets > st.bullets_container.capacity && st.bullets_container.size == 0) st.bullets_container.set_capacity(scr_limits.bullets);
				if (scr_limits.orders > st.orders_container.capacity && st.orders_container.size == 0) st.orders_container.set_capacity(scr_limits.orders);
			}
			game_load_funcs.setup_info.victory_condition = victory_condition;
			game_load_funcs.setup_info.starting_units = create_initial_units;
			game_load_funcs.setup_info.tournament_mode = tournament_mode;
			game_load_funcs.setup_info.resource_type = resource_type;
			game_load_funcs.setup_info.starting_minerals = starting_minerals;
			for (size_t i = 0; i != 12; ++i) {
				st.players[i].controller = slot_controller[i];
				// A computer slot of an own recording was an OCCUPIED slot while the map
				// loaded and became a computer right after (when its aiscript was
				// started); the load has to see the same thing.
				if (trailer.has_setup && i < 8 && trailer.ai_script[i] && slot_controller[i] == player_t::controller_computer_game)
					st.players[i].controller = player_t::controller_occupied;
				st.players[i].race = (race_t)slot_race[i];
				// The header stores the lobby's force number: 1-4 in a use-map-settings
				// game (0 = none), while the engine indexes forces the way the map's
				// FORC chunk does, 0-3 (trigger groups 18-21, force flags). Taking the
				// header value as is shifted every UMS player one force up: the
				// players of an observer map got the observers' triggers and never
				// the "set resources to 50" one, so the first worker could not be
				// trained and the whole playback diverged from frame 16 on
				// (measured: ZerovsMind sel_null 7026, bisu_hiya 6789, r3_funny 12485).
				// Melee-type games keep the header value: forces mean nothing there.
				if (victory_condition == 0 && tournament_mode == 0 && create_initial_units == 0 && own_old_recording) {
					st.players[i].force = slot_force[i];
				} else if (victory_condition == 0 && tournament_mode == 0 && create_initial_units == 0) {
					if (slot_force[i] >= 1 && slot_force[i] <= 4) st.players[i].force = slot_force[i] - 1;
					// (0: keep what the map's FORC chunk assigned)
				} else {
					st.players[i].force = slot_force[i];
				}
				if (victory_condition == 0 && tournament_mode == 0) {
					if (i >= 8) game_load_funcs.setup_info.create_melee_units_for_player[i] = false;
					else game_load_funcs.setup_info.create_melee_units_for_player[i] = create_melee_units_for_player[i] != 0;
				}
			}
			// "Top vs Bottom" (game type 15): the two teams are the two lobby force
			// numbers in the header. Team-mates are allies (allied victory) and see
			// what each other sees. Without this, team-mates' armies fought each other
			// from their first meeting and the playback diverged from there (measured:
			// rep2v2_fs selection misses 1651 -> 6, rep2v2_python 619 -> 2).
			if (game_type == 15) {
				auto in_game = [&](size_t i) {
					return slot_controller[i] == player_t::controller_occupied || slot_controller[i] == player_t::controller_computer
						|| slot_controller[i] == player_t::controller_computer_game;
				};
				for (size_t i = 0; i != 8; ++i) {
					if (!in_game(i)) continue;
					for (size_t j = 0; j != 8; ++j) {
						if (i == j || !in_game(j) || slot_force[i] != slot_force[j]) continue;
						st.alliances[i][j] = 2;
						st.shared_vision[i] |= 1 << j;
					}
				}
			}
			st.lcg_rand_state = random_seed;
		}, initial_processing);

		// v1.0.2: an own recording says what was set up after the map load, in the
		// order the live game did it -- teams first, then the computer players.
		if (trailer.has_setup) {
			for (size_t i = 0; i != 12; ++i) {
				for (size_t j = 0; j != 12; ++j) st.alliances[i][j] = trailer.alliances[i][j];
				st.shared_vision[i] = trailer.shared_vision[i];
			}
			for (size_t i = 0; i != 8; ++i) {
				if (!trailer.ai_script[i]) continue;
				st.players[i].controller = player_t::controller_computer_game;
				ai_start_script((int)i, trailer.ai_script[i]);
			}
		}
		
		std::array<int, 8> source_colors;
		for (size_t i = 0; i != 8; ++i) {
			source_colors[i] = st.players[i].color;
		}
		for (size_t i = 0; i != 8; ++i) {
			// v1.0.22: 0..7 is the classic permutation of the slots' own colours. Our
			// recorder also writes the extra rows of the colour table (8..15, picked in the
			// launcher) -- those are the colour itself. Colour is drawing only.
			const uint32_t pc = player_color[i];
			if (pc < 8) st.players[i].color = source_colors[pc];
			else if (pc < 16) st.players[i].color = (int)pc;
			else st.players[i].color = source_colors[i];
		}
	}
	
	void next_frame() {
		if (st.current_frame == replay_st.end_frame) error("replay: attempt to play past end");
		execute_actions(replay_st.actions_data_buffer.data(), replay_st.actions_data_buffer.data() + replay_st.actions_data_buffer.size());
		state_functions::next_frame();
	}
	
	bool is_done() {
		return st.current_frame == replay_st.end_frame;
	}
	
};

struct replay_player: game_player {
	action_state action_st;
	replay_state replay_st;
	optional<replay_functions> opt_funcs;
	replay_player() = default;
	replay_player(const game_player& n) {
		set_st(n.st());
	}
	
	void load_replay_file(a_string filename, bool initial_processing = true) {
		auto file_r = data_loading::file_reader<>(std::move(filename));
		load_replay(data_loading::make_replay_file_reader(file_r), initial_processing);
	}
	void load_replay_data(uint8_t* data, size_t data_size, bool initial_processing = true) {
		load_replay(data_loading::data_reader_le(data, data + data_size), initial_processing);
	}
	void lazy_init() {
		if (!opt_funcs) opt_funcs.emplace(st(), action_st, replay_st);
	}

	template<typename reader_T>
	void load_replay(reader_T&& r, bool initial_processing = true) {
		lazy_init();
		opt_funcs->load_replay(r, initial_processing);
	}
	
	void next_frame() {
		lazy_init();
		opt_funcs->next_frame();
	}
	
	bool is_done() {
		lazy_init();
		return opt_funcs->is_done();
	}
	
};

}

#endif
