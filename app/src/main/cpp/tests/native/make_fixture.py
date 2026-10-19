#!/usr/bin/env python3
"""Builds a fake ROM for the engine tests: ramdisk.img (gzip cpio), system.img (Android sparse) and vendor.img."""
import gzip
import os
import struct
import subprocess
import sys
import tempfile

MARKER = b"VINE_CONTAINER_INIT_V1"


def cpio_entry(ino, name, mode, data=b"", nlink=1, uid=0, gid=0):
    namez = name.encode() + b"\0"
    head = "070701" + "".join("%08X" % v for v in (
        ino, mode, uid, gid, nlink, 1700000000, len(data), 0, 0, 0, 0, len(namez), 0))
    blob = head.encode() + namez
    blob += b"\0" * ((4 - len(blob) % 4) % 4)
    blob += data
    blob += b"\0" * ((4 - len(data) % 4) % 4)
    return blob


def build_ramdisk(path, init_bytes):
    entries = []
    ino = 1

    def add(name, mode, data=b"", nlink=1):
        nonlocal ino
        entries.append(cpio_entry(ino, name, mode, data, nlink))
        ino += 1

    for d in ("proc", "sys", "dev", "system", "vendor", "data", "cache", "sbin", "etc"):
        add(d, 0o040755)
    add("init", 0o100755, init_bytes)
    add("default.prop", 0o100644, b"ro.secure=0\nro.debuggable=1\n")
    add("init.vine.rc", 0o100644, b"on early-init\n    start vine\n")
    add("sdcard", 0o120777, b"/storage/self/primary")
    entries.append(cpio_entry(0, "TRAILER!!!", 0))
    with gzip.open(path, "wb") as f:
        f.write(b"".join(entries))


def make_ext4(path, staging, size_mb):
    subprocess.run(["mke2fs", "-t", "ext4", "-F", "-q", "-b", "4096", "-d", staging, path, "%dM" % size_mb], check=True)


def to_android_sparse(raw_path, out_path, blk=4096):
    size = os.path.getsize(raw_path)
    assert size % blk == 0
    blocks = size // blk
    chunks = []  # (type, blocks, payload)
    with open(raw_path, "rb") as f:
        zero = b"\0" * blk
        run_type = None
        run = []
        for _ in range(blocks):
            data = f.read(blk)
            kind = "dc" if data == zero else "raw"
            if kind != run_type and run:
                chunks.append((run_type, run))
                run = []
            run_type = kind
            run.append(data)
        if run:
            chunks.append((run_type, run))
    out = bytearray()
    out += struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, blk, blocks, len(chunks), 0)
    for kind, run in chunks:
        if kind == "raw":
            out += struct.pack("<HHII", 0xCAC1, 0, len(run), 12 + len(run) * blk)
            out += b"".join(run)
        else:
            out += struct.pack("<HHII", 0xCAC3, 0, len(run), 12)
    with open(out_path, "wb") as f:
        f.write(out)


def main():
    out_dir, fake_init, fake_tool = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    init = open(fake_init, "rb").read()
    assert MARKER in init, "fake init lacks the marker"
    build_ramdisk(os.path.join(out_dir, "ramdisk.img"), init)
    build_ramdisk(os.path.join(out_dir, "ramdisk_unpatched.img"), init.replace(MARKER, b"XINE_CONTAINER_INIT_V1"))

    with tempfile.TemporaryDirectory() as tmp:
        sysdir = os.path.join(tmp, "system")
        os.makedirs(os.path.join(sysdir, "bin"))
        with open(os.path.join(sysdir, "marker.txt"), "w") as f:
            f.write("system-v1\n")
        with open(fake_tool, "rb") as src, open(os.path.join(sysdir, "bin", "fake_tool"), "wb") as dst:
            dst.write(src.read())
        os.chmod(os.path.join(sysdir, "bin", "fake_tool"), 0o755)
        raw = os.path.join(tmp, "system.raw")
        make_ext4(raw, sysdir, 16)
        to_android_sparse(raw, os.path.join(out_dir, "system.img"))

        vendor = os.path.join(tmp, "vendor")
        os.makedirs(vendor)
        with open(os.path.join(vendor, "marker.txt"), "w") as f:
            f.write("vendor-v1\n")
        make_ext4(os.path.join(out_dir, "vendor.img"), vendor, 8)


if __name__ == "__main__":
    main()
