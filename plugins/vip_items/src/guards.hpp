// vip_items — the plugin's own s2sdk safety guards (PITFALLS B1/B2, C2, C8).
//
// The plugin calls s2sdk directly (docs/PLUGIN_STANDARD.md §7: no pisexcompat), so every rule the
// old wrappers enforced lives here:
//
//   * Map gate: no client, entity or schema call is made before the first map, and none in the
//     window from OnMapEnd until the next OnMapStart (the client list is torn down there and
//     s2sdk's client lookups read an emptied list - PITFALLS B1/B2). The listeners are registered
//     in OnPluginStart, the gate is opened by OnMapStart (and by the first OnPluginUpdate, which
//     proves a live map for a plugin loaded mid-map) and closed by OnMapEnd. OnPluginEnd removes
//     the listeners again, so `plg reload` leaves nothing pointing into this library (A7).
//   * Client checks: s2sdk exports do not validate their arguments (PITFALLS C2), so
//     IsConnected() layers the map gate, the slot range and IsClientConnected, and IsAlive() adds
//     in-game, !IsClientSourceTV, a controller entity pointer *and* handle, a pawn pointer *and*
//     handle, and finally IsClientAlive. Nothing below a failed layer reaches s2sdk.
//   * Schema offsets: GetSchemaOffset segfaults before the first map and between OnMapEnd and the
//     next OnMapStart (PITFALLS C8), so SchemaOffset() answers -1 while the gate is closed,
//     memoises only offsets >= 0 (a failure is retried instead of poisoning the field) and is
//     cleared in OnPluginEnd.
//
// Header-only (inline): included by src/plugin.cpp and by tests/vip_items_tests.cpp, which installs
// fake __s2sdk_* bindings and checks every layer without an engine.

#pragma once

#include <cstdint>
#include <string>

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include "common.hpp"
#include "items_logic.hpp"
#include "schema_cache.hpp"

namespace vip_items {
namespace map_gate_detail {

// The same three flags (and the same state machine) pisexcompat and the other ported plugins keep:
//   g_mapReady             a map is live - the only state in which a client/entity/schema call is
//                          allowed
//   g_mapStartSeen         an OnMapStart arrived since OnPluginStart / the last OnMapEnd
//   g_mapEndSeen           an OnMapEnd arrived since OnPluginStart
inline bool g_mapReady = false;
inline bool g_mapStartSeen = false;
inline bool g_mapEndSeen = false;
inline bool g_listenersRegistered = false;

// s2sdk listeners: plain function pointers called on the game thread. Nothing here may throw, and
// no client or entity call is made - only state changes. inline so that plugin and test share one
// address per program.
inline void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist: readiness is granted here
	// for everything that follows (PITFALLS B1). A plugin loaded into a running map never sees this
	// callback - PollMapGate() covers it.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

inline void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

} // namespace map_gate_detail

// --- map gate ---------------------------------------------------------------------------------

// Registers the OnMapStart/OnMapEnd listeners. Touches no client; safe in OnPluginStart.
inline void InitMapGate() {
	map_gate_detail::g_mapReady = false;
	map_gate_detail::g_mapStartSeen = false;
	map_gate_detail::g_mapEndSeen = false;
	if (map_gate_detail::g_listenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without both listeners nothing could ever close the gate again, so it is never opened
		// (fail safe, PITFALLS A1: log and stay inactive, never return Failed).
		LogError(std::string("[Error] [") + kLogTag +
				 "] the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&map_gate_detail::OnMapStartListener);
	s2sdk::OnMapEnd_Register(&map_gate_detail::OnMapEndListener);
	map_gate_detail::g_listenersRegistered = true;
}

// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
inline void PollMapGate() {
	if (!map_gate_detail::g_listenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart proves a live map (the host pumps updates from the server frame). After an
	// OnMapEnd only a new OnMapStart may re-open the gate - the host keeps pumping updates through
	// a changelevel, so "no OnMapEnd seen" must not be enough once one was seen.
	if (!map_gate_detail::g_mapReady &&
		(map_gate_detail::g_mapStartSeen || !map_gate_detail::g_mapEndSeen)) {
		map_gate_detail::g_mapReady = true;
	}
}

// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
inline void ShutdownMapGate() {
	if (map_gate_detail::g_listenersRegistered) {
		map_gate_detail::g_listenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&map_gate_detail::OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&map_gate_detail::OnMapEndListener);
		}
	}
	// Drop readiness last: nothing can reach a client through this plugin after OnPluginEnd.
	map_gate_detail::g_mapReady = false;
	map_gate_detail::g_mapStartSeen = false;
	map_gate_detail::g_mapEndSeen = false;
}

// True while a map is up and client/entity/schema calls are safe.
inline bool IsMapReady() {
	return map_gate_detail::g_mapReady;
}

// --- client checks (PITFALLS C2) ---------------------------------------------------------------

// A map is up and the slot is in range: an s2sdk client query on this slot is safe.
inline bool CanQueryClient(int32_t slot) {
	return IsMapReady() && IsSlotInRange(slot);
}

// The Pisex IsConnected(): a map is up, the slot is in range and the engine reports a connected
// client (bots count as clients - the original gave items to them too).
inline bool IsConnected(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

// The Pisex IsAlive() the original's `!pPlayerPawn || !pPlayerPawn->IsAlive()` stands for: a
// connected, in-game, non-SourceTV client with a controller entity (pointer and handle) and a
// living pawn (pointer and handle). Every s2sdk export the chain needs is checked for a null
// binding before it is called; a throwing export counts as "not alive".
inline bool IsAlive(int32_t slot) {
	if (!IsConnected(slot)) {
		return false;
	}
	if (__s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_IsClientAlive == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_GetClientPawn == nullptr ||
		__s2sdk_EntPointerToEntHandle == nullptr || __s2sdk_IsValidEntHandle == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientInGame(slot) || s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		// The controller entity: the pointer must exist and its handle must be valid (C2).
		if (s2sdk::PlayerSlotToEntPointer(slot) == nullptr) {
			return false;
		}
		const int32_t controller = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsUsableEntHandle(controller) || !s2sdk::IsValidEntHandle(controller)) {
			return false;
		}
		// The pawn: s2sdk::GetClientPawn answers the pointer, EntPointerToEntHandle its handle; the
		// old "GetPawnEntity(i) == 0" skip is the invalid-handle/!pointer case.
		void* pawn = s2sdk::GetClientPawn(slot);
		if (pawn == nullptr) {
			return false;
		}
		const int32_t pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
		if (!IsUsableEntHandle(pawnHandle) || !s2sdk::IsValidEntHandle(pawnHandle) ||
			!s2sdk::IsValidEntPointer(pawn)) {
			return false;
		}
		return s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

// --- schema offsets (PITFALLS C8) --------------------------------------------------------------

inline SchemaOffsetCache g_schemaOffsets;

inline int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName)); // -1: not found
	} catch (...) {
		return -1;
	}
}

// The map-gated lookup, the replacement of pisex::SchemaOffset:
//   * -1 while the map gate is closed: the serializer map the lookup walks does not exist before
//     the first map and in the window between OnMapEnd and the next OnMapStart;
//   * only an offset >= 0 is cached - a -1 (unknown field, or a lookup that ran too early) is
//     retried on the next call instead of poisoning the field for the whole session;
//   * ClearSchemaOffsets() in OnPluginEnd makes `plg reload` resolve the fields again.
inline int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return g_schemaOffsets.Offset(IsMapReady(), className, fieldName, &LookupSchemaOffset);
	} catch (...) {
		return -1; // the cache allocates; the spawn handler may not unwind into the engine
	}
}

inline void ClearSchemaOffsets() {
	g_schemaOffsets.Clear();
}

} // namespace vip_items
