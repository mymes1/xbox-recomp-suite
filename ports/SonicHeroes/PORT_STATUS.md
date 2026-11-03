# Sonic Heroes port status

**Snapshot:** scaffold and bring-up tooling, not a device-verified port.

## Implemented

- `ports/SonicHeroes/` recompiler port with a generated-from-your-XBE config.
- `tools/sonic_heroes.py` XBE header parser and config writer (9 unit tests);
  a rebuild keeps the `[functions]`, `[symbols]`, `[data]` and `[abi]` tables.
- `ports/SonicHeroes/build.sh` ISO extraction, config generation, optional
  `xbe_symbols`, guest recompilation and Linux build.
- `GameInfo` with the 60 FPS mobile profile and the shared settings panel.
- Android launcher integration: `CONTROLS=platformer`, ISO/folder picker,
  remembered selection, dedicated `:game` process.
- Touch overlay: left analog stick, Jump (A), Action (B), Special (X),
  camera left/right, Start, Settings; virtual Xbox controller 0 in the runtime.
- Performance: 60 FPS cap, no vsync, 75% render scale, dynamic resolution
  50–85%, absolute-clock precise pacing, big-core render-thread pinning,
  inline DPC at DISPATCH_LEVEL, power modes, first-run `settings.toml`.
- GitHub Actions workflow that downloads your own ISO from a release asset or
  URL, recompiles, builds the ARM64 APK and uploads it.
- OGlue fix mapping and first-boot checklist in `patches/README.md`.

## Not verified

- Retail Sonic Heroes boot on a phone.
- Gameplay, level progression, sound, saves, cutscenes.
- The exact XDK library build of your executable. If it has no checked-in
  signatures, the `[symbols]` table is empty and host calls must be identified.
  The build compiles and packages in that state: the recompiler gives every XDK
  name a placeholder, so the first host call stops with a message naming the
  missing symbol instead of the build failing.
- The touch overlay's feel and camera mapping on a real device.
- Sustained 60 FPS and thermals on a specific phone.

## First boot

1. Build with the workflow or locally.
2. Install the APK and select your ISO/folder.
3. Capture logcat. If the game hangs on a loading screen, toggle **Inline DPC
   at DISPATCH_LEVEL**. If music stops after a minute, check the same setting
   and the thread suspend/resume path.
4. If giant triangles appear, add a draw/pushbuffer hook in `src/game/hooks.cpp`
   using the OGlue `GENRY_PB_FOLLOW` notes.
5. Tune the render scale, resolution floor and power mode in the settings
   panel. The governor writes `settings.toml`.

See [../../docs/SONIC_HEROES.md](../../docs/SONIC_HEROES.md) for the workflow
link and release setup.
