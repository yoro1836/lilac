#!/usr/bin/env python3
"""ui.py tap TEXT | dump: drives the phone's UI over adb (uiautomator)"""
import re, subprocess, sys
import os
ADB = os.path.join(os.environ.get("ANDROID_HOME", os.path.dirname(os.path.abspath(__file__)) + "/../../build/remote-tools/sdk"), "platform-tools/adb")
def sh(cmd): return subprocess.run([ADB, "shell", cmd], capture_output=True, text=True).stdout
def nodes():
    sh("uiautomator dump /data/local/tmp/tawc-dev/ui.xml >/dev/null 2>&1")
    xml = sh("cat /data/local/tmp/tawc-dev/ui.xml")
    for m in re.finditer(r'<node [^>]*>', xml):
        n = m.group(0)
        t = re.search(r' text="([^"]*)"', n).group(1)
        d = re.search(r'content-desc="([^"]*)"', n).group(1)
        b = re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', n)
        yield t, d, tuple(int(x) for x in b.groups())
cmd = sys.argv[1]
if cmd == "dump":
    for t, d, b in nodes():
        if t or d: print(repr(t), repr(d), b)
elif cmd == "tap":
    for t, d, (x1, y1, x2, y2) in nodes():
        if sys.argv[2] in (t, d):
            sh(f"input tap {(x1+x2)//2} {(y1+y2)//2}"); print("tapped", t or d); break
    else: print("not found"); sys.exit(1)
