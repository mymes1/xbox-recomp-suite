#!/usr/bin/env python3
"""Sonic Heroes XBE inspection and recompiler-config generation.

The checked-in ``ports/SonicHeroes/config/game.toml`` is deliberately a
template: it has no entry point or symbol table, because those depend on the
exact retail executable the builder supplies.  Run this tool on your own
``default.xbe`` to write a config that matches it:

    python3 tools/sonic_heroes.py ports/SonicHeroes/assets/default.xbe

It parses the XBE header directly (no game bytes are stored in the repository),
prints the title id, entry point, sections and XDK library builds, and writes
``ports/SonicHeroes/config/game.toml``.  If the image's library build has
checked-in XDK signatures, run ``xbe_symbols`` afterwards to fill in the
``[symbols]`` and ``[data]`` tables.  If it does not, the generated config is
still a valid starting point; identify the missing symbols from the log and add
them to the config as the port is brought up.

Running it again on the same executable rewrites ``[xbe]`` and keeps the
``[functions]``, ``[symbols]``, ``[data]`` and ``[abi]`` tables, so addresses
found by ``xbe_symbols`` or proven by hand survive a rebuild.

The tool never writes into ``assets/`` and never copies game data.
"""
from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "ports" / "SonicHeroes"
XDK_DIR = ROOT / "sdk" / "xdk"

RETAIL_ENTRY_KEY = 0xA8FC57AB
DEBUG_ENTRY_KEY = 0x94859D4B
RETAIL_THUNK_KEY = 0x5B6D40B6
DEBUG_THUNK_KEY = 0xEFB1F152

# Libraries that commonly own code sections in original Xbox titles.  The
# recompiler needs the section names that actually contain code; the list is
# also a useful checklist when a section is missing from the image.
KNOWN_CODE_SECTIONS = [
    ".text", "D3D", "D3DX", "XGRPH", "DSOUND", "XMV", "XPP", "WMADEC",
    "WMADECXM", "BINK", "BINK32", "BINK32A", "BINK16", "BINK4444",
    "BINK5551", "BINKYUY2", "BINK16MX", "BINK16X2", "BINK16M", "BINK32MX",
    "BINK32X2", "BINK32M", "DOLBY", "XAPILIB", "XONLINE", "XNET",
]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


class Xbe:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 0x180 or self.data[:4] != b"XBEH":
            raise ValueError(f"{path}: not an XBE image")
        d = self.data
        self.base = u32(d, 0x104)
        self.header_size = u32(d, 0x108)
        self.image_size = u32(d, 0x10C)
        self.cert = u32(d, 0x118)
        self.nsec = u32(d, 0x11C)
        self.sec_addr = u32(d, 0x120)
        self.init_flags = u32(d, 0x124)
        self.ep_raw = u32(d, 0x128)
        self.tls_addr = u32(d, 0x12C)
        self.stack_commit = u32(d, 0x130)
        self.heap_reserve = u32(d, 0x134)
        self.heap_commit = u32(d, 0x138)
        self.kt_raw = u32(d, 0x158)
        self.nlib = u32(d, 0x160)
        self.lib_addr = u32(d, 0x164)
        self.entry = 0
        self.kernel_thunk = 0
        self.debug = False
        ep = self.ep_raw ^ RETAIL_ENTRY_KEY
        kt = self.kt_raw ^ RETAIL_THUNK_KEY
        if self.inside(ep) and self.inside(kt):
            self.entry, self.kernel_thunk, self.debug = ep, kt, False
        else:
            ep = self.ep_raw ^ DEBUG_ENTRY_KEY
            kt = self.kt_raw ^ DEBUG_THUNK_KEY
            if self.inside(ep) and self.inside(kt):
                self.entry, self.kernel_thunk, self.debug = ep, kt, True
            else:
                raise ValueError(f"{path}: entry point key not recognised")
        self.sections = []
        for i in range(self.nsec):
            o = self.sec_addr - self.base + i * 56
            if o + 56 > len(d):
                break
            name_off = u32(d, o + 20)
            name = ""
            if self.base <= name_off < self.base + self.header_size:
                end = d.find(b"\0", name_off - self.base)
                if end >= 0:
                    name = d[name_off - self.base:end].decode("latin-1")
            self.sections.append({
                "name": name,
                "flags": u32(d, o),
                "va": u32(d, o + 4),
                "vsize": u32(d, o + 8),
                "raw": u32(d, o + 12),
                "rsize": u32(d, o + 16),
            })
        self.libraries = []
        for i in range(self.nlib):
            o = self.lib_addr - self.base + i * 16
            if o + 16 > len(d):
                break
            name = d[o:o + 8].split(b"\0")[0].decode("latin-1")
            self.libraries.append({
                "name": name,
                "major": u16(d, o + 8),
                "minor": u16(d, o + 10),
                "build": u16(d, o + 12),
                "flags": u16(d, o + 14),
            })
        self.title_id = 0
        if self.cert - self.base + 12 <= len(d):
            self.title_id = u32(d, self.cert - self.base + 8)

    def inside(self, va: int) -> bool:
        return self.base <= va < self.base + self.image_size

    def executable_sections(self) -> list[str]:
        names = []
        for s in self.sections:
            if not s["name"] or not (s["flags"] & 4):
                continue
            if s["name"] not in names:
                names.append(s["name"])
        # Some original Xbox images mark library code with a non-executable
        # flag but still contain the D3D/DSOUND/XGRPH thunks.  Keep the known
        # library sections that are present so the recompiler walks them.
        present = {s["name"] for s in self.sections}
        for name in KNOWN_CODE_SECTIONS:
            if name in present and name not in names:
                names.append(name)
        if not names:
            names = [".text"]
        return names

    def code_size(self) -> int:
        total = 0
        for s in self.sections:
            if s["name"] and (s["flags"] & 4 or s["name"] in KNOWN_CODE_SECTIONS):
                total += s["rsize"]
        return total or self.image_size

    def library_builds(self) -> list[int]:
        return sorted({lib["build"] for lib in self.libraries if lib["build"]})

    def choose_xdk(self) -> tuple[int, bool]:
        """Pick the closest checked-in XDK layout.

        Returns ``(build, exact)``.  An older image can usually be recompiled
        against a newer layout, but the struct offsets must be checked; the
        caller prints a warning when the build is not exact.
        """
        available = sorted(int(p.name) for p in XDK_DIR.iterdir() if p.is_dir() and p.name.isdigit())
        if not available:
            return 5849, False
        for build in self.library_builds():
            if build in available:
                return build, True
        # The largest D3D8/XAPILIB build wins: it is the closest layout.
        wanted = max(self.library_builds()) if self.library_builds() else 0
        for build in reversed(available):
            if wanted >= build:
                return build, False
        return available[0], False

    def partitions(self) -> int:
        # The recompiler's own heuristic is about 60 KB of code per partition,
        # clamped to 8..64.  Sonic Heroes is large; 64 keeps the compile units
        # small enough for a phone build and a CI runner.
        return max(8, min(64, self.code_size() // 60000 or 8))


def section_lines(text: str, name: str) -> list[str]:
    """The body of a top-level ``[name]`` table, comments and order kept."""
    body, inside = [], False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and not stripped.startswith("[["):
            inside = stripped == f"[{name}]"
            continue
        if inside:
            body.append(line)
    while body and not body[0].strip():
        body.pop(0)
    # blank lines and a comment after the last key introduce the next table, not this one
    while body and (not body[-1].strip() or body[-1].strip().startswith("#")):
        body.pop()
    return body


def has_entries(lines: list[str]) -> bool:
    """Whether a table body holds a key and not only comments."""
    return any(line.strip() and not line.strip().startswith("#") and "=" in line for line in lines)


def kept_tables(out: Path, title: int, entry: int) -> dict[str, list[str]]:
    """The hand-proven tables of an existing config for the same executable.

    Symbol addresses found by ``xbe_symbols`` or proven by hand are the
    expensive part of a bring-up, so a rebuild of the same XBE keeps
    ``[functions]``, ``[symbols]``, ``[data]`` and ``[abi]`` as they are.  A
    config written for another executable is replaced instead of merged.
    """
    try:
        text = out.read_text(encoding="utf-8")
    except OSError:
        return {}
    identity: dict[str, int] = {}
    for line in section_lines(text, "xbe"):
        for key in ("title", "entry"):
            match = re.match(rf"\s*{key}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", line)
            if match:
                identity[key] = int(match.group(1), 0)
    if identity.get("title") != title or identity.get("entry") != entry:
        return {}
    keep = {}
    for name in ("functions", "symbols", "data", "abi"):
        body = section_lines(text, name)
        # an empty [symbols] or [data] says nothing worth carrying over
        if name in ("functions", "abi") and body or has_entries(body):
            keep[name] = body
    return keep


def write_config(xbe: Xbe, out: Path, xdk: int | None = None) -> tuple[int, bool]:
    if xdk is None:
        xdk, exact = xbe.choose_xdk()
    else:
        exact = xdk in xbe.library_builds()
    sections = xbe.executable_sections()
    keep = kept_tables(out, xbe.title_id, xbe.entry)
    header = [
        "# Recompiler input for Sonic Heroes default.xbe, written by",
        "# tools/sonic_heroes.py from the builder's own executable.",
        "#",
        "# This file contains no game data.  [symbols] and [data] start empty: run",
        "# xbe_symbols when the image's XDK build has checked-in signatures, or add",
        "# the addresses as the port is brought up.  A later run of this tool on the",
        "# same executable rewrites [xbe] and keeps those tables.",
        "",
        "[xbe]",
        'path = "assets/default.xbe"',
        f"title = 0x{xbe.title_id:08X}",
        f"entry = 0x{xbe.entry:X}",
        f"xdk = {xdk}",
        'out = "generated/game"',
        f"partitions = {xbe.partitions()}",
        "code_sections = [" + ", ".join(f'"{s}"' for s in sections) + "]",
    ]
    tables = []
    for caption, name in (
            ("# extra entry points", "functions"),
            ("# functions the host replaces or calls by name, 0 where the image has none", "symbols"),
            (None, "data"),
            ("# argument locations, for a function that takes its API arguments in other than the declared places", "abi")):
        # [abi] is written by xbe_symbols or by hand, so it appears only when there is one to keep
        if name == "abi" and name not in keep:
            continue
        block = ([caption] if caption else []) + [f"[{name}]"]
        if name in keep:
            block.extend(keep[name])
        tables.append("\n".join(block))
    out.write_text("\n".join(header) + "\n\n" + "\n\n".join(tables) + "\n", encoding="utf-8")
    return xdk, exact


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbe", type=Path, help="your extracted Sonic Heroes default.xbe")
    parser.add_argument("--out", type=Path, default=PORT / "config" / "game.toml")
    parser.add_argument("--xdk", type=int, help="override the XDK layout build")
    parser.add_argument("--print", dest="do_print", action="store_true", help="also print the config")
    args = parser.parse_args()
    try:
        xbe = Xbe(args.xbe)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Sonic Heroes XBE: {args.xbe}")
    print(f"  title id:      0x{xbe.title_id:08X}")
    print(f"  entry:         0x{xbe.entry:X} (debug={xbe.debug})")
    print(f"  image:         0x{xbe.image_size:X} bytes, base 0x{xbe.base:X}")
    print(f"  sections:      {', '.join(s['name'] or '<unnamed>' for s in xbe.sections)}")
    libs = ", ".join(f"{lib['name']} {lib['major']}.{lib['minor']}.{lib['build']}" for lib in xbe.libraries)
    print(f"  libraries:     {libs or 'none'}")
    print(f"  code sections: {', '.join(xbe.executable_sections())}")
    xdk, exact = write_config(xbe, args.out, args.xdk)
    print(f"  XDK layout:    {xdk}{'' if exact else ' (closest available; verify struct offsets)'}")
    print(f"  config:        {args.out.relative_to(ROOT)}")
    if not exact:
        print("  note: the image's library build has no checked-in signatures.")
        print("        The config is valid, but [symbols] must be filled in before")
        print("        the port can call the host's D3D8/DSOUND/XInput implementations.")
    if args.do_print:
        print()
        print(args.out.read_text(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
