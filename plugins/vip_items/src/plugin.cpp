// vip_items — Plugify port of the Pisex VIP module "VIP_Items"
// (the original's vip_items.cpp).
//
// Behaviour kept identical:
//   * config <configs>/vip_items/vip_items.ini ("round_min"), the original
//     addons/configs/vip/vip_items.ini.
//   * the feature is registered as "items" (VIP_BOOL, TOGGLABLE); its value is the space-separated
//     (quotes allowed) weapon list, e.g. "weapon_ak47 weapon_flashbang".
//   * on VIP_OnPlayerSpawn (the original's OnPlayerSpawn) a VIP gets every item of the list on the
//     original round condition; grenades are given only when the player has no ammo of that
//     grenade type (ammo slots 13..17), all other weapons only when they are not carried yet.
//   * the original refused to load without a config; the port logs and stays disabled instead
//     (PITFALLS A1: OnPluginStart never returns Failed).
//
// Pure s2sdk, no pisexcompat (docs/PLUGIN_STANDARD.md §7). Every safety rule the old wrappers
// enforced lives in this plugin:
//   * guards.hpp  map gate (PITFALLS B1/B2: the OnMapStart/OnMapEnd listeners, opened on the first
//                 update for a plugin loaded mid-map, removed in OnPluginEnd), IsConnected/
//                 IsAlive (PITFALLS C2: slot range, connected, in game, not SourceTV, controller
//                 pointer/handle, pawn pointer/handle, IsClientAlive) and the map-gated
//                 SchemaOffset cache (PITFALLS C8: -1 outside a live map, only successful offsets
//                 cached, cleared on unload);
//   * kv1.hpp     the self-contained Valve KeyValues reader for the config (the original's file is
//                 read unchanged);
//   * items_logic.hpp  the engine-free decisions shared with the test: the feature-string
//                 tokenizer, the round condition, the grenade ammo slots, the handle filter and
//                 the carried-weapon helpers.
//
// Port notes:
//   * the weapon list of the player comes from s2sdk::GetClientWeapons, whose entries are entity
//     handles (the original walked CPlayer_WeaponServices::m_hMyWeapons). Each handle is filtered
//     (not 0 / not -1), validated with s2sdk::IsValidEntHandle(handle) and then passed straight to
//     s2sdk::GetEntityClassname(handle) - the s2sdk 3.0 export takes the handle itself, so the old
//     EntHandleToEntIndex + index->classname round trip is gone.
//   * the grenade ammo counts come from the pawn's weapon-services ammo array (m_iAmmo, a
//     uint16[32], the field the original read directly), via the map-gated schema offsets and a
//     raw GetEntData2 read of the non-entity services component in the same tick the pawn was
//     validated (PITFALLS C3/C4).
//   * CCSPlayer_ItemServices::GiveNamedItem -> s2sdk::GiveNamedItem(slot, name).

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "common.hpp"
#include "guards.hpp"
#include "items_logic.hpp"
#include "kv1.hpp"
#include "vip_api_compat.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE, ...): the
// consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

// --- schema offsets (PITFALLS C8) -------------------------------------------------------------

// CBasePlayerPawn::m_pWeaponServices (a stored pointer, 8 bytes) and CPlayer_WeaponServices::m_iAmmo
// (uint16[32], 2 bytes per element). Both are resolved only while a map is up and cached on the
// first success by guards.hpp; -1 means "no map yet or unknown field" and the ammo read is skipped.
int32_t WeaponServicesOffset() {
	return vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices");
}

int32_t AmmoOffset() {
	return vip_items::SchemaOffset("CPlayer_WeaponServices", "m_iAmmo");
}

// m_iAmmo[ammoIndex] of the player's weapon services, -1 when it cannot be read.
int GetClientAmmo(int iSlot, int iAmmoIndex) {
	// s2sdk's GetClientPawn warns "invalid player slot: N" (to the server console) for every call
	// on a slot without a client/controller, so the map gate and the connection are checked here
	// first. OnPlayerSpawn already ran IsAlive; this is the layer that keeps the helper safe on its
	// own.
	if (__s2sdk_GetClientPawn == nullptr || __s2sdk_GetEntData2 == nullptr ||
		!vip_items::IsConnected(iSlot)) {
		return -1;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr) {
			return -1;
		}
		const int32_t servicesOffset = WeaponServicesOffset();
		const int32_t ammoOffset = AmmoOffset();
		if (servicesOffset < 0 || ammoOffset < 0) {
			return -1; // no map / the field is gone: never guess an offset
		}
		// m_pWeaponServices is a stored pointer: read the 8 bytes at the pawn's schema offset. The
		// services component is not an entity, so its ammo array is read through GetEntData2 at the
		// cached offset - in this same tick, right after the pawn pointer was obtained.
		void* services = *reinterpret_cast<void**>(static_cast<char*>(pawn) + servicesOffset);
		if (services == nullptr) {
			return -1;
		}
		return static_cast<int>(s2sdk::GetEntData2(services, ammoOffset + iAmmoIndex * 2, 2));
	} catch (...) {
		return -1;
	}
}

class ItemsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void LoadConfig();
	void OnPlayerSpawn(int iSlot, bool bIsVIP);

	bool m_loaded = false;
	int m_roundMin = vip_items::kDefaultRoundMin;
};

ItemsPlugin g_itemsPlugin;

plg::PluginResult ItemsPlugin::OnPluginStart() {
	try {
		// Map listeners first: they gate every client/entity/schema call this plugin ever makes, and
		// registering them touches neither a client nor an entity (PITFALLS B1). Never fatal: a
		// missing binding only logs and keeps the gate shut.
		vip_items::InitMapGate();

		LoadConfig();

		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool bIsVIP) {
			// Nothing may unwind into the vip core's spawn dispatch.
			try {
				OnPlayerSpawn(iSlot, bIsVIP);
			} catch (const std::exception& ex) {
				vip_items::LogError(std::string("vip_items: OnPlayerSpawn failed: ") + ex.what());
			} catch (...) {
				vip_items::LogError("vip_items: OnPlayerSpawn failed with an unknown exception");
			}
		});
		g_pVIPCore->VIP_RegisterFeature(vip_items::kFeature, VIP_BOOL, TOGGLABLE);

		vip_items::LogInfo("vip_items: ready");
	} catch (const std::exception& ex) {
		vip_items::LogError(std::string("vip_items: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		vip_items::LogError("vip_items: OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult ItemsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// Opens the gate on the first update after a mid-map load; after OnMapEnd only a new
		// OnMapStart opens it again (PITFALLS B1/B2).
		vip_items::PollMapGate();
	} catch (const std::exception& ex) {
		vip_items::LogError(std::string("vip_items: OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		vip_items::LogError("vip_items: OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult ItemsPlugin::OnPluginEnd() {
	// Every core -> module callback and every engine listener registered in OnPluginStart is
	// dropped while this library is still mapped, so `plg reload` leaves nothing behind
	// (PITFALLS A7). Each call is guarded: the core may already be going away on a full unload.
	try {
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	} catch (...) {
	}
	// The vip API has no feature unregister, but this plugin registers "items" without any
	// callbacks (the core holds no pointer into this library for it), so re-registering would only
	// rewrite the same null delegates - unlike vip_jumps/vip_nofalldamage, nothing has to be
	// cleared there.
	try {
		vip_items::ShutdownMapGate();
	} catch (...) {
	}
	// A reload resolves the schema fields against the live map again (PITFALLS C8).
	vip_items::ClearSchemaOffsets();
	return {};
}

void ItemsPlugin::LoadConfig() {
	m_loaded = false;
	const std::string path = vip_items::ConfigPath();
	if (path.empty()) {
		vip_items::LogError("vip_items: the Plugify configs directory is unavailable - "
							"the module stays disabled (no items)");
		return;
	}
	const std::optional<vip_items::KvNode> config = vip_items::LoadKv1File(path);
	if (!config) {
		vip_items::LogError("vip_items: failed to load " + path +
							" - the module stays disabled (no items)");
		return;
	}
	m_roundMin = config->GetInt("round_min", vip_items::kDefaultRoundMin);
	m_loaded = true;
}

void ItemsPlugin::OnPlayerSpawn(int iSlot, bool bIsVIP) {
	if (!m_loaded || !bIsVIP || !vip_items::IsSlotInRange(iSlot)) {
		return;
	}
	if (__s2sdk_GiveNamedItem == nullptr) {
		return;
	}
	const bool bPistolRound = g_pVIPCore->VIP_PistolRound();
	const int iTotalRounds = g_pVIPCore->VIP_GetTotalRounds();
	// The original's round condition (items_logic.hpp, shared with the test).
	if (!vip_items::ShouldGiveOnRound(m_roundMin, iTotalRounds, bPistolRound)) {
		return;
	}

	// The feature value is the weapon list. The core returns a pointer into its per-plugin buffer,
	// so it is copied before anything else can query the core again.
	const char* szItems = g_pVIPCore->VIP_GetClientFeatureString(iSlot, vip_items::kFeature);
	if (szItems == nullptr || szItems[0] == '\0') {
		return;
	}
	const std::vector<std::string> items = vip_items::SplitStringBySpace(std::string(szItems));
	if (items.empty()) {
		return;
	}

	// The original's `!pPlayerPawn || !pPlayerPawn->IsAlive()` check plus every s2sdk guard
	// (PITFALLS B1/B2, C2): nothing below reaches the engine outside a live map or for a player
	// without a pawn.
	if (!vip_items::IsAlive(iSlot)) {
		return;
	}
	if (__s2sdk_GetClientWeapons == nullptr || __s2sdk_IsValidEntHandle == nullptr ||
		__s2sdk_GetEntityClassname == nullptr) {
		return;
	}

	// Classnames of the weapons the player already carries (hWeapons of the original).
	std::vector<std::string> carried;
	try {
		carried = vip_items::CollectCarriedClassnames(
			s2sdk::GetClientWeapons(iSlot), [](int32_t handle) -> std::string {
				// s2sdk hands out the m_hMyWeapons handles and does not validate them: the handle is
				// checked against the entity system before GetEntityClassname is called, and the
				// binding itself is checked so a missing export is not a null call (PITFALLS C2).
				// The numeric filter (0 = empty slot, -1 = INVALID_EHANDLE_INDEX) lives in
				// items_logic.hpp; it is not "handle > 0" because a valid handle may be negative
				// once the serial passes 65535 (PITFALLS C1).
				if (__s2sdk_IsValidEntHandle == nullptr ||
					!s2sdk::IsValidEntHandle(handle) ||
					__s2sdk_GetEntityClassname == nullptr) {
					return {};
				}
				try {
					const plg::string classname = s2sdk::GetEntityClassname(handle);
					return std::string(classname.data(), classname.size());
				} catch (...) {
					return {};
				}
			});
	} catch (...) {
		return;
	}

	const auto give = [iSlot](const std::string& item) {
		if (__s2sdk_GiveNamedItem == nullptr) {
			return;
		}
		try {
			s2sdk::GiveNamedItem(iSlot, plg::string(item));
		} catch (...) {
		}
	};

	for (const std::string& item : items) {
		const int ammoIndex = vip_items::GrenadeAmmoIndexFor(item);
		if (ammoIndex >= 0) {
			// A grenade is given only when the player has none of that type. GetClientAmmo answers
			// -1 when the value cannot be read, and -1 != 0 keeps the item back (never guess).
			if (GetClientAmmo(iSlot, ammoIndex) == 0) {
				give(item);
			}
		} else if (!vip_items::IsCarried(carried, item)) {
			give(item);
		}
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_itemsPlugin)
