#pragma once
// vip_items — constants and helpers shared by the plugin and its local headers.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no guard
// library. Everything the module needs - the map gate, the client checks and the map-cached schema
// offsets - lives in guards.hpp, and the config reader in kv1.hpp.

#include <plg/plugin.hpp>

#include <string>
#include <string_view>

namespace vip_items {

// The log prefix the port has always used ("vip_items: ..."). Error lines keep the "[Error] "
// prefix pisexcompat's ErrorLog added.
inline constexpr char kLogTag[] = "vip_items";

// The feature of the original module, as groups.ini spells it: its value is the space-separated
// (quotes allowed) weapon list, e.g. "weapon_ak47 weapon_flashbang".
inline constexpr char kFeature[] = "items";

// vip_items.ini relative to <plg::GetConfigsDir()>: the original's
// addons/configs/vip/vip_items.ini, installed by plugins/vip_items/package.sh into the plugin's own
// config dir (PLUGIN_STANDARD §3).
inline constexpr char kConfigFile[] = "vip_items/vip_items.ini";

// The original's GetInt("round_min") fallback when the key is missing.
inline constexpr int kDefaultRoundMin = 0;

// <GetConfigsDir()>/<relative>; "" when the host provides no configs dir (tests). The plugify
// configs dir is addons/plugify/configs on the server.
inline std::string ConfigPath(std::string_view relative = kConfigFile) {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	std::string path(plg::GetConfigsDir());
	if (path.empty()) {
		return {};
	}
	if (path.back() != '/') {
		path.push_back('/');
	}
	path.append(relative);
	return path;
}

inline void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

} // namespace vip_items
