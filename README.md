# openQ4-GameLibs

Quake 4 game libraries maintained for use with the openQ4 engine project.

## Overview
openQ4-GameLibs contains Quake4SDK-derived single-player and multiplayer game library code, maintained with a compatibility-first focus for modern development workflows.
This repository is the canonical home for SDK/game-library source used by the openQ4 workspace.

## Included
- Game library source code in `src/game` and `src/mpgame`
- Shared SDK-era interfaces used by Quake 4 style game modules
- Meson/Ninja build configuration for modern local builds

## Not Included
- Retail Quake 4 assets (`.pk4`, textures, audio, media)
- A standalone engine executable

## Build
### Windows (Meson + Ninja)
Meson/Ninja is the primary and only supported build workflow in this repository.

Requirements:
- Visual Studio C++ toolchain (`cl.exe`); the wrapper selects an x64 target by default on x64 hosts
- Meson and Ninja

1. Configure:
   `powershell -ExecutionPolicy Bypass -File tools/build/meson_setup.ps1 setup --wipe builddir . --backend ninja --buildtype release --vsenv`
2. Build:
   `powershell -ExecutionPolicy Bypass -File tools/build/meson_setup.ps1 compile -C builddir`
3. Outputs:
   `builddir/src/game-sp_x64.dll` and `builddir/src/game-mp_x64.dll` on x64 hosts

The x64 build uses the generic SIMD implementation; the legacy MMX/3DNow/SSE source files are x86 inline-assembly backends and are excluded from x64 builds.

From openQ4, you can invoke this same flow with:
`powershell -ExecutionPolicy Bypass -File tools/build/build_gamelibs.ps1`

### Useful Configure Options
- Build single-player only:
  `meson setup builddir -Dbuild_mpgame=false`
- Build multiplayer only:
  `meson setup builddir -Dbuild_spgame=false`
- Enable legacy DebugInline behavior:
  `meson setup builddir --buildtype debug -Dinline_debug=true`

## Integration
- Intended to pair with openQ4 engine builds.
- Requires user-supplied Quake 4 game data.
- Companion engine repository: `https://github.com/themuffinator/openQ4`
- Default local companion path: `..\openQ4` (sibling repo layout).
- openQ4 build wrappers can invoke this repository directly from `..\openQ4-GameLibs`.

## Project Goals
- Preserve original Quake 4 gameplay behavior.
- Maintain expected single-player and multiplayer parity.
- Improve long-term maintainability on modern systems.

## Credits
- Upstream Quake4SDK (Quake 4 v1.4.2 SDK baseline)
- id Software
- Raven Software
- openQ4 contributors

## License
This repository is licensed under the Quake 4 Software Development Kit Limited Use License Agreement (EULA), not the GNU GPL.

See `LICENSE` and `doc/legacy/EULA.Development Kit.rtf`.
