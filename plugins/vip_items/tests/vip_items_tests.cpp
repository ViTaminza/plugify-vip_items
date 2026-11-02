// vip_items_tests — the test binary of the pure-s2sdk vip_items plugin.
//
// Two halves, neither of which links pisexcompat:
//
//   1. Engine-free logic (items_logic.hpp, kv1.hpp, common.hpp): the feature-string tokenizer (the
//      original's SplitStringBySpace), the round condition of VIP_OnPlayerSpawn, the grenade ammo
//      slots of its if-chain, the carried-weapon helpers (the handle filter that must keep a valid
//      negative handle - PITFALLS C1 - and drop 0/-1) and the self-contained KV1 reader on the
//      production-shaped vip_items.ini in tests/data, plus the config path.
//   2. The plugin's own guards with fake __s2sdk_* bindings (guards.hpp, the technique
//      vip_regen_armor_tests and vip_vips_tests use): no client or schema query before the first
//      map and after OnMapEnd (PITFALLS B1/B2), the IsConnected/IsAlive chain of C2 and the
//      map-gated, success-only-cached SchemaOffset of C8.
//
// Run by ../../test.sh (build/tests/vip_items_tests), or directly from the build.

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include "common.hpp"
#include "guards.hpp"
#include "items_logic.hpp"
#include "kv1.hpp"
#include "schema_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#ifndef VIP_ITEMS_TEST_DATA_DIR
#define VIP_ITEMS_TEST_DATA_DIR "."
#endif

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null like a test binary
// without the Plugify module, except Log and GetConfigsDir, which it drives itself.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

std::vector<std::string> g_logLines;
plg::Severity g_lastSeverity = plg::Severity::Unknown;

// The configs dir the fake plg::GetConfigsDir answers; the path test drives it.
std::string g_fakeConfigsDir;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	g_lastSeverity = severity;
}

plg::string FakeGetConfigsDir() {
	return plg::string(g_fakeConfigsDir.data(), g_fakeConfigsDir.size());
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected \"%.*s\", got \"%.*s\")\n", what.c_str(),
					static_cast<int>(expected.size()), expected.data(), static_cast<int>(actual.size()),
					actual.data());
	}
}

void CheckEqVec(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
				const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected %zu tokens, got %zu\n", what.c_str(), expected.size(), actual.size());
	}
}

// ==============================================================================================
// Part 1: the engine-free logic
// ==============================================================================================

void TestTokenizer() {
	using vip_items::SplitStringBySpace;

	// The shipped groups.ini values, e.g. "weapon_hegrenade weapon_smokegrenade weapon_flashbang
	// weapon_healthshot".
	CheckEqVec(SplitStringBySpace("weapon_ak47 weapon_flashbang"), {"weapon_ak47", "weapon_flashbang"},
			   "two weapons");
	CheckEqVec(SplitStringBySpace("  weapon_ak47   weapon_flashbang  "),
			   {"weapon_ak47", "weapon_flashbang"}, "extra whitespace");
	CheckEqVec(SplitStringBySpace(""), {}, "empty value");
	CheckEqVec(SplitStringBySpace("   "), {}, "whitespace only");
	CheckEqVec(SplitStringBySpace("weapon_ak47"), {"weapon_ak47"}, "single weapon");
	CheckEqVec(SplitStringBySpace("\"weapon_ak47\" \"weapon_flashbang\""),
			   {"weapon_ak47", "weapon_flashbang"}, "quoted tokens");
	CheckEqVec(SplitStringBySpace("\"weapon_ak 47\""), {"weapon_ak 47"}, "quotes group spaces");
	CheckEqVec(SplitStringBySpace("a\tb\nc"), {"a", "b", "c"}, "tab and newline are separators");
}

void TestGrenadeAmmoIndex() {
	using vip_items::GrenadeAmmoIndexFor;

	CheckEqInt(GrenadeAmmoIndexFor("weapon_hegrenade"), 13, "hegrenade ammo slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_flashbang"), 14, "flashbang ammo slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_smokegrenade"), 15, "smokegrenade ammo slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_molotov"), 16, "molotov ammo slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_incgrenade"), 16, "incgrenade shares the molotov slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_decoy"), 17, "decoy ammo slot");
	CheckEqInt(GrenadeAmmoIndexFor("weapon_ak47"), -1, "rifles are not grenades");
	CheckEqInt(GrenadeAmmoIndexFor(""), -1, "empty name");
}

void TestRoundCondition() {
	using vip_items::ShouldGiveOnRound;

	// round_min 0: every round, including the pistol round.
	Check(ShouldGiveOnRound(0, 0, false), "round_min 0 gives on the first round");
	Check(ShouldGiveOnRound(0, 0, true), "round_min 0 gives on the pistol round");
	Check(ShouldGiveOnRound(0, 7, true), "round_min 0 gives on a later pistol round");

	// round_min 1: every round except the pistol round.
	Check(ShouldGiveOnRound(1, 0, false), "round_min 1 gives on the first non-pistol round");
	Check(ShouldGiveOnRound(1, 5, false), "round_min 1 gives on a later non-pistol round");
	Check(!ShouldGiveOnRound(1, 0, true), "round_min 1 skips the pistol round");
	Check(!ShouldGiveOnRound(1, 5, true), "round_min 1 skips a later pistol round");

	// round_min >= 2 (the production value is 2): only once that many rounds were played, never on
	// a pistol round - the original's VIP_GetTotalRounds() comparison.
	Check(!ShouldGiveOnRound(2, 1, false), "round_min 2 waits for the second round");
	Check(ShouldGiveOnRound(2, 2, false), "round_min 2 gives on the second round");
	Check(ShouldGiveOnRound(2, 9, false), "round_min 2 gives on later rounds");
	Check(!ShouldGiveOnRound(2, 2, true), "round_min 2 skips the pistol round");
	Check(!ShouldGiveOnRound(3, 2, false), "round_min 3 still waits at round 2");
	Check(ShouldGiveOnRound(3, 3, false), "round_min 3 gives on the third round");
}

void TestHandleFilter() {
	using vip_items::IsUsableEntHandle;

	Check(!IsUsableEntHandle(0), "handle 0 (empty slot / world) is filtered out");
	Check(!IsUsableEntHandle(-1), "handle -1 (INVALID_EHANDLE_INDEX) is filtered out");
	Check(IsUsableEntHandle(1), "a small positive handle is usable");
	// Handles are index | serial << 15: once the serial passes 65535 a *valid* handle is negative
	// (PITFALLS C1). The filter must not be "> 0", the tests lock that in.
	const int32_t highSerialHandle = static_cast<int32_t>(0x80000001u);
	Check(highSerialHandle < 0, "the high-serial handle really is negative");
	Check(IsUsableEntHandle(highSerialHandle), "a valid negative handle is not filtered out");
}

void TestIsCarried() {
	const std::vector<std::string> carried{"weapon_ak47", "weapon_flashbang"};

	Check(vip_items::IsCarried(carried, "weapon_ak47"), "a carried weapon is found");
	Check(vip_items::IsCarried(carried, "weapon_flashbang"), "the second carried weapon is found");
	Check(!vip_items::IsCarried(carried, "weapon_m4a1"), "a weapon that is not carried is not found");
	Check(!vip_items::IsCarried(carried, ""), "an empty classname is never carried");
	Check(!vip_items::IsCarried({}, "weapon_ak47"), "an empty carried list matches nothing");
}

void TestCollectCarriedClassnames() {
	// The handles s2sdk::GetClientWeapons() may hand out: a real one, the two invalid spellings and
	// a valid negative (high serial) one.
	const std::vector<int32_t> handles{100, 0, -1, static_cast<int32_t>(0x80000001u), 200};
	const std::vector<std::string> carried =
		vip_items::CollectCarriedClassnames(handles, [](int32_t handle) -> std::string {
			if (handle == 100) {
				return "weapon_ak47";
			}
			if (handle == static_cast<int32_t>(0x80000001u)) {
				return "weapon_knife"; // negative, but a valid handle
			}
			if (handle == 200) {
				return {}; // the engine answered an empty classname
			}
			return "unexpected"; // 0 and -1 must never reach the classname lookup
		});

	CheckEqVec(carried, {"weapon_ak47", "weapon_knife"},
			   "only usable handles with a non-empty classname are carried");
}

void TestKv1Reader() {
	using vip_items::LoadKv1File;
	using vip_items::ParseKv1;

	// The production file, byte for byte.
	const std::string production =
		"\"VIP\"\n{\n    \"round_min\"     \"2\"     //Только со второго будут выданы шприцы\n}\n";
	const auto config = ParseKv1(production);
	Check(config.has_value(), "the production config parses");
	if (config) {
		CheckEqInt(config->GetInt("round_min"), 2, "round_min of the production config");
		CheckEqInt(config->GetInt("missing", 7), 7, "a missing key keeps the default");
	}

	// The same file from disk (tests/data/vip_items.ini, shipped with the sources).
	const std::string path = std::string(VIP_ITEMS_TEST_DATA_DIR) + "/vip_items.ini";
	const auto fileConfig = LoadKv1File(path);
	Check(fileConfig.has_value(), "tests/data/vip_items.ini loads");
	if (fileConfig) {
		CheckEqInt(fileConfig->GetInt("round_min"), 2, "round_min of the data file");
	}
	Check(!LoadKv1File(std::string(VIP_ITEMS_TEST_DATA_DIR) + "/does-not-exist.ini").has_value(),
		  "a missing config file is reported");

	// Key lookup is case-insensitive, like Valve's loader.
	const auto mixedCase = ParseKv1("\"VIP\" { \"Round_Min\" \"4\" }");
	Check(mixedCase.has_value(), "a mixed-case key parses");
	if (mixedCase) {
		CheckEqInt(mixedCase->GetInt("round_min"), 4, "key lookup is case-insensitive");
		CheckEqInt(mixedCase->GetInt("ROUND_MIN"), 4, "upper-case lookup is case-insensitive");
	}

	// Unquoted tokens.
	const auto unquoted = ParseKv1("VIP { round_min 5 }");
	Check(unquoted.has_value(), "unquoted tokens parse");
	if (unquoted) {
		CheckEqInt(unquoted->GetInt("round_min"), 5, "unquoted value");
	}

	// Comments: line comments after a value and block comments between the tokens.
	const auto comments = ParseKv1("\"VIP\"\n{\n  // the round gate\n"
								   "  \"round_min\" /* inline */ \"3\" /* after */\n}\n");
	Check(comments.has_value(), "comments are skipped");
	if (comments) {
		CheckEqInt(comments->GetInt("round_min"), 3, "the value behind the comments");
	}

	// #base / #include / #define are skipped, not included.
	const auto includes = ParseKv1("#include \"other.ini\"\n\"VIP\" { \"round_min\" \"6\" }\n");
	Check(includes.has_value(), "an #include line does not invalidate the file");
	if (includes) {
		CheckEqInt(includes->GetInt("round_min"), 6, "the value next to an #include line");
	}

	// A file without the single "Root" block: the keys live at the top level.
	const auto flat = ParseKv1("\"round_min\" \"7\"\n");
	Check(flat.has_value(), "a flat file parses");
	if (flat) {
		CheckEqInt(flat->GetInt("round_min"), 7, "a top-level key is found");
	}

	// Empty file: a valid, empty tree (pisexcompat's behaviour; the caller keeps its defaults).
	const auto empty = ParseKv1("");
	Check(empty.has_value(), "an empty file is a valid empty tree");
	if (empty) {
		CheckEqInt(empty->GetInt("round_min", 9), 9, "an empty tree keeps the default");
	}

	// Value parsing: whitespace trimmed, Valve's atoi fallback.
	const auto values = ParseKv1("VIP { \"round_min\" \" 5 \" \"junk\" \"abc\" \"leading\" \"12abc\" }");
	Check(values.has_value(), "the values file parses");
	if (values) {
		CheckEqInt(values->GetInt("round_min"), 5, "the value is trimmed");
		CheckEqInt(values->GetInt("junk"), 0, "a non-number reads as 0 (atoi)");
		CheckEqInt(values->GetInt("leading"), 12, "a leading number wins (atoi)");
		CheckEqInt(values->GetInt("absent", 8), 8, "a missing key keeps the default");
	}

	// Escapes inside quotes.
	const auto escapes = ParseKv1("\"VIP\" { \"note\" \"a\\\"b\\\\c\" }");
	Check(escapes.has_value(), "escaped quotes and backslashes parse");
	if (escapes) {
		CheckEqStr(escapes->GetString("note"), "a\"b\\c", "the escapes are decoded");
	}

	// Malformed files are rejected as a whole (no half-read tree).
	Check(!ParseKv1("\"VIP\" { \"round_min\" \"2\"").has_value(), "a missing '}' is rejected");
	Check(!ParseKv1("\"VIP\" { \"round_min\" \"2\" } }").has_value(), "a stray '}' is rejected");
	Check(!ParseKv1("\"VIP\" { \"round_min\" \"2 }").has_value(), "an unterminated string is rejected");
}

void TestConfigPath() {
	plg::GetConfigsDir = &FakeGetConfigsDir;

	g_fakeConfigsDir = "/test/configs";
	CheckEqStr(vip_items::ConfigPath(), "/test/configs/vip_items/vip_items.ini", "the config path");
	CheckEqStr(vip_items::ConfigPath("vip/x.ini"), "/test/configs/vip/x.ini", "an explicit relative path");
	g_fakeConfigsDir = "/test/configs/";
	CheckEqStr(vip_items::ConfigPath(), "/test/configs/vip_items/vip_items.ini",
			   "a trailing slash is not doubled");
	g_fakeConfigsDir.clear();
	Check(vip_items::ConfigPath().empty(), "an empty configs dir answers an empty path");
	plg::GetConfigsDir = nullptr;
	Check(vip_items::ConfigPath().empty(), "a missing configs dir answers an empty path");
}

// ==============================================================================================
// Part 2: the guards with fake s2sdk bindings
// ==============================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool alive = false;
	bool controllerPointer = false; // PlayerSlotToEntPointer() answers non-null
	bool controllerHandleValid = false;
	bool pawnPointer = false; // GetClientPawn() answers non-null
	bool pawnHandleValid = false;
	bool pawnPointerValid = false;
};

std::vector<FakeClient> g_clients(vip_items::kMaxSlots);

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_aliveCalls = 0;
int g_controllerPointerCalls = 0;
int g_controllerHandleCalls = 0;
int g_pawnCalls = 0;
int g_entPointerToHandleCalls = 0;
int g_validHandleCalls = 0;
int g_validPointerCalls = 0;
int g_schemaOffsetCalls = 0;

std::vector<int32_t> g_validHandles;

int32_t g_schemaOffsetResult = 72;
std::string g_lastSchemaClass;
std::string g_lastSchemaField;

FakeClient& Client(int32_t slot) {
	return g_clients[static_cast<std::size_t>(slot)];
}

int32_t ControllerHandle(int32_t slot) {
	return 1000 + slot;
}

int32_t PawnHandle(int32_t slot) {
	return 2000 + slot;
}

void* ControllerPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000 + slot));
}

void* PawnPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5000 + slot));
}

int32_t SlotFromPawnPointer(void* entity) {
	const std::uintptr_t base = 0x5000;
	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(entity);
	if (value < base || value >= base + static_cast<std::uintptr_t>(vip_items::kMaxSlots)) {
		return -1;
	}
	return static_cast<int32_t>(value - base);
}

bool IsRegisteredHandle(int32_t handle) {
	return std::find(g_validHandles.begin(), g_validHandles.end(), handle) != g_validHandles.end();
}

void AliveClient(int32_t slot) {
	FakeClient& client = Client(slot);
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.alive = true;
	client.controllerPointer = true;
	client.controllerHandleValid = true;
	client.pawnPointer = true;
	client.pawnHandleValid = true;
	client.pawnPointerValid = true;
	g_validHandles.push_back(ControllerHandle(slot));
	g_validHandles.push_back(PawnHandle(slot));
}

void ResetCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_aliveCalls = 0;
	g_controllerPointerCalls = 0;
	g_controllerHandleCalls = 0;
	g_pawnCalls = 0;
	g_entPointerToHandleCalls = 0;
	g_validHandleCalls = 0;
	g_validPointerCalls = 0;
	g_schemaOffsetCalls = 0;
	g_lastSchemaClass.clear();
	g_lastSchemaField.clear();
}

void ResetAll() {
	std::fill(g_clients.begin(), g_clients.end(), FakeClient{});
	ResetCounters();
	g_validHandles.clear();
	g_schemaOffsetResult = 72;
	g_logLines.clear();
	g_lastSeverity = plg::Severity::Unknown;
}

int ClientQueries() {
	return g_connectedCalls + g_inGameCalls + g_sourceTVCalls + g_aliveCalls + g_controllerPointerCalls +
		   g_controllerHandleCalls + g_pawnCalls + g_entPointerToHandleCalls + g_validHandleCalls +
		   g_validPointerCalls;
}

// --- fake s2sdk exports ------------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return Client(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_inGameCalls;
	return Client(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_sourceTVCalls;
	return Client(slot).sourceTV;
}

bool FakeIsClientAlive(int32_t slot) {
	++g_aliveCalls;
	return Client(slot).alive;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_controllerPointerCalls;
	return Client(slot).controllerPointer ? ControllerPointer(slot) : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_controllerHandleCalls;
	return Client(slot).controllerHandleValid ? ControllerHandle(slot) : 0;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_pawnCalls;
	return Client(slot).pawnPointer ? PawnPointer(slot) : nullptr;
}

int32_t FakeEntPointerToEntHandle(void* entity) {
	++g_entPointerToHandleCalls;
	const int32_t slot = SlotFromPawnPointer(entity);
	if (slot < 0) {
		return -1;
	}
	return Client(slot).pawnHandleValid ? PawnHandle(slot) : 0;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_validHandleCalls;
	return IsRegisteredHandle(handle);
}

bool FakeIsValidEntPointer(void* entity) {
	++g_validPointerCalls;
	const int32_t slot = SlotFromPawnPointer(entity);
	return slot >= 0 && Client(slot).pawnPointerValid;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaOffsetCalls;
	g_lastSchemaClass.assign(className.data(), className.size());
	g_lastSchemaField.assign(memberName.data(), memberName.size());
	return g_schemaOffsetResult;
}

// --- fake map listeners -------------------------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegistrations = 0;
int g_mapStartUnregistrations = 0;
int g_mapEndRegistrations = 0;
int g_mapEndUnregistrations = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegistrations;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregistrations;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegistrations;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregistrations;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

void InstallFakes() {
	plg::Log = &FakeLog;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndRegistrations = 0;
	g_mapEndUnregistrations = 0;
}

void OpenGate() {
	InstallFakes();
	ResetAll();
	vip_items::InitMapGate();
	vip_items::PollMapGate();
}

void CloseGate() {
	vip_items::ShutdownMapGate();
}

void TestMapGate() {
	// 1. Without the map-listener bindings the gate stays shut for good - even though the client
	// bindings exist (PITFALLS A1: log and stay inactive, never guess readiness).
	InstallFakes();
	ResetAll();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	vip_items::InitMapGate();
	vip_items::PollMapGate();
	Check(!vip_items::IsMapReady(), "without the map listeners the gate stays shut");
	Check(!g_logLines.empty(), "without the map listeners one error line is logged");
	Check(g_lastSeverity == plg::Severity::Error, "the missing-listener line is an error");
	AliveClient(0);
	Check(!vip_items::IsConnected(0), "without the map listeners IsConnected is false");
	Check(!vip_items::IsAlive(0), "without the map listeners IsAlive is false");
	CheckEqInt(ClientQueries(), 0, "without the map listeners no client query is made");
	CloseGate();

	// 2. Bindings available: OnPluginStart registers both listeners exactly once.
	InstallFakes();
	ResetAll();
	vip_items::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 1, "the OnMapStart listener is registered once");
	CheckEqInt(g_mapEndRegistrations, 1, "the OnMapEnd listener is registered once");
	vip_items::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 1, "a second InitMapGate does not register again");
	Check(!vip_items::IsMapReady(), "the gate is shut until a map start or the first update");

	// 3. A plugin loaded into a running map: the first update proves a live map.
	vip_items::PollMapGate();
	Check(vip_items::IsMapReady(), "the first update after OnPluginStart opens the gate");
	AliveClient(1);
	Check(vip_items::IsConnected(1), "a connected client passes IsConnected");
	Check(vip_items::IsAlive(1), "a live client with a controller and a pawn passes IsAlive");

	// Out-of-range slots: no s2sdk call at all.
	ResetCounters();
	Check(!vip_items::IsConnected(-1), "slot -1 is refused");
	Check(!vip_items::IsConnected(vip_items::kMaxSlots), "slot 64 is refused");
	Check(!vip_items::IsAlive(65), "slot 65 is refused");
	CheckEqInt(ClientQueries(), 0, "no client query is made for an out-of-range slot");

	// 4. OnMapEnd closes the gate with no client query at all.
	ResetCounters();
	FireMapEnd();
	Check(!vip_items::IsMapReady(), "OnMapEnd closes the gate");
	Check(!vip_items::IsConnected(1), "IsConnected is false in the map-end window");
	Check(!vip_items::IsAlive(1), "IsAlive is false in the map-end window");
	CheckEqInt(ClientQueries(), 0, "no client query is made in the map-end window");

	// 5. Updates keep coming through the changelevel: they must not re-open the gate.
	vip_items::PollMapGate();
	Check(!vip_items::IsMapReady(), "an update after OnMapEnd does not re-open the gate");

	// 6. The next map opens it again.
	FireMapStart();
	Check(vip_items::IsMapReady(), "the next OnMapStart opens the gate again");

	// 7. Shutdown unregisters both listeners, closes the gate and forgets them.
	vip_items::ShutdownMapGate();
	CheckEqInt(g_mapStartUnregistrations, 1, "the OnMapStart listener is removed once");
	CheckEqInt(g_mapEndUnregistrations, 1, "the OnMapEnd listener is removed once");
	Check(!vip_items::IsMapReady(), "ShutdownMapGate closes the gate");
	ResetCounters();
	Check(!vip_items::IsConnected(1), "after shutdown IsConnected is false");
	Check(!vip_items::IsAlive(1), "after shutdown IsAlive is false");
	CheckEqInt(ClientQueries(), 0, "after shutdown no client query is made");
	Check(g_mapStartCallback == nullptr, "the map-start callback is gone");
	Check(g_mapEndCallback == nullptr, "the map-end callback is gone");

	// 8. A reload may init again: the listeners are registered afresh.
	vip_items::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 2, "a reload registers the map listeners again");
	CheckEqInt(g_mapEndRegistrations, 2, "a reload registers the map-end listener again");
	CloseGate();
}

void TestIsAliveChain() {
	OpenGate();

	// The full chain passes.
	AliveClient(2);
	Check(vip_items::IsAlive(2), "a fully valid client is alive");

	// IsConnected is the cheap first layer: it is true as soon as the client is connected, no
	// matter what the rest of the state is.
	FakeClient& client = Client(2);
	client = FakeClient{};
	client.connected = true;
	Check(vip_items::IsConnected(2), "a connected client passes IsConnected without a pawn");
	Check(!vip_items::IsAlive(2), "a connected client without a pawn is not alive");

	// Each layer of IsAlive in turn (the rest of the state is valid).
	struct Layer {
		const char* what;
		void (*breakIt)(FakeClient&);
	};
	const Layer layers[] = {
		{"not connected", [](FakeClient& c) { c.connected = false; }},
		{"not in game", [](FakeClient& c) { c.inGame = false; }},
		{"SourceTV", [](FakeClient& c) { c.sourceTV = true; }},
		{"no controller pointer", [](FakeClient& c) { c.controllerPointer = false; }},
		{"invalid controller handle", [](FakeClient& c) { c.controllerHandleValid = false; }},
		{"no pawn pointer", [](FakeClient& c) { c.pawnPointer = false; }},
		{"invalid pawn handle", [](FakeClient& c) { c.pawnHandleValid = false; }},
		{"invalid pawn pointer", [](FakeClient& c) { c.pawnPointerValid = false; }},
		{"dead pawn", [](FakeClient& c) { c.alive = false; }},
	};
	for (const Layer& layer : layers) {
		AliveClient(2);
		layer.breakIt(Client(2));
		Check(!vip_items::IsAlive(2), std::string("IsAlive rejects a client with ") + layer.what);
	}

	// ... and the full chain still passes afterwards.
	AliveClient(2);
	Check(vip_items::IsAlive(2), "the restored client is alive again");

	CloseGate();
}

void TestSchemaOffset() {
	// 1. Gate closed: the lookup is never called and the answer is -1, even if the field was
	// cached by an earlier map (PITFALLS C8: no schema read in the loading windows).
	InstallFakes();
	ResetAll();
	vip_items::ClearSchemaOffsets();
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices"), -1,
			   "no schema offset before a map");
	CheckEqInt(g_schemaOffsetCalls, 0, "the schema lookup is not called before a map");

	// 2. A live map: the lookup runs, the answer is returned and cached.
	vip_items::InitMapGate();
	vip_items::PollMapGate();
	Check(vip_items::IsMapReady(), "the gate is open for the schema test");
	ResetCounters();
	g_schemaOffsetResult = 72;
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices"), 72,
			   "the weapon-services offset");
	CheckEqInt(g_schemaOffsetCalls, 1, "the first lookup calls s2sdk");
	CheckEqStr(g_lastSchemaClass, "CBasePlayerPawn", "the class name reaches s2sdk");
	CheckEqStr(g_lastSchemaField, "m_pWeaponServices", "the field name reaches s2sdk");
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices"), 72,
			   "the cached offset is returned");
	CheckEqInt(g_schemaOffsetCalls, 1, "a cached offset does not call s2sdk again");

	// 3. A second field is cached separately.
	g_schemaOffsetResult = 104;
	CheckEqInt(vip_items::SchemaOffset("CPlayer_WeaponServices", "m_iAmmo"), 104, "the ammo offset");
	CheckEqStr(g_lastSchemaField, "m_iAmmo", "the ammo field name reaches s2sdk");
	CheckEqInt(g_schemaOffsetCalls, 2, "the second field is looked up once");
	CheckEqInt(vip_items::SchemaOffset("CPlayer_WeaponServices", "m_iAmmo"), 104, "the cached ammo offset");
	CheckEqInt(g_schemaOffsetCalls, 2, "the cached ammo offset does not call s2sdk again");

	// 4. A failed lookup is retried, not poisoned for the session.
	g_schemaOffsetResult = -1;
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_unknown"), -1, "an unknown field answers -1");
	CheckEqInt(g_schemaOffsetCalls, 3, "the failed lookup called s2sdk");
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_unknown"), -1, "the unknown field is retried");
	CheckEqInt(g_schemaOffsetCalls, 4, "the failed lookup is retried instead of cached");
	g_schemaOffsetResult = 88;
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_unknown"), 88,
			   "the field is found once s2sdk answers it");
	CheckEqInt(g_schemaOffsetCalls, 5, "the successful retry is cached");

	// 5. A missing binding answers -1 without calling into s2sdk (a field that is not cached: a
	// cached offset is a plain number and stays valid without the binding).
	ResetCounters();
	__s2sdk_GetSchemaOffset = nullptr;
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_notCached"), -1,
			   "a missing schema binding answers -1");
	CheckEqInt(g_schemaOffsetCalls, 0, "a missing schema binding calls nothing");
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;

	// 6. ClearSchemaOffsets forgets everything (a reload resolves the fields again).
	vip_items::ClearSchemaOffsets();
	ResetCounters();
	g_schemaOffsetResult = 72;
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices"), 72, "a cleared cache looks up");
	CheckEqInt(g_schemaOffsetCalls, 1, "the cleared cache calls s2sdk once");

	// 7. Closing the map closes the schema gate again.
	FireMapEnd();
	ResetCounters();
	CheckEqInt(vip_items::SchemaOffset("CBasePlayerPawn", "m_pWeaponServices"), -1,
			   "no schema offset in the map-end window");
	CheckEqInt(g_schemaOffsetCalls, 0, "the schema lookup is not called in the map-end window");

	CloseGate();
	vip_items::ClearSchemaOffsets();
}

void TestSchemaCachePolicy() {
	// The cache on its own (no bindings at all): the map gate comes first, -1 is never cached and
	// Clear() forgets the successes.
	struct Lookup {
		static int32_t Call(const char* /*className*/, const char* /*fieldName*/) {
			++g_schemaOffsetCalls;
			return g_schemaOffsetResult;
		}
	};

	vip_items::SchemaOffsetCache cache;
	ResetCounters();
	g_schemaOffsetResult = 42;
	CheckEqInt(cache.Offset(false, "A", "b", &Lookup::Call), -1, "a closed gate answers -1");
	CheckEqInt(g_schemaOffsetCalls, 0, "a closed gate does not call the lookup");
	CheckEqInt(cache.CachedCount(), 0, "a closed gate caches nothing");

	CheckEqInt(cache.Offset(true, nullptr, "b", &Lookup::Call), -1, "a null class answers -1");
	CheckEqInt(cache.Offset(true, "A", nullptr, &Lookup::Call), -1, "a null field answers -1");
	CheckEqInt(cache.Offset(true, "", "b", &Lookup::Call), -1, "an empty class answers -1");
	CheckEqInt(cache.Offset(true, "A", "", &Lookup::Call), -1, "an empty field answers -1");
	CheckEqInt(cache.Offset(true, "A", "b", nullptr), -1, "a null lookup answers -1");
	CheckEqInt(g_schemaOffsetCalls, 0, "the invalid calls never reach the lookup");

	g_schemaOffsetResult = -1;
	CheckEqInt(cache.Offset(true, "A", "b", &Lookup::Call), -1, "a failed lookup answers -1");
	CheckEqInt(cache.CachedCount(), 0, "a failed lookup is not cached");
	CheckEqInt(cache.Offset(true, "A", "b", &Lookup::Call), -1, "the failed lookup is retried");
	CheckEqInt(g_schemaOffsetCalls, 2, "the failure was retried");

	g_schemaOffsetResult = 7;
	CheckEqInt(cache.Offset(true, "A", "b", &Lookup::Call), 7, "a successful lookup answers the offset");
	CheckEqInt(cache.CachedCount(), 1, "a successful lookup is cached");
	CheckEqInt(cache.Offset(true, "A", "b", &Lookup::Call), 7, "the cached offset is returned");
	CheckEqInt(g_schemaOffsetCalls, 3, "the cache hit does not call the lookup");

	cache.Clear();
	CheckEqInt(cache.CachedCount(), 0, "Clear() forgets the offsets");
	CheckEqInt(cache.Offset(true, "A", "b", &Lookup::Call), 7, "the cleared cache looks up again");
	CheckEqInt(g_schemaOffsetCalls, 4, "the cleared cache called the lookup");

	// Case sensitivity: the cache key is the exact class/field pair (s2sdk's own names).
	CheckEqInt(cache.Offset(true, "a", "b", &Lookup::Call), 7, "a different class is a different key");
	CheckEqInt(g_schemaOffsetCalls, 5, "the different class was looked up");
}

} // namespace

int main() {
	std::printf("vip_items tests\n");

	TestTokenizer();
	TestGrenadeAmmoIndex();
	TestRoundCondition();
	TestHandleFilter();
	TestIsCarried();
	TestCollectCarriedClassnames();
	TestKv1Reader();
	TestConfigPath();
	TestMapGate();
	TestIsAliveChain();
	TestSchemaOffset();
	TestSchemaCachePolicy();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
