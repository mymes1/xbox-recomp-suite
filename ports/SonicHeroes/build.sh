
#!/usr/bin/env bash
set -Eeuo pipefail

GAME_TITLE="Sonic Heroes - XBE Recomp"
GAME_XBE="default.xbe"
GAME_EXE="SonicHeroes"

cd "$(dirname "$(realpath "$0")")"

PRESET="linux-release"
GENERATE_ONLY=0

for arg in "$@"; do
    case "$arg" in
        debug) PRESET="linux-debug" ;;
        --generate-only) GENERATE_ONLY=1 ;;
        *) echo "usage: $0 [debug] [--generate-only]"; exit 2 ;;
    esac
done

OUT="out/build/$PRESET"

fail() {
    echo
    echo "BUILD FAILED: $*"
    exit 1
}

trap 'rc=$?; echo "::error::build.sh failed at line $LINENO: $BASH_COMMAND (exit $rc)"; exit "$rc"' ERR

echo
echo "$GAME_TITLE"
echo "===================="

# Find default.xbe, including nested directories.
find_default_xbe() {
    find assets -type f -iname "$GAME_XBE" -print -quit 2>/dev/null || true
}

# If there is exactly one differently named XBE, use it as a fallback.
find_alternative_xbe() {
    mapfile -t candidates < <(
        find assets -type f -iname '*.xbe' -print 2>/dev/null
    )

    if [ "${#candidates[@]}" -eq 1 ]; then
        echo "${candidates[0]}"
        return 0
    fi

    if [ "${#candidates[@]}" -gt 1 ]; then
        echo "Multiple XBE files found:" >&2
        printf '  %s\n' "${candidates[@]}" >&2
    fi

    return 1
}

prepare_xbe() {
    local found=""

    found="$(find_default_xbe)"

    if [ -z "$found" ]; then
        found="$(find_alternative_xbe || true)"
    fi

    if [ -z "$found" ]; then
        return 1
    fi

    if [ "$found" != "assets/$GAME_XBE" ]; then
        echo "Found executable: $found"
        cp "$found" "assets/$GAME_XBE"
    fi

    test -s "assets/$GAME_XBE"
    echo "Executable ready: assets/$GAME_XBE"
}

# --------------------------------------------------
# 1. Toolchain
# --------------------------------------------------

echo "[1/5] Toolchain"

missing=()

for tool in clang++ cmake ninja git ld.lld pkg-config python3; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done

if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists sdl3 || missing+=("SDL3")
    pkg-config --exists vulkan || missing+=("Vulkan")
fi

if ((${#missing[@]})); then
    printf 'Missing tools: %s\n' "${missing[*]}"
    fail "Install the missing build dependencies."
fi

clang++ --version | head -1

# --------------------------------------------------
# 2. Game files
# --------------------------------------------------

echo "[2/5] Game files"

mkdir -p assets

if ! prepare_xbe; then

    echo "No XBE found in extracted files."

    iso="$(find assets -maxdepth 1 -type f -iname '*.iso' -print -quit)"

    if [ -z "$iso" ]; then
        echo
        echo "=== All XBE files ==="
        find assets -type f -iname '*.xbe' -print || true

        echo
        echo "=== Top-level game files ==="
        find assets -maxdepth 2 -type f -print | head -200

        echo
        echo "=== DVD directory ==="
        ls -lah assets/DVD/ 2>/dev/null || true

        fail "No default.xbe or ISO found. The extracted archive may be missing the Xbox executable."
    fi

    echo "Extracting ISO: $iso"

    python3 ../../tools/prepare.py SonicHeroes \
        || fail "SDK preparation failed."

    cmake --preset "$PRESET" \
        || fail "CMake configuration failed."

    cmake --build --preset "$PRESET" \
        --target iso_extract \
        -j "$(nproc)" \
        || fail "iso_extract compilation failed."

    test -x "$OUT/iso_extract" \
        || fail "iso_extract executable was not generated."

    "$OUT/iso_extract" --all "$iso" assets \
        || fail "ISO extraction failed."

    prepare_xbe \
        || {
            echo "Extracted files:"
            find assets -maxdepth 4 -type f | head -200
            fail "ISO extraction completed but no XBE was found."
        }
fi

# --------------------------------------------------
# 3. Recompiler configuration
# --------------------------------------------------

echo "[3/5] Recompiler config"

python3 ../../tools/prepare.py SonicHeroes \
    || fail "SDK preparation failed."

test -s "assets/$GAME_XBE" \
    || fail "Missing executable."

python3 ../../tools/sonic_heroes.py "assets/$GAME_XBE" \
    || fail "XBE header parsing failed."

# A [symbols] entry is a name and its address; comments alone are an empty table.
has_symbols() {
    awk '
        /^\[/ { inside = ($0 == "[symbols]"); next }
        inside && /^[[:space:]]*[A-Za-z_][A-Za-z_0-9]*[[:space:]]*=/ { found = 1 }
        END { exit !found }
    ' config/game.toml
}

if ! has_symbols; then

    cmake --preset "$PRESET" \
        || fail "CMake configuration failed."

    cmake --build --preset "$PRESET" \
        --target xbe_symbols \
        -j "$(nproc)" \
        || fail "xbe_symbols compilation failed."

    if [ -f "$OUT/xbe_symbols" ]; then
        SDK="$(head -n 1 "$OUT/sdk_dir.txt")"

        if "$OUT/xbe_symbols" \
            "assets/$GAME_XBE" "$SDK/xdk" \
            --out config/game.toml; then
            echo "Symbol generation completed."
        else
            echo
            echo "No signature set for this XBE's XDK library build."
            echo "The guest code still recompiles and the APK still builds: every XDK"
            echo "function and data address the host replaces is left 0, which becomes a"
            echo "trapping stub, so the game stops with \"not in this image\" or"
            echo "\"is unknown for this image, add it to the config\" on the first call it"
            echo "needs. Fill in config/game.toml [symbols] and [data] to go further:"
            echo "  - import addresses from XbSymbolDatabase (docs/XBE_PORTING_COOKBOOK.md),"
            echo "  - or add them by hand, and they survive the next run of this script."
            echo
            echo "::warning title=No XDK signatures for this XBE::config/game.toml [symbols] is empty, so the APK traps on every host XDK call until the addresses are filled in."
        fi
    else
        echo
        echo "::warning title=xbe_symbols was not built::config/game.toml [symbols] stays empty, so the APK traps on every host XDK call until the addresses are filled in."
    fi
fi

cmake --preset "$PRESET" \
    || fail "Final CMake configuration failed."

if grep -n '^[A-Za-z_0-9]* *= *\[.*"?"' config/game.toml; then
    fail "Unresolved ABI argument locations need manual correction."
fi

# --------------------------------------------------
# 4. Game code
# --------------------------------------------------

echo "[4/5] Game code"

if [ "$GENERATE_ONLY" = 1 ]; then

    cmake --build --preset "$PRESET" \
        --target recompile_SonicHeroes \
        -j "$(nproc)" \
        || fail "Guest code recompilation failed."

    echo "Guest code generation completed."
    exit 0
fi

cmake --build --preset "$PRESET" \
    --target SonicHeroes \
    -j "$(nproc)" \
    || fail "Sonic Heroes compilation failed."

# --------------------------------------------------
# 5. Tests
# --------------------------------------------------

echo "[5/5] Tests"

ctest --preset "$PRESET" --output-on-failure \
    || echo "WARNING: Some tests failed."

echo
echo "Build completed."
echo "$OUT/$GAME_EXE"
