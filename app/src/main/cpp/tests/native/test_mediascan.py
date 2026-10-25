#!/usr/bin/env python3
"""vine_mediascan against a directory tree: once writes settle it asks for
scans of the new files (by their upper path), of the directories files
left, and never of Android/.

Usage: test_mediascan.py VINE_MEDIASCAN"""
import os
import subprocess
import sys
import tempfile
import time

bin_path = sys.argv[1]
failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL", msg)


tmp = tempfile.mkdtemp()
lower = os.path.join(tmp, "media", "0")
for d in ("DCIM", "Download", "Android/data/app"):
    os.makedirs(os.path.join(lower, d))
open(os.path.join(lower, "Download", "old.txt"), "w").write("x")
log = os.path.join(tmp, "scans")
fake = os.path.join(tmp, "content")
with open(fake, "w") as f:
    f.write('#!/bin/sh\necho "$1 $2" >> %s\n' % log)
os.chmod(fake, 0o755)

env = dict(os.environ, VINE_MEDIASCAN_CMD=fake)
p = subprocess.Popen([bin_path, lower, "/storage/emulated/0"], env=env)
time.sleep(0.5)


def scans():
    return open(log).read().splitlines() if os.path.exists(log) else []


open(os.path.join(lower, "DCIM", "a.jpg"), "w").write("jpeg")
open(os.path.join(lower, "DCIM", "a.jpg"), "a").write("more")  # one scan for both writes
os.unlink(os.path.join(lower, "Download", "old.txt"))
os.makedirs(os.path.join(lower, "Music", "Album"))
time.sleep(0.2)
open(os.path.join(lower, "Music", "Album", "t.mp3"), "w").write("mp3")
open(os.path.join(lower, "Android", "data", "app", "cache.bin"), "w").write("private")
check(scans() == [], "nothing before things settle")
time.sleep(2.5)
got = scans()
check("scan_file /storage/emulated/0/DCIM/a.jpg" in got, "new file scanned: %s" % got)
check(got.count("scan_file /storage/emulated/0/DCIM/a.jpg") == 1, "once")
check("scan_file /storage/emulated/0/Download" in got, "a deletion rescans the directory")
check("scan_file /storage/emulated/0/Music/Album/t.mp3" in got, "files in new directories")
check(not any("Android" in s for s in got), "Android/ is not watched")

p.kill()
p.wait()
print("%d checks, %d failed" % (checks, failures))
sys.exit(1 if failures else 0)
