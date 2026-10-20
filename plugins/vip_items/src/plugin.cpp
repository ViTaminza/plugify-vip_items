// vip_items — Plugify port of the Pisex VIP module "VIP_Items"
// (the original's vip_items.cpp).
//
// Behaviour kept identical:
//   * config <configs>/vip_items/vip_items.ini ("round_min"), the original
//     addons/configs/vip/vip_items.ini.
//   * the feature is registered as "items" (VIP_BOOL, TOGGLABLE); its value is the
//     space-separated (quotes allowed) weapon list, e.g. "weapon_ak47 weapon_flashbang".
//   * on VIP_OnPlayerSpawn (the original's OnPlayerSpawn) a VIP gets every item of the list on the
//     original round condition; grenades are given only when the player has no ammo of that
//     grenade type (ammo slots 13..17), all other weapons only when they are not carried yet.
//
// Port notes:
//   * the weapon list of the player comes from s2sdk::GetClientWeapons + pisex::GetEntityClassname
//     (the original walked the weapon services' weapon list and compared the classnames).
//   * the grenade ammo counts come from the pawn's weapon services ammo array (m_iAmmo, the field
//     the original read directly); see vip_healthshot for the schema access.
//   * CCSPlayer_ItemServices::GiveNamedItem -> s2sdk::GiveNamedItem(slot, name).
//   * the original refused to load without a config; the port logs and stays disabled instead.

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "items_logic.hpp"
#include "vip_api_compat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

constexpr int kMaxSlots = 64;
constexpr const char* kFeature = "items";
constexpr const char* kConfigFile = "vip_items.ini";

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// m_iAmmo[ammoIndex] of the player's weapon services, -1 when it cannot be read.
int GetClientAmmo(int iSlot, int iAmmoIndex) {
	// s2sdk's GetClientPawn warns "invalid player slot: N" (to the server console) for every call
	// on a slot without a controller, so an empty slot is filtered out here.
	if (!__s2sdk_GetClientPawn || !__s2sdk_GetEntData2 || iSlot < 0 || iSlot >= kMaxSlots ||
		!pisex::IsConnected(iSlot)) {
		return -1;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (!pawn) {
			return -1;
		}
		const int servicesOffset = pisex::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices");
		const int ammoOffset = pisex::SchemaOffset("CPlayer_WeaponServices", "m_iAmmo");
		if (servicesOffset < 0 || ammoOffset < 0) {
			return -1;
		}
		void* services = *reinterpret_cast<void**>(static_cast<char*>(pawn) + servicesOffset);
		if (!services) {
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
	int m_roundMin = 0;
};

ItemsPlugin g_itemsPlugin;

plg::PluginResult ItemsPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_items");

		LoadConfig();

		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool bIsVIP) {
			OnPlayerSpawn(iSlot, bIsVIP);
		});
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);

		LogInfo("vip_items: ready");
	} catch (const std::exception& ex) {
		LogError(std::string("vip_items: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("vip_items: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult ItemsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	pisex::Poll();
	return {};
}

plg::PluginResult ItemsPlugin::OnPluginEnd() {
	g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	pisex::Shutdown();
	return {};
}

void ItemsPlugin::LoadConfig() {
	m_loaded = false;
	pisex::KeyValues kv("VIP");
	if (!kv.LoadFromFile(kConfigFile)) {
		LogError(std::string("vip_items: failed to load ") + kConfigFile +
				 " - the module stays disabled (no items)");
		return;
	}
	m_roundMin = kv.GetInt("round_min");
	m_loaded = true;
}

void ItemsPlugin::OnPlayerSpawn(int iSlot, bool bIsVIP) {
	if (!m_loaded || !bIsVIP || iSlot < 0 || iSlot >= kMaxSlots || !__s2sdk_GiveNamedItem) {
		return;
	}
	const bool bPistolRound = g_pVIPCore->VIP_PistolRound();
	const int iTotalRounds = g_pVIPCore->VIP_GetTotalRounds();
	if (!((m_roundMin == 1 && !bPistolRound) || (m_roundMin <= iTotalRounds && !bPistolRound) || (m_roundMin == 0))) {
		return;
	}
	const char* szItems = g_pVIPCore->VIP_GetClientFeatureString(iSlot, kFeature);
	if (szItems == nullptr || szItems[0] == '\0' || !pisex::IsAlive(iSlot) || !__s2sdk_GetClientWeapons) {
		return;
	}
	const std::vector<std::string> items = vip_items::SplitStringBySpace(szItems);
	if (items.empty()) {
		return;
	}

	// Classnames of the weapons the player already carries (hWeapons of the original).
	std::vector<std::string> carried;
	try {
		for (const int32_t handle : s2sdk::GetClientWeapons(iSlot)) {
			const int index = pisex::EntHandleToEntIndex(handle);
			if (index <= 0) {
				continue;
			}
			const std::string classname = pisex::GetEntityClassname(index);
			if (!classname.empty()) {
				carried.push_back(classname);
			}
		}
	} catch (...) {
		return;
	}

	const auto give = [iSlot](const std::string& item) {
		try {
			s2sdk::GiveNamedItem(iSlot, plg::string(item));
		} catch (...) {
		}
	};

	for (const std::string& item : items) {
		const int ammoIndex = vip_items::GrenadeAmmoIndexFor(item);
		if (ammoIndex >= 0) {
			// A grenade is given only when the player has none of that type.
			if (GetClientAmmo(iSlot, ammoIndex) == 0) {
				give(item);
			}
		} else if (std::find(carried.begin(), carried.end(), item) == carried.end()) {
			give(item);
		}
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_itemsPlugin)
