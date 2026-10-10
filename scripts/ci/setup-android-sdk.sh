#!/usr/bin/env bash
# Install the Android command-line tools and accept licenses so AGP can fetch the rest
set -euo pipefail

: "${ANDROID_HOME:?ANDROID_HOME must be set}"
CMDLINE_TOOLS_VERSION="${CMDLINE_TOOLS_VERSION:-13114758}"
CMAKE_VERSION="${CMAKE_VERSION:-3.22.1}"
tools_dir="${ANDROID_HOME}/cmdline-tools/latest"

if ! command -v unzip >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends curl unzip ca-certificates
fi

if [ ! -x "${tools_dir}/bin/sdkmanager" ]; then
  mkdir -p "${ANDROID_HOME}/cmdline-tools"
  archive="$(mktemp)"
  curl -fsSL -o "${archive}" "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS_VERSION}_latest.zip"
  unzip -q "${archive}" -d "${ANDROID_HOME}/cmdline-tools"
  mv "${ANDROID_HOME}/cmdline-tools/cmdline-tools" "${tools_dir}"
  rm -f "${archive}"
fi

export PATH="${tools_dir}/bin:${PATH}"

# yes exits with SIGPIPE once sdkmanager stops reading, which is expected
yes | sdkmanager --licenses >/dev/null || true
if [ ! -f "${ANDROID_HOME}/licenses/android-sdk-license" ]; then
  echo "ERROR: Android SDK licenses were not accepted" >&2
  exit 1
fi

sdkmanager --install "platform-tools" >/dev/null

if [ "${ANDROID_SDK_NATIVE:-false}" = "true" ]; then
  sdkmanager --install "cmake;${CMAKE_VERSION}" >/dev/null
  if [ -n "${ANDROID_NDK_VERSION:-}" ]; then
    sdkmanager --install "ndk;${ANDROID_NDK_VERSION}" >/dev/null
  fi
fi

echo "Android SDK ready at ${ANDROID_HOME}"
