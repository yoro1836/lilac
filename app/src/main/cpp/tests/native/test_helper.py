#!/usr/bin/env python3
"""Drives the real vine_helper binary over stdin/stdout against the fake ROM, like the app does."""
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

fixture, helper_bin = sys.argv[1], sys.argv[2]
failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL", msg)


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"n": "\n", "r": "\r", "t": "\t"}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


class Helper:
    def __init__(self, base):
        self.p = subprocess.Popen([helper_bin, "--base", base, "--uid", "0"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.replies = {}
        self.events = queue.Queue()
        self.n = 0
        self.cv = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            f = line.rstrip("\n").split("\t")
            if f[0] == "*":
                self.events.put([f[1]] + [unesc(x) for x in f[2:]])
            else:
                with self.cv:
                    self.replies[f[0]] = (f[1] == "ok", unesc(f[2]) if len(f) > 2 else "")
                    self.cv.notify_all()

    def call(self, cmd, *args, timeout=60):
        self.n += 1
        rid = str(self.n)
        self.p.stdin.write("\t".join([rid, cmd] + [esc(a) for a in args]) + "\n")
        self.p.stdin.flush()
        end = time.time() + timeout
        with self.cv:
            while rid not in self.replies:
                if time.time() > end:
                    return False, "TIMEOUT"
                self.cv.wait(0.2)
            return self.replies.pop(rid)

    def fire(self, cmd, *args):
        self.p.stdin.write("\t".join(["-", cmd] + [esc(a) for a in args]) + "\n")
        self.p.stdin.flush()

    def wait_event(self, name, match=None, timeout=60):
        end = time.time() + timeout
        stash = []
        while time.time() < end:
            try:
                ev = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if ev[0] == name and (match is None or match(ev)):
                for s in stash:
                    self.events.put(s)
                return ev
            stash.append(ev)
        for s in stash:
            self.events.put(s)
        return None


class FakeAgent(threading.Thread):
    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.touches = []
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if os.path.exists(path):
            os.unlink(path)
        self.srv.bind(path)
        os.chmod(path, 0o666)
        self.srv.listen(2)

    def run(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        buf = b""
        while True:
            data = conn.recv(4096)
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode()
                cmd = text.split(" ")[0]
                if cmd == "touch":
                    self.touches.append(text)
                    continue
                if cmd == "apps":
                    conn.sendall(b"ok 2\ncom.example.alpha\tAlpha\ncom.example.beta\tBeta\n.\n")
                elif cmd == "launch":
                    conn.sendall(b"ok\n.\n" if text.endswith("com.example.alpha") else b"err no such package\n.\n")
                elif cmd == "icon":
                    conn.sendall(b"ok\niVBORw0KGgo=\n.\n")
                elif cmd == "ping":
                    conn.sendall(b"ok pong\n.\n")
                else:
                    conn.sendall(b"err unknown\n.\n")


def count_procs(name):
    n = 0
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                if name in open("/proc/%s/cmdline" % pid).read():
                    n += 1
            except OSError:
                pass
    return n


def main():
    base = tempfile.mkdtemp(prefix="vinehelper.")
    inst = os.path.join(base, "alpha")
    os.makedirs(inst)
    for f in ("ramdisk.img", "system.img", "vendor.img"):
        shutil.copy(os.path.join(fixture, f), inst)
    h = Helper(base)
    ev = h.wait_event("ready")
    check(ev is not None, "no ready event")

    ok, text = h.call("hello")
    check(ok and "proto=1" in text and "uid=0" in text, "hello: %r" % text)
    ok, text = h.call("probe")
    check(ok and "unshare=1" in text and "loop=1" in text, "probe: %r" % text)
    ok, text = h.call("bogus")
    check(not ok and "unknown command" in text, "unknown command: %r" % text)

    ok, text = h.call("start", "alpha", "/etc", "ram_mb=256")
    check(not ok and "outside" in text, "path outside base accepted: %r" % text)
    ok, text = h.call("start", "bad id!", inst)
    check(not ok, "bad id accepted")

    ok, text = h.call("start", "alpha", inst, "ram_mb=256", "cpu_cores=1", "data_size_mb=64", "width=720", "height=1280",
                      "dpi=320", "network=none", "boot_timeout=60")
    check(ok and "init_pid=" in text, "start: %r" % text)
    ev = h.wait_event("state", lambda e: e[1] == "alpha" and e[2] == "running", 30)
    check(ev is not None, "never saw a running event")
    ok, text = h.call("status", "alpha")
    check(ok and "state=running" in text and "boot=1" in text, "status: %r" % text)
    ok, text = h.call("list")
    check(ok and "alpha=running" in text, "list: %r" % text)
    ok, text = h.call("start", "alpha", inst)
    check(not ok and "already running" in text, "double start: %r" % text)

    ok, text = h.call("exec", "alpha", "5000", "/system/bin/fake_tool", "a b", "c")
    check(ok and text.startswith("exit=0\n") and "args=a b,c" in text and "marker=system-v1" in text, "exec: %r" % text)
    ok, text = h.call("exec", "nobody", "5000", "x")
    check(not ok, "exec on unknown instance")

    # agent features through a stand-in agent
    time.sleep(0.2)
    agent = FakeAgent(os.path.join(inst, "shared", "agent.sock"))
    agent.start()
    ok, text = h.call("agent", "alpha", "3000", "ping")
    check(ok and text.startswith("ok pong"), "agent ping: %r" % text)
    ok, text = h.call("apps", "alpha")
    check(ok and "com.example.alpha\tAlpha" in text and "com.example.beta\tBeta" in text and not text.startswith("ok"), "apps: %r" % text)
    ok, text = h.call("launch", "alpha", "com.example.alpha")
    check(ok, "launch: %r" % text)
    ok, text = h.call("launch", "alpha", "com.example.missing")
    check(not ok, "launching a missing package must fail: %r" % text)
    ok, text = h.call("launch", "alpha", "bad name;rm")
    check(not ok and "invalid package" in text, "package validation: %r" % text)
    ok, text = h.call("icon", "alpha", "com.example.alpha")
    check(ok and text.strip() == "iVBORw0KGgo=", "icon: %r" % text)
    for i in range(50):
        h.fire("agent_send", "alpha", "touch MOVE 0 1 0 0.5 %0.3f" % (i / 100.0))
    time.sleep(0.5)
    check(len(agent.touches) == 50 and agent.touches[0].startswith("touch MOVE"), "touches delivered: %d" % len(agent.touches))

    # snapshot while running (frozen copy), then restore after stopping
    ok, text = h.call("snapshot", "alpha", inst, "first")
    check(ok, "snapshot start: %r" % text)
    ev = h.wait_event("snapshot", lambda e: e[2] == "first", 60)
    check(ev is not None and ev[3] == "ok", "snapshot event: %r" % (ev,))
    ok, text = h.call("snapshots", inst)
    check(ok and text.startswith("first\t"), "snapshots: %r" % text)
    ok, text = h.call("status", "alpha")
    check("state=running" in text, "guest should be thawed after the snapshot: %r" % text)
    ok, text = h.call("restore", "alpha", inst, "first")
    check(not ok and "stop the instance" in text, "restore while running: %r" % text)

    ok, text = h.call("stop", "alpha", "5000")
    check(ok and "stopped" in text, "stop: %r" % text)
    ev = h.wait_event("state", lambda e: e[1] == "alpha" and e[2] == "stopped", 10)
    check(ev is not None, "no stopped event")
    ok, text = h.call("restore", "alpha", inst, "first")
    check(ok, "restore start: %r" % text)
    ev = h.wait_event("restore", lambda e: e[2] == "first", 60)
    check(ev is not None and ev[3] == "ok", "restore event: %r" % (ev,))
    ok, text = h.call("delete_snapshot", inst, "first")
    ok2, text2 = h.call("snapshots", inst)
    check(ok and ok2 and text2 == "", "snapshot not deleted: %r" % text2)
    ok, text = h.call("snapshot", "alpha", inst, "../evil")
    check(not ok, "bad snapshot name accepted")

    ok, text = h.call("rmtree", "/tmp")
    check(not ok and "outside" in text, "rmtree outside base: %r" % text)
    ok, text = h.call("rmtree", base)
    check(not ok, "rmtree of the base itself must be refused")

    # restart, then drop the pipe like a crashed app: guests must die with the helper
    ok, text = h.call("start", "alpha", inst, "ram_mb=256", "data_size_mb=64", "network=none")
    check(ok, "restart: %r" % text)
    h.wait_event("state", lambda e: e[1] == "alpha" and e[2] == "running", 30)
    check(count_procs("/init") >= 0, "sanity")
    before = count_procs(os.path.join(inst, "rootfs"))
    h.p.stdin.close()
    try:
        h.p.wait(timeout=15)
    except subprocess.TimeoutExpired:
        check(False, "helper did not exit after stdin closed")
        h.p.kill()
    time.sleep(0.5)
    ok = True
    for pid in os.listdir("/proc"):
        if pid.isdigit():
            try:
                if "fake_init" in os.readlink("/proc/%s/exe" % pid) or "/init" == os.readlink("/proc/%s/exe" % pid):
                    ok = False
            except OSError:
                pass
    check(ok, "a guest init survived the helper")
    ok_rm = True
    h2 = Helper(base)
    h2.wait_event("ready")
    ok_rm, text = h2.call("rmtree", inst)
    check(ok_rm and not os.path.exists(inst), "rmtree of an instance: %r" % text)
    h2.call("quit")
    h2.p.wait(timeout=10)
    shutil.rmtree(base, ignore_errors=True)
    print("\n%d checks, %d failed" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
