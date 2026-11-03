# Sonic Heroes

Original Xbox Sonic Heroes, statically recompiled with the shared Xbox XBE
runtime and packaged for Android.

This directory contains the port configuration, build script, GameInfo and
mobile performance profile. It contains **no game data**.

## Quick start

```sh
# from the suite root
python3 tools/prepare.py SonicHeroes

# put your own extracted game in assets/, or an ISO there
ports/SonicHeroes/build.sh --generate-only
python3 tools/build_android.py SonicHeroes --jobs 4
adb install -r dist/SonicHeroes-arm64-debug.apk
```

`build.sh` writes `config/game.toml` from your `default.xbe` with
`tools/sonic_heroes.py`, tries `xbe_symbols` when your image's XDK build has
checked-in signatures, then recompiles. The checked-in `config/game.toml` is a
template with a placeholder entry point; do not build it without regenerating.
A rebuild keeps the `[functions]`, `[symbols]`, `[data]` and `[abi]` tables you
filled in, and prints a warning when `[symbols]` is still empty (the APK then
builds, but every host XDK call traps until the addresses are supplied).

## Controls

Touch: left stick, Jump (A), Action (B), Special (X), CamL/CamR, Start,
Settings. A physical controller works through SDL; R3 opens settings.

## Performance

The port starts at 60 FPS, no vsync, 75% internal render scale, dynamic
resolution 50–85%, precise pacing and the performance power mode. The render
thread is pinned to the fastest cores when `cpufreq` sysfs is available. All
settings are editable in-game and persist to `settings.toml`.

## Status and OGlue mapping

See [PORT_STATUS.md](PORT_STATUS.md) and [patches/README.md](patches/README.md).
The full workflow/release instructions are in
[../../docs/SONIC_HEROES.md](../../docs/SONIC_HEROES.md).
