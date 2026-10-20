#pragma once
// Engine-free helpers of the VIP_Items port: the feature-string tokenizer (the original
// SplitStringBySpace) and the grenade ammo slots the module checks before giving a grenade.
//
// The grenade ammo indices are the CS2 weapon-script ammo slots of the original if-chain:
// hegrenade 13, flashbang 14, smokegrenade 15, molotov/incgrenade 16, decoy 17. Every other item
// is handed out when the player does not carry it yet.

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace vip_items {

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

} // namespace vip_items
