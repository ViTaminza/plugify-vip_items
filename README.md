# vip_items (Plugify)

VIP round-start items: every VIP with the 'items' feature (a space-separated weapon list in groups.ini) gets those weapons at spawn on the configured rounds; grenades are only added when the player has none of that type, other weapons only when not already carried. A port of the Metamod:Source module 'VIP_Items' of the Pisex cs2-vip plugin.

Plugify port (fork) of **VIP_Items** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_items-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_items`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

