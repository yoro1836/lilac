#!/bin/bash
# Source from host scripts to get the on-device test/debug scratch dir.
# Sets TAWC_SCRATCH to the single canonical path and ensures it exists.
#
# TAWC_SCRATCH is the ONLY on-device location anything in this repo
# writes outside the app's private data dir. Test artefacts, adb-push
# staging, screenshots, firefox configs, tawcroot test binaries — all
# go here.
#
# This is for tests and debugging ONLY. Production (the APK at runtime)
# must never read or write here. Everything production needs ships in
# the APK and lives under /data/data/me.phie.tawc/ — that way `pm
# uninstall` removes every trace of the app.
#
# Why /data/local/tmp/tawc-dev/ specifically:
#   - /data/local/tmp/ is the only stock-Android location that is
#     simultaneously (a) writable by the adb `shell` uid without root,
#     (b) labeled with an SELinux context that allows execution of
#     pushed binaries, and (c) preserved across reboots. /sdcard is
#     noexec; /data/local/<other> needs root to mkdir; the app's
#     private dir isn't reachable from `adb push` without run-as or su.
#   - Namespacing under tawc-dev/ keeps us from polluting the shared
#     /data/local/tmp/ that other dev tooling on the same device may
#     also be using.
#
# Callers:
#   source scripts/lib/tawc-scratch.sh   # exports TAWC_SCRATCH, mkdir's it
#   adb push foo "$TAWC_SCRATCH/foo"
#
# Requires: adb on PATH, ANDROID_SERIAL already selected (e.g. by
# sourcing scripts/lib/select-device.sh first).

set -euo pipefail

export TAWC_SCRATCH=/data/local/tmp/tawc-dev
adb shell "mkdir -p $TAWC_SCRATCH" >/dev/null
