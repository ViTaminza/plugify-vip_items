#pragma once
// vip_items — engine-free helpers of the VIP_Items port: the feature-string tokenizer (the
// original's SplitStringBySpace), the round condition of its spawn handler, the grenade ammo slots
// and the two "carried weapons" helpers.
//
// Header-only: shared by src/plugin.cpp and tests/vip_items_tests.cpp, so the decisions are checked
// without an engine, without s2sdk and without any binding.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vip_items {

// CS2 exposes 64 player slots; a slot outside [0, kMaxSlots) has no client record and must never
// reach an s2sdk export (PITFALLS C2).
inline constexpr int kMaxSlots = 64;

constexpr bool IsSlotInRange(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// A raw s2sdk entity id (CEntityHandle): 0 is "no entity" (the world / an empty weapon slot) and -1
// is INVALID_EHANDLE_INDEX. Handles are index | serial << 15 in 32 bits, so once the game's serial
// counter passes 65535 a perfectly valid handle is a *negative* int (PITFALLS C1). That is why this
// cheap filter is "not 0 and not -1" and not "> 0"; s2sdk::IsValidEntHandle() stays the
// authoritative check right after it.
constexpr bool IsUsableEntHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

// The round condition of the original's VIP_OnPlayerSpawn, unchanged:
//   * round_min == 0: every round (including the pistol round),
//   * round_min == 1: every round except the pistol round,
//   * round_min >= 2: only once that many rounds have been played, and never on a pistol round.
constexpr bool ShouldGiveOnRound(int roundMin, int totalRounds, bool pistolRound) {
	return (roundMin == 1 && !pistolRound) || (roundMin <= totalRounds && !pistolRound) || (roundMin == 0);
}

// Splits a feature string on whitespace; double quotes group a token and are removed.
inline std::vector<std::string> SplitStringBySpace(const std::string& input) {
	std::vector<std::string> tokens;
	std::string current;
	bool inQuotes = false;
	for (const char ch : input) {
		if (ch == '"') {
			inQuotes = !inQuotes;
			continue;
		}
		if (std::isspace(static_cast<unsigned char>(ch)) != 0 && !inQuotes) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current += ch;
		}
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

// Ammo slot of the grenade type the item stands for; -1 when the item is not one of the grenades
// the original special-cased.
//
// The indices are the CS2 weapon-script ammo slots of the original if-chain: hegrenade 13,
// flashbang 14, smokegrenade 15, molotov/incgrenade 16, decoy 17. CPlayer_WeaponServices::m_iAmmo
// is a uint16[32] (sdk.json), so the plugin reads two bytes at offset + index * 2.
inline int GrenadeAmmoIndexFor(std::string_view item) {
	if (item == "weapon_hegrenade") {
		return 13;
	}
	if (item == "weapon_flashbang") {
		return 14;
	}
	if (item == "weapon_smokegrenade") {
		return 15;
	}
	if (item == "weapon_molotov" || item == "weapon_incgrenade") {
		return 16;
	}
	if (item == "weapon_decoy") {
		return 17;
	}
	return -1;
}

// The original's std::find(hWeapons.begin(), hWeapons.end(), m_items[i]) test.
inline bool IsCarried(const std::vector<std::string>& carried, std::string_view item) {
	for (const std::string& entry : carried) {
		if (std::string_view(entry) == item) {
			return true;
		}
	}
	return false;
}

// Builds the hWeapons list of the original from the handles s2sdk::GetClientWeapons() hands out.
//
// The cheap handle filter is applied here (the same rule for every caller and covered by the
// tests); `classnameOf` is the engine-facing part - the plugin validates the handle with
// s2sdk::IsValidEntHandle() and reads s2sdk::GetEntityClassname() there, and returns "" for
// anything the engine refused. An empty classname is skipped, exactly like the original skipped a
// weapon whose pointer was null.
template <typename Handles, typename ClassnameForHandle>
std::vector<std::string> CollectCarriedClassnames(const Handles& handles, ClassnameForHandle classnameOf) {
	std::vector<std::string> carried;
	for (const auto rawHandle : handles) {
		const int32_t handle = static_cast<int32_t>(rawHandle);
		if (!IsUsableEntHandle(handle)) {
			continue;
		}
		std::string classname = classnameOf(handle);
		if (!classname.empty()) {
			carried.push_back(std::move(classname));
		}
	}
	return carried;
}

} // namespace vip_items
