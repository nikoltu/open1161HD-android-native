// Disk save/load (v0.83, G9).
//
// Both halves of `state` are here now:
//   - the VALUE half  (state_base_copyable)      -- visit_copyable()
//   - the OBJECT half (units/sprites/images/...)  -- visit_objects()
// and both directions share one definition each.
//
// Verified behaviourally by aidump savetest (the only test that catches a
// forgotten field -- see the note at visit_objects' object_io): save a running
// game, reload the map from scratch, apply the save, then step the restored
// state beside a reference for K frames and require identical lcg_rand_state.
// Confirmed on terran01/03/05, zerg01, protoss01 -- PASS for 200-400 frames,
// all three races.
//
// Still not written (deliberately): action_state (input/selection, not read by
// the deterministic sim), and a versioned file header (magic + format_version
// + engine versionCode + map id) so an incompatible save is rejected rather
// than half-applied. Neither is needed for the round-trip to be correct; both
// are needed before a save is written to disk for real.
//
// Shape of the problem, measured rather than assumed:
//
//   sizeof(state)        =  80 768 B   (the fixed part)
//   sizeof(action_state) =   3 456 B
//   sizeof(game_state)   =  10 408 B   -- derived from the map, NOT saved
//   object containers    = ~3 MB worst case, far less in practice
//
// `state_base_copyable` holds ~65 members, 45 of them fixed-size arrays, five
// dynamic containers, and exactly TWO pointers (`global`, `game`) -- both to
// shared or map-derived data that is rebuilt on load. So the value half needs
// no pointer remapping at all; only the object half does.
//
// The object half is a mechanical twin of `state_copier` (bwgame.h): wherever
// the copier writes `remap_unit(x)` this writes the object's stable `index`,
// and wherever it walks an intrusive list this writes that list as a sequence
// of indices.
//
// TWO DIRECTIONS, ONE DEFINITION. Every field is named exactly once, in
// visit_copyable(), and the visitor is either a writer or a reader. A field
// cannot be saved but not loaded, because there is only one list. What a
// single list cannot prevent is a field being *forgotten entirely* -- that is
// what savecheck() below is for.
#ifndef BWGAME_SAVEGAME_H
#define BWGAME_SAVEGAME_H

#include "bwgame.h"
#include "actions.h"

namespace bwgame {
namespace savegame {

static const uint32_t magic = 0x57424f21;   // "!OBW"
static const uint32_t format_version = 1;

// --- visitors --------------------------------------------------------

struct writer_t {
	a_vector<uint8_t> out;
	static const bool reading = false;
	void bytes(const void* p, size_t n) {
		auto* b = (const uint8_t*)p;
		out.insert(out.end(), b, b + n);
	}
	void u32(uint32_t v) { bytes(&v, 4); }
	uint32_t io_u32(uint32_t w) { u32(w); return w; }   // direction-safe: emit
	size_t count(size_t n) { u32((uint32_t)n); return n; }
};

struct reader_t {
	const uint8_t* p;
	const uint8_t* e;
	bool failed = false;
	static const bool reading = true;
	void bytes(void* d, size_t n) {
		if ((size_t)(e - p) < n) { failed = true; return; }
		memcpy(d, p, n);
		p += n;
	}
	uint32_t u32() { uint32_t v = 0; bytes(&v, 4); return v; }
	uint32_t io_u32(uint32_t) { return u32(); }         // direction-safe: read
	size_t count(size_t&) { return u32(); }
};

// A trivially copyable field goes out as its own bytes. std::array and
// type_indexed_array land here too -- both are aggregates over a std::array,
// so they are trivially copyable and have no padding worth worrying about.
template<typename V, typename T>
typename std::enable_if<std::is_trivially_copyable<T>::value>::type
field(V& v, T& x) {
	if (V::reading) v.bytes(&x, sizeof(x));
	else v.bytes(&x, sizeof(x));
}

// The dynamic containers need genuinely different code in each direction, and
// C++14 has no `if constexpr` -- both branches of an `if` would still have to
// compile for both visitors. So these are explicit writer/reader overload
// pairs. Verbose, but it type-checks each direction on its own terms.

// a_vector<T> of trivially copyable T: length, then the elements.
template<typename T>
void field(writer_t& v, a_vector<T>& x) {
	v.u32((uint32_t)x.size());
	if (!x.empty()) v.bytes(x.data(), x.size() * sizeof(T));
}
template<typename T>
void field(reader_t& v, a_vector<T>& x) {
	size_t n = v.u32();
	x.resize(n);
	if (n) v.bytes(x.data(), n * sizeof(T));
}

template<typename K, typename T>
void field(writer_t& v, a_map<K, T>& x) {
	v.u32((uint32_t)x.size());
	for (auto& kv : x) {
		K k = kv.first;
		v.bytes(&k, sizeof(k));
		v.bytes(&kv.second, sizeof(kv.second));
	}
}
template<typename K, typename T>
void field(reader_t& v, a_map<K, T>& x) {
	size_t n = v.u32();
	x.clear();
	for (size_t i = 0; i != n; ++i) {
		K k{}; T t{};
		v.bytes(&k, sizeof(k));
		v.bytes(&t, sizeof(t));
		x.emplace(k, t);
	}
}

template<typename T, size_t N>
void field(writer_t& v, static_vector<T, N>& x) {
	v.u32((uint32_t)x.size());
	for (auto& t : x) v.bytes(&t, sizeof(t));
}
template<typename T, size_t N>
void field(reader_t& v, static_vector<T, N>& x) {
	size_t n = v.u32();
	x.clear();
	for (size_t i = 0; i != n && i != N; ++i) {
		T t{};
		v.bytes(&t, sizeof(t));
		x.push_back(t);
	}
}

// Any std::array whose element is NOT trivially copyable (array of a_vector,
// array of static_vector, ...) recurses element-wise. Trivially copyable
// arrays are caught by the byte overload above.
template<typename V, typename T, size_t N>
typename std::enable_if<!std::is_trivially_copyable<std::array<T, N>>::value>::type
field(V& v, std::array<T, N>& x) {
	for (auto& e : x) field(v, e);
}

// creep_life_t is a pointer-linked structure: 1024 entries in a fixed
// container, plus nine lists, a free list and a 512-bucket hash table, all
// intrusive. It is serialised the same way creep_life_t::operator= copies it
// -- entries by value, every list as a sequence of container indices. That
// operator is working, reviewed code; following it is the whole reason this
// does not need inventing.
template<typename list_T>
void creep_list(writer_t& v, list_T& l, creep_life_t& c) {
	a_vector<uint32_t> idx;
	for (auto& e : l) idx.push_back((uint32_t)(&e - c.entry_container.data()));
	v.u32((uint32_t)idx.size());
	for (auto i : idx) v.u32(i);
}
template<typename list_T>
void creep_list(reader_t& v, list_T& l, creep_life_t& c) {
	size_t n = v.u32();
	l.clear();
	for (size_t i = 0; i != n; ++i) {
		uint32_t idx = v.u32();
		if (idx < c.entry_container.size()) l.push_back(c.entry_container[idx]);
	}
}

template<typename V>
void field(V& v, creep_life_t& c) {
	field(v, c.recede_timer);
	field(v, c.check_dead_unit_timer);
	for (auto& e : c.entry_container) {
		field(v, e.tile_pos);
		field(v, e.n_neighboring_creep_tiles);
	}
	for (auto& l : c.lists) creep_list(v, l, c);
	field(v, c.lists_size);
	creep_list(v, c.free_list, c);
	field(v, c.free_list_size);
	for (auto& b : c.table.buckets) creep_list(v, b, c);
}

// --- the value half --------------------------------------------------
//
// The order here is the declaration order of state_base_copyable. Keeping the
// two in step is the whole discipline of this file; savecheck() is what proves
// they are.
template<typename V>
void visit_copyable(state& st, V& v) {
	// `global` and `game` are deliberately absent: both point at data the
	// loader rebuilds (global_state from the MPQs, game_state from the map).
	field(v, st.update_tiles_countdown);
	field(v, st.order_timer_counter);
	field(v, st.secondary_order_timer_counter);
	field(v, st.current_frame);
	field(v, st.players);
	field(v, st.alliances);
	field(v, st.upgrade_levels);
	field(v, st.upgrade_upgrading);
	field(v, st.tech_researched);
	field(v, st.tech_researching);
	field(v, st.unit_counts);
	field(v, st.completed_unit_counts);
	field(v, st.factory_counts);
	field(v, st.building_counts);
	field(v, st.non_building_counts);
	field(v, st.completed_factory_counts);
	field(v, st.completed_building_counts);
	field(v, st.completed_non_building_counts);
	field(v, st.total_buildings_ever_completed);
	field(v, st.total_non_buildings_ever_completed);
	field(v, st.unit_score);
	field(v, st.building_score);
	field(v, st.supply_used);
	field(v, st.supply_available);
	field(v, st.shared_vision);
	field(v, st.tiles);
	field(v, st.tiles_mega_tile_index);
	field(v, st.random_counts);
	field(v, st.total_random_counts);
	field(v, st.lcg_rand_state);
	field(v, st.last_error);
	field(v, st.last_error_player);
	field(v, st.last_error_frame);
	field(v, st.trigger_timer);
	field(v, st.running_triggers);
	field(v, st.trigger_wait_timers);
	field(v, st.trigger_waiting);
	field(v, st.active_orders_size);
	field(v, st.active_bullets_size);
	field(v, st.active_thingies_size);
	field(v, st.repulse_field);
	field(v, st.prev_bullet_heading_offset_clockwise);
	field(v, st.current_minerals);
	field(v, st.current_gas);
	field(v, st.total_minerals_gathered);
	field(v, st.total_gas_gathered);
	field(v, st.recent_lurker_hits);
	field(v, st.recent_lurker_hit_current_index);
	field(v, st.creep_life);
	field(v, st.update_psionic_matrix);
	field(v, st.disruption_webbed_units);
	field(v, st.cheats_enabled);
	field(v, st.cheat_operation_cwal);
	field(v, st.locations);
	field(v, st.trigger_switches);
	field(v, st.unit_deaths);
	field(v, st.unit_kills);
	field(v, st.eud_deaths);
	field(v, st.countdown_timer_ms);
	field(v, st.countdown_paused);
	field(v, st.custom_score);
	field(v, st.kills_score);
	field(v, st.razings_score);
	field(v, st.trigger_speech_muted);
	field(v, st.mission_objectives_string);
	field(v, st.leaderboard);
	field(v, st.next_scenario_string);
	for (auto& ai : st.ai_players) {
		field(v, ai.active);
		field(v, ai.bw);
		field(v, ai.town_pos);
		field(v, ai.threads);
		field(v, ai.build_goals);
		field(v, ai.attack_group);
		field(v, ai.upgrade_goals);
		field(v, ai.tech_goals);
		field(v, ai.max_counts);
		field(v, ai.max_force);
		field(v, ai.defense_units);
		field(v, ai.campaign);
		field(v, ai.garrison);
		field(v, ai.wave);
		field(v, ai.wave_gather);
		field(v, ai.wave_target);
		field(v, ai.wave_deadline);
		field(v, ai.wave_order);
		field(v, ai.wave_order_deadline);
		field(v, ai.garrison_bunkers);
		field(v, ai.attack_targets);
		field(v, ai.attack_target_rr);
		field(v, ai.last_attack_frame);
	}
}

// --- the object half -------------------------------------------------
//
// v0.83.012: units / sprites / images / bullets / orders and their intrusive
// lists, plus the heap-allocated paths and thingies.
//
// Structured as the exact inverse of state_copier (bwgame.h). The copier
// memcpy's each object and then overwrites the ~40 pointer fields; this does
// the same, but a pointer is written as a stable index (for objects) or a type
// id (for .dat pointers) and read back through the containers / get_*_type.
// Copying the whole object as bytes means a plain data field cannot be
// forgotten -- only a pointer left un-re-resolved can, and those are exactly
// the sites the copier already enumerates, transcribed here.
//
// Intrusive links (link, player_units_link, order_queue, gather_queue, the
// carrier/reaver lists, ...) are NOT trusted from the bytes: like the copier,
// every list is rebuilt from an index sequence, so the garbage link values a
// raw copy carries are never traversed.
//
// The behavioural harness (aidump savetest) is what proves this: a forgotten
// pointer changes the simulation, which changes lcg_rand_state within a few
// hundred frames. There is no structural test that can.

template<typename V>
struct object_io {
	state& st;
	state_functions funcs;    // get_*_type, unit_is_* -- same helpers the copier uses
	V& v;
	// Heap objects are addressed by their position in st.paths / st.thingies.
	// Save fills the ptr->index maps up front; load fills the index->ptr vectors.
	a_unordered_map<const path_t*, uint32_t> path_index;
	a_unordered_map<const thingy_t*, uint32_t> thingy_index;
	a_vector<path_t*> path_by_index;
	a_vector<thingy_t*> thingy_by_index;

	object_io(state& st, V& v) : st(st), funcs(st), v(v) {}

	template<typename T> void raw(T& x) { v.bytes(&x, sizeof(x)); }
	// Direction-safe u32 via the visitor's io_u32: the reader ignores the
	// argument and returns the read value; the writer emits it.
	uint32_t u32(uint32_t w) { return v.io_u32(w); }

	// ---- object references: 0 = null, else external index + 1 ----------
	template<typename Cont, typename Ptr>
	void obj_ref(Cont& cont, Ptr& p) {
		if (V::reading) { uint32_t i = u32(0); p = i ? cont.get(i - 1) : nullptr; }
		else u32(p ? (uint32_t)p->index + 1 : 0);
	}
	void uref(unit_t*& p) { obj_ref(st.units_container, p); }
	void uref(const unit_t*& p) {
		if (V::reading) { uint32_t i = u32(0); p = i ? st.units_container.get(i - 1) : nullptr; }
		else u32(p ? (uint32_t)p->index + 1 : 0);
	}
	void sref(sprite_t*& p) { obj_ref(st.sprites_container, p); }
	void iref(image_t*& p) { obj_ref(st.images_container, p); }
	void bref(bullet_t*& p) { obj_ref(st.bullets_container, p); }
	void oref(order_t*& p) { obj_ref(st.orders_container, p); }

	void pref(path_t*& p) {
		if (V::reading) { uint32_t i = u32(0); p = i ? path_by_index[i - 1] : nullptr; }
		else u32(p ? path_index.at(p) + 1 : 0);
	}
	void tref(thingy_t*& p) {
		if (V::reading) { uint32_t i = u32(0); p = i ? thingy_by_index[i - 1] : nullptr; }
		else u32(p ? thingy_index.at(p) + 1 : 0);
	}

	// ---- .dat type references: 0xFFFFFFFF = null, else the type id ------
	template<typename T, typename GetById>
	void type_ref(const T*& p, GetById get_by_id) {
		if (V::reading) { uint32_t id = u32(0); p = id == 0xFFFFFFFFu ? nullptr : get_by_id(id); }
		else u32(p ? (uint32_t)(size_t)p->id : 0xFFFFFFFFu);
	}
	void ut_ref(const unit_type_t*& p)   { type_ref(p, [&](uint32_t id){ return funcs.get_unit_type((UnitTypes)id); }); }
	void ord_ref(const order_type_t*& p) { type_ref(p, [&](uint32_t id){ return funcs.get_order_type((Orders)id); }); }
	void it_ref(const image_type_t*& p)  { type_ref(p, [&](uint32_t id){ return funcs.get_image_type((ImageTypes)id); }); }
	void spt_ref(const sprite_type_t*& p){ type_ref(p, [&](uint32_t id){ return funcs.get_sprite_type((SpriteTypes)id); }); }
	void ft_ref(const flingy_type_t*& p) { type_ref(p, [&](uint32_t id){ return funcs.get_flingy_type((FlingyTypes)id); }); }
	void wt_ref(const weapon_type_t*& p) { type_ref(p, [&](uint32_t id){ return funcs.get_weapon_type((WeaponTypes)id); }); }
	void tt_ref(const tech_type_t*& p)   { type_ref(p, [&](uint32_t id){ return funcs.get_tech_type((TechTypes)id); }); }
	void upt_ref(const upgrade_type_t*& p){ type_ref(p, [&](uint32_t id){ return funcs.get_upgrade_type((UpgradeTypes)id); }); }

	// A static_vector of unit-type pointers (build_queue): count then ids. The
	// vector must be REBUILT on read, never iterated in its raw-copied state --
	// see the note at the call site.
	template<size_t N>
	void type_vec(static_vector<const unit_type_t*, N>& q) {
		if (V::reading) {
			uint32_t n = u32(0);
			q.clear();
			for (uint32_t i = 0; i != n; ++i) { const unit_type_t* t = nullptr; ut_ref(t); q.push_back(t); }
		} else {
			u32((uint32_t)q.size());
			for (auto* t : q) { const unit_type_t* tt = t; ut_ref(tt); }
		}
	}

	// ---- target_t (pos + unit) and order_target_t (+ unit_type) ---------
	void target(target_t& t) { uref(t.unit); }               // t.pos rides in raw bytes
	void order_target(order_target_t& t) { uref(t.unit); ut_ref(t.unit_type); }

	// ---- iscript current_script: a pointer into st.global->iscript.scripts ------
	void script_ref(const iscript_t::script*& p) {
		if (V::reading) {
			int id = (int)u32(0);
			auto it = st.global->iscript.scripts.find(id);
			p = it != st.global->iscript.scripts.end() ? &it->second : nullptr;
		} else u32((uint32_t)(p ? p->id : -1));
	}

	// ---- intrusive lists rebuilt from index sequences -------------------
	template<typename List, typename Cont>
	void obj_list(List& l, Cont& cont) {
		if (V::reading) {
			uint32_t n = u32(0);
			l.clear();
			for (uint32_t i = 0; i != n; ++i) l.push_back(*cont.get(u32(0)));
		} else {
			uint32_t n = 0; for (auto& e : l) { (void)e; ++n; } u32(n);
			for (auto& e : l) u32((uint32_t)e.index);
		}
	}
	template<typename List> void unit_list(List& l)  { obj_list(l, st.units_container); }
	template<typename List> void image_list(List& l) { obj_list(l, st.images_container); }
	template<typename List> void order_list(List& l) { obj_list(l, st.orders_container); }

	// ---- circular vectors -----------------------------------------------
	void xy_ring(a_circular_vector<xy>& r) {
		if (V::reading) { uint32_t n = u32(0); r.clear(); for (uint32_t i = 0; i != n; ++i) { xy p; raw(p); r.push_back(p); } }
		else { u32((uint32_t)r.size()); for (auto& p : r) raw(p); }
	}
	void region_ring(a_circular_vector<const regions_t::region*>& r) {
		auto& regs = st.game->regions.regions;
		if (V::reading) { uint32_t n = u32(0); r.clear(); for (uint32_t i = 0; i != n; ++i) { uint32_t idx = u32(0); r.push_back(&regs[idx]); } }
		else { u32((uint32_t)r.size()); for (auto* rg : r) u32((uint32_t)rg->index); }
	}

	// ---- per-type payloads --------------------------------------------
	//
	// Split into DATA and LISTS. raw() overwrites an object's intrusive-link
	// bytes, so it MUST run before that object is pushed into any list. All
	// objects go through their _data pass first (run() phase C/D); only then
	// are intrusive lists rebuilt (phase E/F). Getting this order wrong
	// clobbers, e.g., an order's link right after it was linked into a unit's
	// order_queue -- which round-trips byte-perfectly yet produces a state
	// that loops when stepped.
	//
	// image / bullet / order / thingy own no intrusive object lists, so their
	// _data pass is the whole object. Only unit and sprite have a _lists pass.

	void image(image_t& im) {
		raw(im);
		it_ref(im.image_type);
		script_ref(im.iscript_state.current_script);
		sref(im.sprite);
		// grp is derived from image_type (bwgame.h:15495), so it is not saved as
		// data -- only whether it was set, then recomputed the engine's way.
		uint8_t has_grp = im.grp ? 1 : 0;
		raw(has_grp);
		if (V::reading)
			im.grp = (has_grp && im.image_type) ? st.global->image_grp[(size_t)im.image_type->id] : nullptr;
	}
	void sprite_data(sprite_t& sp) {
		raw(sp);
		spt_ref(sp.sprite_type);
		iref(sp.main_image);
	}
	void sprite_lists(sprite_t& sp) { image_list(sp.images); }
	void order(order_t& o) {
		raw(o);
		ord_ref(o.order_type);
		order_target(o.target);
	}
	void bullet(bullet_t& b) {
		raw(b);
		sref(b.sprite);              // thingy base
		ft_ref(b.flingy_type);       // flingy base
		target(b.move_target);       // flingy base
		uref(b.bullet_target);
		wt_ref(b.weapon_type);
		uref(b.bullet_owner_unit);
		uref(b.prev_bounce_unit);
	}
	void unit_data(unit_t& u) {
		raw(u);
		// thingy + flingy base
		sref(u.sprite);
		ft_ref(u.flingy_type);
		target(u.move_target);
		// unit body -- transcribed from state_copier::unit(), lists excepted
		ord_ref(u.order_type);
		ut_ref(u.order_unit_type);
		target(u.order_target);
		ut_ref(u.unit_type);
		uref(u.subunit);
		uref(u.auto_target_unit);
		uref(u.connected_unit);
		ut_ref(u.previous_unit_type);
		// build queues: a static_vector uses an end-POINTER, so raw(u) copied a
		// pointer into the WRITE object's buffer -- on the read side size()
		// would be garbage and the loop would run wild. Serialise count +
		// elements and rebuild instead of trusting the raw copy.
		type_vec(u.build_queue);
		type_vec(u.build_queue_limbo);
		ord_ref(u.secondary_order_type);
		// union single-pointer members (the LIST members are in unit_lists)
		if (u.unit_type) {
			if (funcs.unit_is(&u, UnitTypes::Protoss_Interceptor) || funcs.unit_is(&u, UnitTypes::Protoss_Scarab)) {
				uref(u.fighter.parent);
			} else if (funcs.unit_is_ghost(&u)) {
				tref(u.ghost.nuke_dot);
			}
		}
		uref(u.worker.powerup);
		uref(u.worker.target_resource_unit);
		uref(u.worker.gather_target);
		uref(u.building.addon);
		ut_ref(u.building.addon_build_type);
		tt_ref(u.building.researching_type);
		upt_ref(u.building.upgrading_type);
		target(u.building.rally);
		if (u.unit_type) {
			if (funcs.unit_is_nydus(&u)) {
				uref(u.building.nydus.exit);
			} else if (funcs.unit_is(&u, UnitTypes::Terran_Nuclear_Silo)) {
				uref(u.building.silo.nuke);
			} else if (funcs.unit_is(&u, UnitTypes::Protoss_Pylon)) {
				sref(u.building.pylon.psi_field_sprite);
			}
		}
		uref(u.current_build_unit);
		pref(u.path);
		uref(u.irradiated_by);
	}
	void unit_lists(unit_t& u) {
		order_list(u.order_queue);
		if (u.unit_type) {
			if (funcs.unit_is_carrier(&u)) {
				unit_list(u.carrier.inside_units);
				unit_list(u.carrier.outside_units);
			} else if (funcs.unit_is_reaver(&u)) {
				unit_list(u.reaver.inside_units);
				unit_list(u.reaver.outside_units);
			}
			if (funcs.ut_resource(&u)) unit_list(u.building.resource.gather_queue);
		}
	}
	void path(path_t& p) {
		// path_t is the ONE object that cannot be raw-copied: long_path and
		// short_path are a_circular_vectors that OWN heap. Raw-overwriting them
		// clobbers the buffer pointer, and the destructor then frees garbage
		// (seen as a Scudo "corrupted chunk header" at program teardown). So
		// every field is handled individually; the two rings go through their
		// own helpers, everything else is trivially copyable.
		raw(p.delay);
		raw(p.creation_frame);
		raw(p.state_flags);
		region_ring(p.long_path);
		raw(p.full_long_path_size);
		xy_ring(p.short_path);
		raw(p.current_long_path_index);
		raw(p.current_short_path_index);
		raw(p.source);
		raw(p.destination);
		raw(p.next);
		raw(p.last_collision_unit);
		raw(p.last_collision_speed);
		raw(p.slide_free_direction);
	}
	void thingy(thingy_t& t) {
		raw(t);
		sref(t.sprite);
	}

	// ---- fixed-index container occupancy --------------------------------
	// Reset+grow on load so every slot exists before any pointer resolves;
	// preserve free_list ORDER (it decides which index the next spawn gets).
	template<typename Cont>
	void container_free(Cont& cont) {
		if (V::reading) {
			uint32_t size = u32(0);
			cont.free_list.clear();
			cont.list.clear();
			cont.size = 0;
			while (cont.size < size) cont.grow(false);
			uint32_t fn = u32(0);
			for (uint32_t i = 0; i != fn; ++i) cont.free_list.push_back(*cont.get(u32(0)));
		} else {
			u32((uint32_t)cont.size);
			uint32_t fn = 0; for (auto& e : cont.free_list) { (void)e; ++fn; } u32(fn);
			for (auto& e : cont.free_list) u32((uint32_t)e.index);
		}
	}
	// Occupied = grown slots not in the free list. Save/load index + payload.
	template<typename Cont, typename Visit>
	void container_occupied(Cont& cont, Visit visit) {
		if (V::reading) {
			uint32_t n = u32(0);
			for (uint32_t i = 0; i != n; ++i) { uint32_t idx = u32(0); visit(*cont.get(idx)); }
		} else {
			a_unordered_set<uint32_t> free;
			for (auto& e : cont.free_list) free.insert((uint32_t)e.index);
			// Visit exactly the `size` grown slots by internal position: a
			// partial trailing block must never feed a garbage index into
			// get(), which would auto-grow the container (an earlier block-walk
			// did exactly that and corrupted the heap). Occupied iff the
			// external index is not in the free set.
			const size_t gran = cont.list.empty() ? 1 : cont.list.front().size();
			a_vector<uint32_t> occ;
			for (size_t i = 0; i != cont.size; ++i) {
				uint32_t ext = (uint32_t)cont.list[i / gran][i % gran].index;
				if (!free.count(ext)) occ.push_back(ext);
			}
			u32((uint32_t)occ.size());
			for (uint32_t ext : occ) { u32(ext); visit(*cont.get(ext)); }
		}
	}

	// Second pass over the SAME occupied set (recomputed from the container,
	// which is identical on save and load after the data pass), with NO stream
	// prefix -- the visitor writes only the intrusive-list contents. Iteration
	// order is internal-index, so it matches on both sides.
	template<typename Cont, typename Visit>
	void container_lists(Cont& cont, Visit visit) {
		a_unordered_set<uint32_t> free;
		for (auto& e : cont.free_list) free.insert((uint32_t)e.index);
		const size_t gran = cont.list.empty() ? 1 : cont.list.front().size();
		for (size_t i = 0; i != cont.size; ++i) {
			uint32_t ext = (uint32_t)cont.list[i / gran][i % gran].index;
			if (!free.count(ext)) visit(*cont.get(ext));
		}
	}

	// ---- heap pools: allocate the nodes, then everything references them --
	void pools() {
		if (V::reading) {
			uint32_t np = u32(0);
			st.paths.clear();
			path_by_index.clear();
			for (uint32_t i = 0; i != np; ++i) { st.paths.emplace_back(); path_by_index.push_back(&st.paths.back()); }
			uint32_t nt = u32(0);
			st.thingies.clear();
			thingy_by_index.clear();
			for (uint32_t i = 0; i != nt; ++i) { st.thingies.emplace_back(); thingy_by_index.push_back(&st.thingies.back()); }
		} else {
			uint32_t idx = 0;
			for (auto& p : st.paths) path_index[&p] = idx++;
			u32(idx);
			idx = 0;
			for (auto& t : st.thingies) thingy_index[&t] = idx++;
			u32(idx);
		}
	}

	template<typename List> void thingy_list(List& l) {
		if (V::reading) { uint32_t n = u32(0); l.clear(); for (uint32_t i = 0; i != n; ++i) l.push_back(*thingy_by_index[u32(0)]); }
		else { uint32_t n = 0; for (auto& e : l) { (void)e; ++n; } u32(n); for (auto& e : l) u32(thingy_index.at(&e)); }
	}
	template<typename List> void path_list(List& l) {
		if (V::reading) { uint32_t n = u32(0); l.clear(); for (uint32_t i = 0; i != n; ++i) l.push_back(*path_by_index[u32(0)]); }
		else { uint32_t n = 0; for (auto& e : l) { (void)e; ++n; } u32(n); for (auto& e : l) u32(path_index.at(&e)); }
	}

	template<typename Vec> void finder(Vec& f) {
		if (V::reading) { uint32_t n = u32(0); f.resize(n); for (auto& e : f) { uref(e.u); raw(e.value); } }
		else { u32((uint32_t)f.size()); for (auto& e : f) { uref(e.u); raw(e.value); } }
	}

	void roots() {
		// unit membership lists
		for (auto& l : st.player_units) unit_list(l);
		unit_list(st.cloaked_units);
		unit_list(st.psionic_matrix_units);
		unit_list(st.dead_units);
		unit_list(st.map_revealer_units);
		unit_list(st.hidden_units);
		unit_list(st.visible_units);
		// bullets, sprites-on-tile, thingies, paths
		obj_list(st.active_bullets, st.bullets_container);
		{
			uint32_t n = u32((uint32_t)st.sprites_on_tile_line.size());
			if (V::reading) st.sprites_on_tile_line.resize(n);
			for (auto& l : st.sprites_on_tile_line) obj_list(l, st.sprites_container);
		}
		thingy_list(st.active_thingies);
		thingy_list(st.free_thingies);
		path_list(st.free_paths);
		// unit finder + the two loose pointers
		finder(st.unit_finder_x);
		finder(st.unit_finder_y);
		uref(st.consider_collision_with_unit_bug);
		uref(st.prev_bullet_source_unit);
	}

	void run() {
		// A. container occupancy meta (grows the pools on load)
		container_free(st.units_container);
		container_free(st.bullets_container);
		container_free(st.sprites_container);
		container_free(st.images_container);
		container_free(st.orders_container);
		// B. heap node pools -- must exist before any payload references them
		pools();
		// C. object DATA (raw + non-list refs). Every object is raw-loaded here
		// BEFORE any list push clobbers its link bytes.
		container_occupied(st.units_container,   [&](unit_t& x)   { unit_data(x); });
		container_occupied(st.bullets_container, [&](bullet_t& x) { bullet(x); });
		container_occupied(st.sprites_container, [&](sprite_t& x) { sprite_data(x); });
		container_occupied(st.images_container,  [&](image_t& x)  { image(x); });
		container_occupied(st.orders_container,  [&](order_t& x)  { order(x); });
		// D. heap payloads (no intrusive object lists, so data-safe here)
		if (V::reading) {
			for (auto* p : path_by_index) path(*p);
			for (auto* t : thingy_by_index) thingy(*t);
		} else {
			for (auto& p : st.paths) path(p);
			for (auto& t : st.thingies) thingy(t);
		}
		// E. embedded intrusive lists, now that every object is raw-loaded
		container_lists(st.units_container,   [&](unit_t& x)   { unit_lists(x); });
		container_lists(st.sprites_container, [&](sprite_t& x) { sprite_lists(x); });
		// F. root lists
		roots();
	}
};

template<typename V>
void visit_objects(state& st, V& v) {
	object_io<V> io(st, v);
	io.run();
}

// --- action_state (input / selection / control groups) ---------------
//
// Not needed for the deterministic round-trip (the sim never reads it), but a
// UI that resumes a save wants the selection and control groups back. Both
// selection (unit_t*) and control_groups (unit_id) live in static_vectors, so
// -- like build_queue -- they are rebuilt from a count + elements, never raw-
// copied (a static_vector holds an end-pointer). selection resolves through
// units_container; control_groups are unit_id values, kept verbatim.
template<typename V>
void visit_action_state(action_state& as, state& st, V& v) {
	field(v, as.player_id);
	field(v, as.actions_data_position);
	field(v, as.next_action_frame);
	for (auto& sel : as.selection) {
		uint32_t n = v.io_u32((uint32_t)sel.size());
		if (V::reading) {
			sel.clear();
			for (uint32_t i = 0; i != n; ++i) {
				uint32_t idx = v.io_u32(0);
				sel.push_back(const_cast<unit_t*>(st.units_container.at(idx)));
			}
		} else for (auto* u : sel) v.io_u32((uint32_t)u->index);
	}
	for (auto& grp : as.control_groups) for (auto& cg : grp) {
		uint32_t n = v.io_u32((uint32_t)cg.size());
		if (V::reading) {
			cg.clear();
			for (uint32_t i = 0; i != n; ++i) { unit_id id; v.bytes(&id, sizeof(id)); cg.push_back(id); }
		} else for (auto& id : cg) v.bytes(&id, sizeof(id));
	}
}

// --- disk-ready save / load with a versioned, checked header ---------
//
// The header settles compatibility BEFORE any body is parsed (the VCMI lesson): an incompatible save is rejected, not half-
// applied. engine_version is the app's versionCode; map_id identifies the map
// the save belongs to (e.g. a hash of the CHK) so a save cannot be loaded onto
// the wrong map. The caller loads that map fresh, then load() applies the save.
enum class load_result { ok, bad_magic, bad_format, bad_engine, wrong_map, truncated };

inline a_vector<uint8_t> save(state& st, action_state& as, uint32_t engine_version, uint32_t map_id) {
	writer_t w;
	w.u32(magic);
	w.u32(format_version);
	w.u32(engine_version);
	w.u32(map_id);
	visit_copyable(st, w);
	visit_objects(st, w);
	visit_action_state(as, st, w);
	return std::move(w.out);
}

inline load_result load(state& st, action_state& as, const uint8_t* data, size_t size,
    uint32_t engine_version, uint32_t map_id) {
	reader_t r{data, data + size};
	if (r.u32() != magic) return load_result::bad_magic;
	if (r.u32() != format_version) return load_result::bad_format;
	if (r.u32() != engine_version) return load_result::bad_engine;
	if (r.u32() != map_id) return load_result::wrong_map;
	visit_copyable(st, r);
	visit_objects(st, r);
	visit_action_state(as, st, r);
	if (r.failed) return load_result::truncated;
	return load_result::ok;
}

// --- What is deliberately still missing ------------------------------
//
// 1. action_state (input / selection / current order builder, ~3.5 kB). The
//    deterministic sim never reads it -- that is exactly why savetest PASSES
//    without it -- so a save that restores gameplay does not need it. A UI that
//    resumes with the same selection would.
// 2. A versioned file header: magic + format_version + engine versionCode +
//    map id, so an incompatible save is REJECTED rather than half-applied. The
//    VCMI lesson holds: settle compatibility
//    before parsing, not after. Needed before this is written to disk for real.
//
// Lessons this file paid for, kept so the next change does not re-pay them:
//  - save(st) == save(copy_state(st)) does NOT catch a forgotten field (it is
//    absent from both sides). Only the behavioural harness does. It earned its
//    keep here twice: it turned a byte-perfect but wrong save (an order's link
//    clobbered after it was queued) from a green round-trip into a red step.
//  - a static_vector uses an END-POINTER, so raw-copying one across the save
//    boundary makes size() garbage on the far side. build_queue is rebuilt, not
//    raw-trusted; see type_vec.
//  - raw() overwrites intrusive-link bytes, so every object must be raw-loaded
//    BEFORE it is pushed into any list. Hence the strict data-then-lists phase
//    order in object_io::run().

} // namespace savegame
} // namespace bwgame

#endif
