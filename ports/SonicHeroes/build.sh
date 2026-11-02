
#!/usr/bin/env bash
# Sonic Heroes Linux build.
#
#   ./build.sh                    Release build
#   ./build.sh debug              Debug build
#   ./build.sh --generate-only    Recompile guest code for Android
#
# Accepts:
#   assets/default.xbe
#   assets/DVD/default.xbe
#   assets/<any-subfolder>/default.xbe
#   assets/*.iso

set -euo pipefail

GAME_TITLE="Sonic Heroes - XBE Recomp"
GAME_XBE="default.xbe"
GAME_EXE="SonicHeroes"

cd "$(dirname "$(realpath "$0")")"

PRESET="linux-release"
GENERATE_ONLY=0

for arg in "$@"; do
    case "$arg" in
        debug)
            PRESET="linux-debug"
            ;;
        --generate-only)
            GENERATE_ONLY=1
            ;;
        *)
            echo "usage: $0 [debug] [--generate-only]"
            exit 2
            ;;
    esac
done

OUT="out/build/$PRESET"

echo
echo "  $GAME_TITLE"
echo "  =================="
echo

fail() {
    echo
    echo "  BUILD FAILED. $*"
    echo
    exit 1
}

# Find the game executable anywhere inside assets.
find_game_xbe() {
    if [ -s "assets/$GAME_XBE" ]; then
        printf '%s\n' "assets/$GAME_XBE"
        return 0
    fi

    find assets \
        -type f \
        -iname "$GAME_XBE" \
        -not -path "assets/.git/*" \
        -print 2>/dev/null | sort | head -n 1
}

# Place the executable where the recompiler expects it.
prepare_game_xbe() {
    local found_xbe

    found_xbe="$(find_game_xbe || true)"

    if [ -z "$found_xbe" ]; then
        return 1
    fi

    if [ "$found_xbe" != "assets/$GAME_XBE" ]; then
        echo "      found nested executable: $found_xbe"
        echo "      copying to assets/$GAME_XBE"

        cp "$found_xbe" "assets/$GAME_XBE"
    fi

    [ -s "assets/$GAME_XBE" ] || return 1

    echo "      executable ready: assets/$GAME_XBE"
    return 0
}

# ----------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------

echo "[1/5] Toolchain"

missing=()

for tool in clang++ cmake ninja git ld.lld pkg-config python3; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done

if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists sdl3 || missing+=("SDL3")
    pkg-config --exists vulkan || missing+=("Vulkan loader")
fi

if ((${#missing[@]})); then
    echo "      missing: ${missing[*]}"
    echo "      Install the missing build dependencies."
    fail "Required build tools are unavailable."
fi

echo "      $(clang++ --version | head -n 1)"

# ----------------------------------------------------------------
# Game files
# ----------------------------------------------------------------

echo "[2/5] Game files"

mkdir -p assets

# First check for an executable already extracted by GitHub Actions.
# This is important because the executable may be inside DVD/.
if ! prepare_game_xbe; then

    echo "      No extracted default.xbe found."

    # If no XBE exists, look for an ISO.
    iso="$(find assets -maxdepth 1 -type f -iname '*.iso' -print | sort | head -n 1)"

    if [ -z "$iso" ]; then
        echo "      Files currently inside assets:"
        find assets -maxdepth 3 -type f | head -100

        fail "No default.xbe or ISO found in $(realpath assets)."
    fi

    size="$(du -h "$iso" | cut -f1)"

    echo "      extracting $(basename "$iso") ($size)"
    echo "      extraction destination: assets/"

    # Prepare the SDK and extraction tools.
    python3 ../../tools/prepare.py SonicHeroes >/dev/null \
        || fail "SDK preparation failed."

    cmake --preset "$PRESET" \
        || fail "CMake could not configure."

    cmake --build "$PRESET" --target iso_extract \
        -j "$(nproc)" \
        || fail "iso_extract did not build."

    if [ ! -x "$OUT/iso_extract" ]; then
        fail "The ISO extraction executable was not created at $OUT/iso_extract."
    fi

    "$OUT/iso_extract" --all "$iso" assets \
        || fail "$(basename "$iso") could not be extracted."

    # The ISO extractor may place the XBE in DVD/ or another directory.
    prepare_game_xbe \
        || {
            echo "      Extracted files:"
            find assets -maxdepth 4 -type f | head -150
            fail "The extracted ISO contains no usable $GAME_XBE."
        }
fi

# ----------------------------------------------------------------
# Recompiler configuration
# ----------------------------------------------------------------

echo "[3/5] Recompiler config"

python3 ../../tools/prepare.py SonicHeroes \
    || fail "SDK preparation failed."

[ -s "assets/$GAME_XBE" ] \
    || fail "The game executable is missing or empty."

if [ ! -f config/game.toml ] || {
    grep -q '^entry = 0x10000$' config/game.toml &&
    ! grep -A2 '^\[symbols\]' config/game.toml | grep -q '='
}; then
    python3 ../../tools/sonic_heroes.py "assets/$GAME_XBE" \
        || fail "Could not read the XBE header."
else
    echo "      config/game.toml already written for this executable"
fi

# Try to identify library symbols when signatures are available.
if ! grep -A2 '^\[symbols\]' config/game.toml | grep -q '='; then

    cmake --preset "$PRESET" \
        || fail "CMake could not configure."

    cmake --build "$PRESET" --target xbe_symbols \
        -j "$(nproc)" \
        || fail "xbe_symbols did not build."

    if [ -f "$OUT/xbe_symbols" ]; then

        SDK="$(head -n 1 "$OUT/sdk_dir.txt")"

        echo "      trying xbe_symbols for $GAME_XBE"

        if "$OUT/xbe_symbols" \
            "assets/$GAME_XBE" \
            "$SDK/xdk" \
            --out config/game.toml; then

            echo "      symbols written"

        else
            echo "      no checked-in signatures for this XDK build."
            echo "      continuing with generated config."
        fi
    fi
fi

cmake --preset "$PRESET" \
    || fail "CMake could not configure."

# Check unresolved ABI argument locations.
if grep -n '^[A-Za-z_0-9]* *= *\[.*"?"' config/game.toml; then
    echo
    echo "      Unresolved [abi] argument locations detected."
    echo "      Fill these in according to the API comments."
    fail "The generated ABI configuration is incomplete."
fi

# ----------------------------------------------------------------
# Game code
# ----------------------------------------------------------------

echo "[4/5] Game code"

if [ "$GENERATE_ONLY" = 1 ]; then

    echo "      recompiling $GAME_XBE (guest code only)"

    cmake --build "$PRESET" \
        --target recompile_SonicHeroes \
        -j "$(nproc)" \
        || fail "The guest-code recompiler failed."

    echo
    echo "  Guest code generation completed."
    echo "  Next: python3 tools/build_android.py SonicHeroes"
    echo

    exit 0
fi

echo "      compiling $GAME_XBE"

cmake --build "$PRESET" \
    --target SonicHeroes \
    -j "$(nproc)" \
    || fail "The Sonic Heroes build failed."

# ----------------------------------------------------------------
# Tests
# ----------------------------------------------------------------

echo "[5/5] Tests"

ctest --preset "$PRESET" --output-on-failure \
    || echo "      WARNING: Some tests failed."

echo
echo "  $GAME_TITLE built."
echo "  $OUT/$GAME_EXE"
echo

if [ -t 0 ] && [ -z "${CI:-}" ]; then
    read -r -p "  Play now? [y/N] " answer

    if [[ "$answer" =~ ^[Yy] ]]; then
        cd "$OUT"
        exec "./$GAME_EXE"
    fi
fi

exit 0
