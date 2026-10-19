#!/usr/bin/env python3
"""Packs ramdisk.img, vendor.img (optional) and system.img into a .vrom for VineOS."""

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

PAYLOAD = ("ramdisk.img", "vendor.img", "system.img")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def build(args: argparse.Namespace) -> Path:
    src = Path(args.images)
    present = {name: src / name for name in PAYLOAD if (src / name).is_file()}
    for required in ("ramdisk.img", "system.img"):
        if required not in present:
            raise SystemExit(f"{src / required} is missing")
    manifest = {
        "displayName": args.name,
        "androidVersion": args.android_version,
        "apiLevel": args.api_level,
        "description": args.description,
        "supportedAbis": args.abis,
        "sha256": {name: sha256_of(path) for name, path in present.items()},
    }
    out = Path(args.output)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        zf.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        for name, path in present.items():
            zf.write(path, name)
    return out


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("images", help="folder holding ramdisk.img, system.img and optionally vendor.img")
    p.add_argument("output", help="the .vrom file to write")
    p.add_argument("--name", required=True)
    p.add_argument("--android-version", required=True)
    p.add_argument("--api-level", required=True, type=int)
    p.add_argument("--abis", nargs="+", default=["arm64-v8a"])
    p.add_argument("--description", default="")
    out = build(p.parse_args())
    print(f"Wrote {out} ({out.stat().st_size} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
