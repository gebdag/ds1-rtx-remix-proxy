# Dark Souls: Prepare to Die Edition – RTX Remix compatibility proxy

A `d3d9.dll` proxy that makes *Dark Souls: Prepare to Die Edition* (PC, Steam) work with NVIDIA RTX Remix path tracing.
It sits between the game and the RTX Remix runtime and turns the game's shader-based Direct3D 9 rendering into fixed-function draws with proper transforms, materials and skinning, plus the game's lights, sun, camera, sky and atmosphere sent to Remix through its API.

Requires an owned copy of Dark Souls: Prepare to Die Edition.

## What it does

- **Draw conversion:** world, characters (skinned by Remix from the game's bone matrices), effects, water, sky and UI
  are converted to fixed-function draws compatible with RTX Remix. Repeated geometry is reused instead of re-sent every frame.
- **Lights:** the game's own point lights (map, torch, effect and character lights) become Remix lights with the
  game's falloff. One sun per map. Custom lights per map from `assets/lights`.
- **Look:** the game's haze, ambient, exposure, eye adaptation and colour grade are passed to the Remix runtime.
- **Frame rate:** the game's 30 fps limit is lifted (60 fps by default) and its time step follows the real frame
  length, so the game runs at normal speed.
- **Resolution:** the game's internal 1024×720 render targets follow the chosen display resolution.
- **Game builds:** the 2015-06-15 Steam build is supported. Address-based features look themselves up and switch off when they can't be found.
- **In-game menu (F4):** lights, HUD scale, dev tools etc.
- **Troubleshooting:** other potentially incompatible DLL mods in the game folder are listed in the log at startup and after a crash.
- **Texture packs:** DSfix-style texture packs in `DATA\tex_override` are supported.

## Requirements

- Dark Souls: Prepare to Die Edition (Steam).
- The Dark Souls build of the RTX Remix runtime and bridge.
- An NVIDIA RTX graphics card.
- To build: Visual Studio 2022 with the C++ x86 tools.

## Building

```bat
build.bat release --name DarkSouls
```

The DLL is written to `build\bin\release\d3d9.dll`. If a file named `deploy.conf` in the repo root holds the game's
`DATA` folder, `build.bat` also copies the DLL there; `--no-deploy` skips that.

## Installing

Copy `d3d9.dll` into the game's `DATA` folder next to `DARKSOULS.exe`, together with the RTX Remix runtime.
Settings are read from `rtx_comp\settings\remix-comp-proxy.ini`.

## Credits

Built on the remix-comp-proxy framework by Kim2091 and xoxor4d (from the Vibe Reverse Engineering toolkit), itself
built on xoxor4d's remix-comp-base. Third-party code and its licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT, see [LICENSE](LICENSE).
