# Sonic Heroes on Android

Sonic Heroes is the first original-Xbox title in this suite with a touch
platformer overlay and a dedicated mobile performance profile. The repository
contains the port configuration, the launcher integration, the performance
governor and the CI workflow. It contains **no game data**: you supply your own
`default.xbe` / ISO.

> **Use your own legally obtained copy.** Do not upload retail game data to a
> public repository or a public release. Use a private repository and a private
> release, or a direct URL that only you can access.

## What is in the port

| Piece | Where |
| --- | --- |
| Recompiler config template | `ports/SonicHeroes/config/game.toml` |
| XBE config generator | `tools/sonic_heroes.py` |
| Linux build script | `ports/SonicHeroes/build.sh` |
| GameInfo and 60 FPS profile | `ports/SonicHeroes/src/common/game_info.cpp`, `src/game/performance.cpp` |
| Touch overlay | `android/app/src/main/java/org/xberecomp/PlatformerControls.java` |
| Virtual controller bridge | `android/app/src/main/java/org/xberecomp/TouchInput.java`, `android/app/src/main/cpp/touch_bridge.cpp` |
| Dynamic resolution + frame pacing | `sdk/runtime/app/performance.cpp`, `sdk/runtime/app/window_sdl.cpp` |
| CI workflow | `.github/workflows/sonic-heroes-android.yml` |
| OGlue fix mapping | `ports/SonicHeroes/patches/README.md` |

## Build the APK with GitHub Actions

1. **Create a private release with your game image.** On GitHub, open
   **Releases → Draft a new release**. Use the tag `game-data` (or any tag you
   like), title it “Sonic Heroes game data”, and attach your own
   `sonic-heroes.iso` (or a `.zip` containing `default.xbe` and its folders).
   Keep the release private to the repository. The asset name must match the
   workflow input `game_asset` (default `sonic-heroes.iso`).
2. **Open the workflow:**
   <https://github.com/Player124413/xbox-recomp-suite/actions/workflows/sonic-heroes-android.yml>
3. Click **Run workflow**. Keep `game_release_tag` at `game-data` and
   `game_asset` at `sonic-heroes.iso` unless you named them differently. Choose
   the render scale, frame cap and power mode you want baked into the first-run
   settings.
4. Wait for the run. The workflow extracts the ISO on the runner, recompiles
   `default.xbe`, builds the ARM64 APK and uploads it as the
   `SonicHeroes-arm64-debug` artifact. The ISO is not uploaded or packaged.
5. Download the artifact, unzip it, and install:
   ```sh
   adb install -r SonicHeroes-arm64-debug.apk
   adb shell am start -n com.sonicheroes.recomp/.LauncherActivity
   ```
6. In the launcher, choose your ISO or the extracted game folder once. The
   selection is remembered and the next launch boots directly. Game files stay
   on your SD card.

If the workflow cannot download the asset, it prints the exact release and
asset name it expected. The most common cause is a typo in the tag/asset name,
or the release being in a different repository. The workflow downloads from the
same repository with the default `GITHUB_TOKEN`. For a release in another
private repository, create a repository secret named `GAME_DATA_TOKEN` with a
fine-grained token that has `contents: read` on that repository; the workflow
uses it when present.

### Workflow inputs

| Input | Default | Meaning |
| --- | --- | --- |
| `game_release_tag` | `game-data` | Release that holds the game asset |
| `game_asset` | `sonic-heroes.iso` | Asset name; a `.zip` is unpacked |
| `game_iso_url` | empty | Direct URL to the ISO; overrides the release asset |
| `render_percent` | `75` | Initial internal render scale |
| `fps_cap` | `60` | Frame cap; `0` is unlimited |
| `power_mode` | `1` | `0` balanced, `1` performance (big cores), `2` powersave |
| `dynamic_resolution` | on | Governor trims resolution when a scene misses 60 FPS |
| `precise_pacing` | on | Absolute-clock pacing for the last 2 ms of a frame |

## Build locally

```sh
git clone https://github.com/Player124413/xbox-recomp-suite.git
cd xbox-recomp-suite
python3 tools/prepare.py SonicHeroes

# put your own extracted game in ports/SonicHeroes/assets, or an ISO there
ports/SonicHeroes/build.sh --generate-only   # recompile the guest code
python3 tools/build_android.py SonicHeroes --jobs 4
adb install -r dist/SonicHeroes-arm64-debug.apk
```

`build.sh` extracts an ISO found in `assets/`, writes
`config/game.toml` from your XBE with `tools/sonic_heroes.py`, tries
`xbe_symbols` if your image's XDK build has checked-in signatures, then
recompiles. The first recompile can take 30–90 minutes on a weak machine.

`config/game.toml` is yours to edit. Running the script again rewrites its
`[xbe]` table from the XBE and keeps `[functions]`, `[symbols]`, `[data]` and
`[abi]`, so addresses you add by hand are not thrown away.

The Linux desktop build is:

```sh
ports/SonicHeroes/build.sh
./ports/SonicHeroes/out/build/linux-release/SonicHeroes
```

## Controls

| Touch | Xbox | Action |
| --- | --- | --- |
| Left stick | Left stick | Move |
| Jump | A | Jump |
| Action | B | Action / attack |
| Special | X | Special |
| CamL / CamR | D-pad left/right | Camera |
| Start | Start | Start / pause |
| Settings | R3 / F10 | Settings (pauses the game) |

A physical Xbox/PlayStation/Switch controller works through SDL. R3 opens the
same settings panel. On desktop the keyboard is controller 1.

## Performance profile

The port starts with a 60 FPS cap, no vsync (immediate presentation), 75%
internal render scale, dynamic resolution from 50% to 85%, precise frame pacing
and the performance power mode. The render thread is pinned to the fastest
cores when the phone exposes `cpufreq` sysfs. All of it is editable in the
in-game settings panel; the governor writes changes to `settings.toml`.

- **Dynamic resolution** lowers the internal scale when the smoothed frame
  time misses the target and raises it when there is headroom. It changes at
  most every 1.8 s and never goes below the floor you set.
- **Precise pacing** sleeps on the absolute monotonic clock until the last 2 ms
  of the frame, then spins. This avoids the 1 ms timer granularity that makes a
  60 FPS cap look like 58–59 on some phone kernels.
- **Power mode** pins the render thread to the fastest or slowest cores. On a
  big.LITTLE phone, performance mode uses the big cluster; powersave uses the
  little cluster.
- **Frame cap** is 60 by default. Sonic Heroes is a 60 Hz game; do not raise it
  unless you are testing a 120 Hz panel and the game logic tolerates it.

If a scene still misses 60 FPS, lower the render scale to 66% or 50% and set
the resolution floor to 50%. If the phone runs hot, switch to powersave or
balanced and lower the ceiling to 75%.

## Validation status

The port scaffold, launcher, touch bridge, performance governor and CI
workflow are implemented and the source builds are checked by the repository
audit. The following are **not** claimed as device-verified:

- Booting a retail Sonic Heroes ISO on a phone.
- Full gameplay, level progression, sound and save compatibility.
- The exact XDK library build of your executable and the symbol table it needs.

The recompiler config is generated from your own XBE. If your image's XDK
build has no checked-in signatures, `xbe_symbols` cannot fill `[symbols]`, and
`build.sh` says so. The guest code still recompiles and the APK still builds:
the recompiler gives every XDK name the host binds a placeholder, so a call to
one the config does not name stops with `not in this image` (or `... is unknown
for this image, add it to the config`) instead of failing the build after the
whole Android toolchain has run. Fill in `[symbols]` and `[data]` to get past
those stubs, then check the log and the OGlue notes for the known Sonic Heroes
fixes. Treat the first boot as a bring-up, not a finished port.

## OGlue patches

`ports/SonicHeroes/patches/README.md` maps the Sonic Heroes fixes in
[GenryTheFox0/OGlue](https://github.com/GenryTheFox0/OGlue) to this suite:
pushbuffer execution, render-state handling, the APU/DirectSound idle-voice
path, the DISPATCH gate, and the 60 FPS present path. Some are implemented
directly in the shared runtime (frame pacing, present mode, render-target
recreation); others are documented as candidates for the port's `hooks.cpp`
because the suite's recompiled runtime has a different structure from OGlue's
emulator. No OGlue code is copied verbatim; the mapping explains what to port
and where.
