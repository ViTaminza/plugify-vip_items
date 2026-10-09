// vip_items unit tests — the engine-independent parts: the feature-string tokenizer (the
// original's SplitStringBySpace) and the grenade ammo slot table of its if-chain.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "items_logic.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
			 const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected %zu tokens, got %zu\n", what.c_str(), expected.size(), actual.size());
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void TestTokenizer() {
	using vip_items::SplitStringBySpace;

	// The production groups.ini values, e.g. "weapon_hegrenade weapon_smokegrenade weapon_flashbang
	// weapon_healthshot".
	CheckEq(SplitStringBySpace("weapon_ak47 weapon_flashbang"),
			{"weapon_ak47", "weapon_flashbang"}, "two weapons");
	CheckEq(SplitStringBySpace("  weapon_ak47   weapon_flashbang  "),
			{"weapon_ak47", "weapon_flashbang"}, "extra whitespace");
	CheckEq(SplitStringBySpace(""), {}, "empty value");
	CheckEq(SplitStringBySpace("   "), {}, "whitespace only");
	CheckEq(SplitStringBySpace("weapon_ak47"), {"weapon_ak47"}, "single weapon");
	CheckEq(SplitStringBySpace("\"weapon_ak47\" \"weapon_flashbang\""),
			{"weapon_ak47", "weapon_flashbang"}, "quoted tokens");
	CheckEq(SplitStringBySpace("\"weapon_ak 47\""), {"weapon_ak 47"}, "quotes group spaces");
	CheckEq(SplitStringBySpace("a\tb\nc"), {"a", "b", "c"}, "tab and newline are separators");
}

void TestGrenadeAmmoIndex() {
	using vip_items::GrenadeAmmoIndexFor;

	CheckEq(GrenadeAmmoIndexFor("weapon_hegrenade"), 13, "hegrenade ammo slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_flashbang"), 14, "flashbang ammo slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_smokegrenade"), 15, "smokegrenade ammo slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_molotov"), 16, "molotov ammo slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_incgrenade"), 16, "incgrenade shares the molotov slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_decoy"), 17, "decoy ammo slot");
	CheckEq(GrenadeAmmoIndexFor("weapon_ak47"), -1, "rifles are not grenades");
	CheckEq(GrenadeAmmoIndexFor(""), -1, "empty name");
}

} // namespace

int main() {
	std::printf("vip_items tests\n");
	TestTokenizer();
	TestGrenadeAmmoIndex();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
