#!/usr/bin/env python3
"""Packs ramdisk.img, vendor.img (optional) and system.img into a .vrom for Wayondroid.

--kind waydroid packs a system-as-root image (no ramdisk, e.g. Waydroid's
system.img and vendor.img), which the app boots on the rootless engine."""

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
    for required in ("system.img",) if args.kind == "waydroid" else ("ramdisk.img", "system.img"):
        if required not in present:
            raise SystemExit(f"{src / required} is missing")
    manifest = {
        "kind": args.kind,
        "displayName": args.name,
        "androidVersion": args.android_version,
        "apiLevel": args.api_level,
        "description": args.description,
        "supportedAbis": args.abis,
        "sha256": {name: sha256_of(path) for name, path in present.items()},
    }
    out = Path(args.output)
    method = zipfile.ZIP_DEFLATED if args.deflate else zipfile.ZIP_STORED
    with zipfile.ZipFile(out, "w", method, allowZip64=True) as zf:
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
    p.add_argument("--kind", choices=("vine", "waydroid"), default="vine")
    # Half the size (a GitHub release asset stays under 2 GB); slower to unpack
    p.add_argument("--deflate", action="store_true")
    out = build(p.parse_args())
    print(f"Wrote {out} ({out.stat().st_size} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
