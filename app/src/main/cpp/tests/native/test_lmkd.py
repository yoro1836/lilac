#!/usr/bin/env python3
"""vine_lmkd against processes system_server would have told it about: over
its budget it kills the least important cached app and keeps the rest.

Usage: test_lmkd.py VINE_LMKD"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

lmkd_bin = sys.argv[1]
failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL", msg)


def ints(*v):
    return struct.pack("!%di" % len(v), *v)


def alive(proc):
    return proc.poll() is None


def recv_ints(sock):
    data = sock.recv(64)
    return struct.unpack("!%di" % (len(data) // 4), data)


tmp = tempfile.mkdtemp()
path = os.path.join(tmp, "lmkd")
# 1 MB: any app is over it, so only the adj rules decide
lmkd = subprocess.Popen([lmkd_bin, "--budget-mb", "1", path], stderr=subprocess.PIPE, text=True)
for _ in range(50):
    if os.path.exists(path):
        break
    time.sleep(0.05)

ams = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
ams.connect(path)
ams.send(ints(5, 0))  # LMK_SUBSCRIBE kill events
# Big enough private memory to tell which cached app is bigger
code = "import time; b = bytearray(%d); time.sleep(60)"
top = subprocess.Popen([sys.executable, "-c", code % (1 << 20)])
small = subprocess.Popen([sys.executable, "-c", code % (1 << 20)])
big = subprocess.Popen([sys.executable, "-c", code % (64 << 20)])
time.sleep(0.5)
ams.send(ints(1, top.pid, 10100, 0))       # LMK_PROCPRIO: foreground
ams.send(ints(1, small.pid, 10101, 950))   # cached
ams.send(ints(1, big.pid, 10102, 950))     # cached, bigger

ams.settimeout(5)
note = recv_ints(ams)
check(note[:2] == (6, big.pid), "the bigger cached app is killed first (%s)" % (note,))
note = recv_ints(ams)
check(note[:2] == (6, small.pid), "then the other cached app (%s)" % (note,))
time.sleep(1.5)
check(alive(top), "the foreground app is never killed for the budget")
check(not alive(big) or big.wait(1) is not None, "killed")

ams.send(ints(4, 0, 1000))  # LMK_GETKILLCNT
check(recv_ints(ams) == (4, 2), "kill count")
ams.send(ints(7))  # LMK_UPDATE_PROPS
check(recv_ints(ams) == (7, 0), "props reload answered")

for p in (top, small, big):
    p.kill()
    p.wait()
lmkd.kill()
err = lmkd.communicate()[1]
check("killed pid %d" % big.pid in err, "kills are logged")

print("%d checks, %d failed" % (checks, failures))
sys.exit(1 if failures else 0)
