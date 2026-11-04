#!/usr/bin/env python3
"""Validate the web installer manifests before publishing.

Shared verbatim by every app on this base. Two things fail silently if nobody looks:

  - The part offsets must stay the ones verified against the merged.bin of each board. If
    they drift, esptool writes the app over the partition table without a single error.
  - The version advertised in every manifest must be the FW_VERSION the firmware compiles.
    Otherwise the page offers a version it does not install.

Usage: tools/check-manifest.py [board ...]   (make check passes $(BOARDS))

The app is the only sketch under firmware/ (<name>/<name>.ino). Every docs/manifest*.json is
checked: each build must list bootloader, partitions, boot_app0 and <app> as <part>-<board>.bin
at their offsets, with the chip family of that board. ESP Web Tools picks a build by chip
family, so a chip appears at most once per manifest (two boards with the same chip need two
manifests), and every board given on the command line must be installable from one of them.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# C6 and S3 load the bootloader at 0x0, unlike the classic ESP32 (0x1000).
PARTS = [("bootloader", 0x0), ("partitions", 0x8000), ("boot_app0", 0xE000), (None, 0x10000)]
# Value of BOARD= in the Makefile -> chip family as ESP Web Tools names it.
CHIPS = {"c6": "ESP32-C6", "s3": "ESP32-S3", "round": "ESP32-S3", "amoled": "ESP32-S3"}


def find_app():
    sketches = [d.name for d in (ROOT / "firmware").iterdir() if (d / f"{d.name}.ino").is_file()]
    if len(sketches) != 1:
        raise SystemExit(f"expected one sketch under firmware/, found {len(sketches)}")
    return sketches[0]


def check(path, app, version, errors):
    rel = path.relative_to(ROOT)
    manifest = json.loads(path.read_text())
    if manifest.get("version") != version:
        errors.append(f"{rel}: version {manifest.get('version')} != FW_VERSION {version}")
    boards, chips = [], set()
    for build in manifest.get("builds", []):
        chip = build.get("chipFamily")
        if chip in chips:
            errors.append(f"{rel}: two builds for {chip}; use another manifest for the second board")
        chips.add(chip)
        parts = [(p.get("path"), p.get("offset")) for p in build.get("parts", [])]
        m = re.fullmatch(r"bootloader-([a-z0-9]+)\.bin", parts[0][0] or "") if parts else None
        board = m.group(1) if m else None
        if board not in CHIPS:
            errors.append(f"{rel} {chip}: cannot tell the board from {parts[:1]}")
            continue
        if CHIPS[board] != chip:
            errors.append(f"{rel}: board {board} is {CHIPS[board]}, not {chip}")
        want = [(f"{name or app}-{board}.bin", off) for name, off in PARTS]
        if parts != want:
            errors.append(f"{rel} {chip}: parts {parts} != {want}")
        boards.append(board)
    return boards


def main() -> int:
    app = find_app()
    config = (ROOT / "firmware" / app / "config.h").read_text()
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', config)
    if not m:
        print("manifest: FW_VERSION not found in config.h", file=sys.stderr)
        return 1
    version = m.group(1)

    errors, found = [], []
    manifests = sorted((ROOT / "docs").glob("manifest*.json"))
    if not manifests:
        errors.append("no docs/manifest*.json")
    for path in manifests:
        found += check(path, app, version, errors)
    for board in sorted(set(found)):
        if found.count(board) > 1:
            errors.append(f"board {board} is in more than one manifest")
    for board in sys.argv[1:]:
        if board not in found:
            errors.append(f"board {board} is not in any docs/manifest*.json")
    extra = sorted(set(found) - set(sys.argv[1:])) if sys.argv[1:] else []
    if extra:
        errors.append(f"manifests offer boards the Makefile does not build: {', '.join(extra)}")

    if errors:
        for e in errors:
            print(f"manifest: {e}", file=sys.stderr)
        return 1
    print(f"manifests ok: v{version}, boards {', '.join(found)} in {len(manifests)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
