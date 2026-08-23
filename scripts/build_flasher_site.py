#!/usr/bin/env python3
"""Assemble the web flasher site (GitHub Pages) from PlatformIO build outputs.

For each board it copies bootloader.bin, partitions.bin, boot_app0.bin and
firmware.bin from .pio/build/<env>/ into site/firmware/<board>/ and writes an
ESP Web Tools manifest (manifest-<board>.json) listing them at the standard
Arduino-ESP32 offsets (0x0 / 0x8000 / 0xe000 / 0x10000). The version comes
from platformio.ini's [crosspoint] version.

Usage:  python scripts/build_flasher_site.py --out site [--env lilygo=lilygo ...]
Boards default to lilygo/x4pro/x4 built from the envs lilygo / x4pro / default.
Boards whose build directory is missing are skipped (with a warning), so a
partial matrix still produces a usable site.
"""
import argparse
import configparser
import json
import shutil
import sys
from pathlib import Path

BOARDS = {
    # board id -> (pio env, chip family as ESP Web Tools names it)
    "lilygo": ("lilygo", "ESP32-S3"),
    "x4pro": ("x4pro", "ESP32-S3"),
    "x4": ("default", "ESP32-C3"),
}
PARTS = [("bootloader.bin", 0x0), ("partitions.bin", 0x8000), ("boot_app0.bin", 0xE000), ("firmware.bin", 0x10000)]


def version(repo: Path) -> str:
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(repo / "platformio.ini")
    return cfg.get("crosspoint", "version", fallback="dev")


def find_boot_app0(repo: Path, build: Path) -> Path | None:
    local = build / "boot_app0.bin"
    if local.exists():
        return local
    home = Path.home() / ".platformio" / "packages"
    for p in home.glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin"):
        return p
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--out", default="site")
    ap.add_argument("--env", action="append", default=[], help="board=env override, e.g. lilygo=lilygo-gh_release")
    args = ap.parse_args()
    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    envs = dict(BOARDS)
    for spec in args.env:
        board, env = spec.split("=", 1)
        envs[board] = (env, BOARDS[board][1])

    ver = version(repo)
    out.mkdir(parents=True, exist_ok=True)
    for f in (repo / "web-flasher").iterdir():
        if f.is_file():
            shutil.copy2(f, out / f.name)

    built = 0
    for board, (env, chip) in envs.items():
        build = repo / ".pio" / "build" / env
        if not (build / "firmware.bin").exists():
            print(f"warning: no build for {board} ({build}), skipping", file=sys.stderr)
            continue
        dest = out / "firmware" / board
        dest.mkdir(parents=True, exist_ok=True)
        parts = []
        for name, offset in PARTS:
            src = find_boot_app0(repo, build) if name == "boot_app0.bin" else build / name
            if src is None or not src.exists():
                print(f"error: {board}: missing {name}", file=sys.stderr)
                return 1
            shutil.copy2(src, dest / name)
            parts.append({"path": f"firmware/{board}/{name}", "offset": offset})
        manifest = {
            "name": f"Aurora ({board})",
            "version": ver,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": chip, "parts": parts}],
        }
        (out / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=2) + "\n")
        built += 1
        print(f"{board}: {env} -> {dest} ({chip})")
    print(f"site at {out}: version {ver}, {built} board(s)")
    return 0 if built else 1


if __name__ == "__main__":
    sys.exit(main())
