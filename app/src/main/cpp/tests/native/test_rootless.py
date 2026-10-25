#!/usr/bin/env python3
"""Drives vine_helper --rootless like the app does, against a fake Waydroid
image (mke2fs -d) and stand-ins for tawcroot, binderd, vine_wl and
vine_pulse. The base dir is deep enough that socket paths under it do not
fit in sun_path, like an app data dir.

Usage: test_rootless.py VINE_HELPER"""
import os
import queue
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

helper_bin = sys.argv[1]
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


# tawcroot: --init runs the fake init, anything else runs the command on the host
FAKE_TAWCROOT = r"""#!/bin/sh
here=${0%/*}
init=0; run=
while [ $# -gt 0 ]; do
  case $1 in
    --) shift; break ;;
    --init) init=1 ;;
    -r) shift ;;
    -b) shift; case $1 in *:/run/user/1000) run=${1%:/run/user/1000} ;; esac ;;
  esac
  shift
done
guest_path=$PATH
PATH=$PATH:/usr/bin:/bin
if [ $init = 1 ]; then
  echo "init PATH=$guest_path" >> "$run/../init-env.txt"
  # A daemon that outlives its parent, like the zygotes
  (sh -c 'echo $$ > "$1/../orphan.pid"; exec sleep 1000' sh "$run" &)
  if [ -e "$run/../init-crash" ]; then sleep 0.3; exit 3; fi
  sleep 0.5
  echo 1 > "$run/boot_completed"
  exec sleep 1000
fi
[ "$1" = /system/bin/sh ] && shift && set -- sh "$@"
PATH=$here/fakebin:$PATH exec "$@"
"""

FAKE_BINDERD = """#!/bin/sh
mkdir -p "$1" && : > "$1/ready" && exec sleep 1000
"""

FAKE_PULSE = """#!/bin/sh
exec sleep 1000
"""

# vine_wl: makes the vinefb file, records input lines next to it
FAKE_WL = r"""#!/usr/bin/env python3
import os, socket, sys
a = sys.argv[1:]
opt = {a[i]: a[i + 1] for i in range(0, len(a) - 1, 2)}
open(opt["--vinefb"], "wb").write(b"\0" * 4096)
open("wl-args.txt", "w").write(" ".join(a) + "\n")
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
if os.path.exists(opt["--input"]):
    os.unlink(opt["--input"])
s.bind(opt["--input"])
s.listen(4)
log = open("wl-lines.txt", "a", buffering=1)
while True:
    c, _ = s.accept()
    buf = b""
    while True:
        d = c.recv(4096)
        if not d:
            break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            log.write(line.decode() + "\n")
"""

FAKE_CMD = r"""#!/bin/sh
echo "cmd $*" >> "${0%/*}/../calls.txt"
case "$1 $2" in
  "package query-activities") printf 'com.android.settings/.Settings\ncom.example.app/com.example.app.Main\ncom.example.app/.Other\n' ;;
  "package resolve-activity") echo "priority=0"; echo "com.example.app/.Main" ;;
  "activity start-activity") echo "Starting: Intent { cmp=$4 }" ;;
esac
"""

FAKE_INPUT = r"""#!/bin/sh
echo "input $*" >> "${0%/*}/../calls.txt"
"""


def write_exec(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)


def make_image(src, img, size_mb):
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-d", src, img, f"{size_mb}M"], check=True,
                   stdout=subprocess.DEVNULL)


def build_images(top, inst, audio_hal_class=1):
    sysd = os.path.join(top, "sys")
    shutil.rmtree(sysd, ignore_errors=True)
    for d in ("system/bin", "system/etc/init/hw", "system/apex/com.android.art/bin", "system/product/etc", "apex", "dev",
              "vendor", "proc", "sys", "data"):
        os.makedirs(os.path.join(sysd, d), exist_ok=True)
    write_exec(os.path.join(sysd, "system/bin/init"), "#!/bin/sh\n")
    open(os.path.join(sysd, "system/build.prop"), "w").write(
        "ro.system.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi\n"
        "ro.system.product.cpu.abilist32=armeabi-v7a,armeabi\n"
        "ro.zygote=zygote64_32\nro.build.id=TEST\n")
    open(os.path.join(sysd, "system/etc/init/hw/init.zygote64_32.rc"), "w").write(
        "service zygote /system/bin/app_process64\n    class main\n\nservice zygote_secondary /system/bin/app_process32\n    class main\n")
    open(os.path.join(sysd, "init.environ.rc"), "w").write(
        "on early-init\n    export ANDROID_ROOT /system\n    export BOOTCLASSPATH /apex/a.jar:/apex/b.jar\n")
    vend = os.path.join(top, "vend")
    shutil.rmtree(vend, ignore_errors=True)
    os.makedirs(os.path.join(vend, "bin/hw"), exist_ok=True)
    elf = lambda cls: b"\x7fELF" + bytes([cls]) + b"\0" * 59  # ELFCLASS32 or 64
    for name, cls in (("android.hardware.audio.service", audio_hal_class), ("android.hardware.media.omx@1.0-service", 1),
                      ("android.hardware.light@2.0-service", 2)):
        with open(os.path.join(vend, "bin/hw", name), "wb") as f:
            f.write(elf(cls))
    with open(os.path.join(sysd, "system/bin/mediaserver"), "wb") as f:
        f.write(elf(1))
    open(os.path.join(sysd, "system/etc/init/mediaserver.rc"), "w").write("service media /system/bin/mediaserver\n    class main\n")
    open(os.path.join(sysd, "system/etc/init/init64.rc"), "w").write("service ok /system/bin/init\n    class main\n")
    os.makedirs(os.path.join(vend, "etc/init"), exist_ok=True)
    os.makedirs(os.path.join(vend, "etc/vintf/manifest"), exist_ok=True)
    for rc, svc, name in (("audio.rc", "vendor.audio-hal", "android.hardware.audio.service"),
                          ("omx.rc", "vendor.media.omx", "android.hardware.media.omx@1.0-service"),
                          ("light.rc", "vendor.light", "android.hardware.light@2.0-service")):
        open(os.path.join(vend, "etc/init", rc), "w").write(f"service {svc} /vendor/bin/hw/{name}\n    class hal\n")
    hal = lambda n: f"    <hal format=\"hidl\">\n        <name>{n}</name>\n    </hal>\n"
    open(os.path.join(vend, "etc/vintf/manifest.xml"), "w").write(
        "<manifest>\n" + hal("android.hardware.media.omx") + hal("android.hardware.light") + "</manifest>\n")
    open(os.path.join(vend, "etc/vintf/manifest/omx.xml"), "w").write("<manifest>\n" + hal("android.hardware.media.omx") + "</manifest>\n")
    open(os.path.join(vend, "build.prop"), "w").write("ro.vendor.product.cpu.abilist=arm64-v8a,armeabi-v7a\nro.vendor.x=1\n")
    os.makedirs(os.path.join(vend, "lib64/hw"))
    open(os.path.join(vend, "lib64/hw/gralloc.default.so"), "w").write("default gralloc\n")
    make_image(sysd, os.path.join(inst, "system.img"), 16)
    make_image(vend, os.path.join(inst, "vendor.img"), 8)


def build_libs(lib):
    os.makedirs(os.path.join(lib, "fakebin"))
    write_exec(os.path.join(lib, "libtawcroot.so"), FAKE_TAWCROOT)
    write_exec(os.path.join(lib, "libbinderd.so"), FAKE_BINDERD)
    write_exec(os.path.join(lib, "libvine_pulse.so"), FAKE_PULSE)
    write_exec(os.path.join(lib, "libvine_wl.so"), FAKE_WL)
    write_exec(os.path.join(lib, "libvine_lmkd.so"), "#!/bin/sh\n# lmkd\n")
    write_exec(os.path.join(lib, "libvine_gralloc.so"), "gralloc\n")
    write_exec(os.path.join(lib, "libvine_hal_service.so"), "#!/bin/sh\n# hal\n")
    write_exec(os.path.join(lib, "fakebin/cmd"), FAKE_CMD)
    write_exec(os.path.join(lib, "fakebin/input"), FAKE_INPUT)


class Helper:
    def __init__(self, base, lib, debug_socket):
        self.p = subprocess.Popen([helper_bin, "--rootless", "--base", base, "--libdir", lib, "--debug-socket", debug_socket],
                                  stdin=subprocess.PIPE,
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

    def call(self, cmd, *args, timeout=30):
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

    def wait_state(self, instance, state, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            try:
                ev = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if ev[0] == "state" and ev[1] == instance and ev[2] == state:
                return ev
        return None


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    with open(f"/proc/{pid}/stat") as f:
        return f.read().rsplit(")", 1)[1].split()[0] != "Z"


def procs_using(path):
    found = []
    for p in os.listdir("/proc"):
        if p.isdigit() and int(p) != os.getpid():
            try:
                with open(f"/proc/{p}/cmdline", "rb") as f:
                    if path.encode() in f.read():
                        found.append(int(p))
            except OSError:
                pass
    return found


def wait_for(cond, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def main():
    top = tempfile.mkdtemp(prefix="vine-rootless-")
    base = os.path.join(top, "d" * 40, "files", "instances")
    name = "0123abcd-4567-89ef-0123-456789abcdef"
    inst = os.path.join(base, name)
    lib = os.path.join(top, "lib")
    os.makedirs(inst)
    build_images(top, inst)
    build_libs(lib)
    W = os.path.join(inst, "rootless")
    R = os.path.join(W, "rootfs")
    check(len(os.path.join(W, "run/pulse/native")) > 108, "the socket paths are longer than sun_path")

    sock = os.path.join(top, "debug.sock")
    h = Helper(base, lib, sock)
    print("[hello]")
    ok, text = h.call("hello")
    check(ok and "engine=rootless" in text and f"uid={os.getuid()}" in text, "hello: " + text)
    ok, text = h.call("start", name, os.path.join(top, "elsewhere"))
    check(not ok and "outside" in text, "start outside base refused: " + text)

    print("[boot]")
    ok, text = h.call("start", name, inst, "width=640", "height=480", "dpi=240")
    check(ok, "start: " + text)
    check(h.wait_state(name, "running") is not None, "reaches running")
    ok, text = h.call("status", name)
    check(ok and "state=running" in text and "boot=1" in text, "status: " + text)
    ok, text = h.call("start", name, inst)
    check(not ok and "already" in text, "second start refused: " + text)

    print("[rootfs preparation]")
    prop = read(os.path.join(R, "system/build.prop"))
    check("ro.system.product.cpu.abilist=arm64-v8a\n" in prop and "ro.system.product.cpu.abilist32=\n" in prop and
          "ro.zygote=zygote64\n" in prop and "ro.build.id=TEST" in prop, "64-bit-only system props: " + prop)
    check("ro.vendor.product.cpu.abilist=arm64-v8a\n" in read(os.path.join(R, "vendor/build.prop")), "vendor props")
    z = read(os.path.join(R, "system/etc/init/hw/init.zygote64.rc"))
    check("service zygote " in z and "zygote_secondary" not in z, "zygote64 rc: " + z)
    rc_path = os.path.join(R, "system/product/etc/init/vine-rootless.rc")
    rc = read(rc_path)
    check("service lmkd /system/bin/vine_lmkd" in rc and "vine_hal_service" in rc and
          "write /run/user/1000/boot_completed 1" in rc, "vine-rootless.rc")
    check(os.path.exists(rc_path) and stat.S_IMODE(os.stat(rc_path).st_mode) == 0o644, "rc mode 644")
    check(os.access(os.path.join(R, "vendor/bin/hw/vine_hal_service"), os.X_OK), "hal service installed")
    check(os.access(os.path.join(R, "system/bin/vine_lmkd"), os.X_OK), "lmkd installed")
    check(read(os.path.join(R, "vendor/lib64/hw/gralloc.default.so")) == "gralloc\n" and
          read(os.path.join(R, "vendor/lib64/hw/gralloc.default_real.so")) == "default gralloc\n", "gralloc shim installed")
    check("ro.sf.lcd_density=240" in read(os.path.join(R, "vendor/waydroid.prop")), "waydroid.prop density")
    check("<op n=\"24\"" in read(os.path.join(W, "data/system/appops.xml")), "appops seeded")
    for d in ("data/misc/ethernet", "data/misc/apexdata/com.android.tethering/misc/ethernet"):
        with open(os.path.join(W, d, "ipconfig.txt"), "rb") as f:
            ipc = f.read()
        check(ipc.startswith(b"\0\0\0\3\0\x0cipAssignment\0\x06STATIC\0\x0blinkAddress\0\x0910.0.2.15\0\0\0\x18") and
              ipc.endswith(b"\0\x02id\0\x04eth0\0\x03eos"), "ethernet ipconfig: " + repr(ipc))
    check(os.readlink(os.path.join(R, "dev/binder")) == "binderfs/binder", "binder symlink")
    check(os.readlink(os.path.join(R, "apex/com.android.art")) == "/system/apex/com.android.art", "apex symlink")
    check(os.path.isdir(os.path.join(R, "dev/input")) and os.path.isfile(os.path.join(R, "dev/kmsg")), "dev dirs")
    check(os.path.getsize(os.path.join(W, "tawcroot/owners")) > 0, "ownership table")
    init_dir = os.path.join(R, "vendor/etc/init")
    check(sorted(os.listdir(init_dir)) == ["audio.rc", "light.rc"], "32-bit omx rc dropped: " + str(os.listdir(init_dir)))
    man = read(os.path.join(R, "vendor/etc/vintf/manifest.xml"))
    check("media.omx" not in man and "<name>android.hardware.light</name>" in man and man.count("<hal") == 1, "manifest: " + man)
    check("media.omx" not in read(os.path.join(R, "vendor/etc/vintf/manifest/omx.xml")), "manifest fragment")
    check("service statsd" in rc, "statsd disabled")
    sys_init = os.listdir(os.path.join(R, "system/etc/init"))
    check("mediaserver.rc" not in sys_init and "init64.rc" in sys_init, "32-bit system service dropped: " + str(sys_init))
    check("--create 640x480" in read(os.path.join(W, "wl-args.txt")), "display size")
    check(os.path.isfile(os.path.join(inst, "shared/vinefb")), "vinefb created")
    check("/system/bin" in read(os.path.join(W, "init-env.txt")), "init gets the guest PATH")

    print("[input, exec, apps, launch]")
    h.fire("agent_send", name, "touch 0 0 1 0 0.5 0.5")
    h.fire("agent_send", name, "key 4 1")
    h.fire("agent_send", name, "key 187 1")  # APP_SWITCH: no evdev code
    h.fire("agent_send", name, "key 187 0")
    wl = os.path.join(W, "wl-lines.txt")
    check(wait_for(lambda: "key 4 1" in read(wl)), "input lines reach vine_wl: " + read(wl))
    check("touch 0 0 1 0 0.5 0.5" in read(wl) and "187" not in read(wl), "touch passed, APP_SWITCH not")
    calls = os.path.join(lib, "calls.txt")
    check(wait_for(lambda: "input keyevent 187" in read(calls)), "APP_SWITCH through input: " + read(calls))
    check(read(calls).count("keyevent 187") == 1, "only on key down")
    ok, text = h.call("agent", name, "1500", "ping")
    check(ok, "agent ping: " + text)
    ok, text = h.call("exec", name, "5000", "echo", "a b", "c")
    check(ok and text == "exit=0\na b c\n", "exec: " + repr(text))
    ok, text = h.call("exec", name, "5000", "sh", "-c", "exit 7")
    check(ok and text.startswith("exit=7"), "exec exit code: " + repr(text))
    ok, text = h.call("exec", name, "300", "sleep", "5")
    check(ok and text.startswith("exit=-1"), "exec timeout: " + repr(text))
    ok, text = h.call("apps", name)
    check(ok and text == "com.android.settings\tcom.android.settings\ncom.example.app\tcom.example.app\n", "apps: " + repr(text))
    ok, text = h.call("launch", name, "com.example.app")
    check(ok, "launch: " + text)
    check("activity start-activity -n com.example.app/.Main" in read(calls), "launch component: " + read(calls))
    ok, text = h.call("launch", name, "bad;name")
    check(not ok, "bad package refused")
    r = subprocess.run([helper_bin, "--send", sock, "exec", name, "5000", "echo", "from adb"], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout == "exit=0\nfrom adb\n", "debug socket exec: " + repr(r.stdout + r.stderr))
    r = subprocess.run([helper_bin, "--send", sock, "launch", name, "bad;name"], capture_output=True, text=True)
    check(r.returncode == 1 and "invalid" in r.stdout, "debug socket error: " + repr(r.stdout))
    ok, text = h.call("diag", name)
    check(ok and "Engine: rootless" in text, "diag")

    print("[pause, stop]")
    orphan = int(read(os.path.join(W, "orphan.pid")).strip() or 0)
    check(orphan > 0 and alive(orphan), "orphaned guest running")
    ok, _ = h.call("pause", name)
    check(ok and h.wait_state(name, "paused") is not None, "pause")
    ok, _ = h.call("resume", name)
    check(ok and h.wait_state(name, "running") is not None, "resume")
    ok, text = h.call("stop", name)
    check(ok and text == "stopped", "stop: " + text)
    check(wait_for(lambda: not alive(orphan)), "orphaned guest killed")

    print("[restart keeps the rootfs, init crash]")
    marker = os.path.join(R, "system/kept")
    open(marker, "w").write("x")
    open(os.path.join(W, "init-crash"), "w").write("")
    ok, text = h.call("start", name, inst)
    check(ok, "restart: " + text)
    ev = h.wait_state(name, "error")
    check(ev is not None and "init exited with 3" in ev[3], "init crash reported: " + str(ev))
    check(os.path.exists(marker), "no second extraction")
    os.unlink(os.path.join(W, "init-crash"))

    print("[64-bit audio HAL kept]")
    name64 = "fedcba98-4567-89ef-0123-456789abcdef"
    inst64 = os.path.join(base, name64)
    os.makedirs(inst64)
    build_images(top, inst64, audio_hal_class=2)
    ok, text = h.call("start", name64, inst64)
    check(ok and h.wait_state(name64, "running") is not None, "64-bit image boots: " + text)
    R64 = os.path.join(inst64, "rootless/rootfs")
    check(not os.path.exists(os.path.join(R64, "vendor/bin/hw/vine_hal_service")), "no own audio HAL")
    check("vine_hal_service" not in read(os.path.join(R64, "system/product/etc/init/vine-rootless.rc")), "rc keeps the image's")
    ok, _ = h.call("stop", name64)
    check(ok, "stop the 64-bit instance")

    print("[quit]")
    ok, _ = h.call("start", name, inst)
    check(ok and h.wait_state(name, "running") is not None, "start again")
    orphan = int(read(os.path.join(W, "orphan.pid")).strip() or 0)
    h.call("quit")
    h.p.wait(timeout=20)
    check(h.p.returncode == 0, "quit exits 0")
    check(wait_for(lambda: not alive(orphan)), "quit stops the guests")
    check(wait_for(lambda: not procs_using(lib)), "no host processes left: " + str(procs_using(lib)))

    shutil.rmtree(top, ignore_errors=True)
    print(f"\n{checks} checks, {failures} failed")
    sys.exit(1 if failures else 0)


main()
