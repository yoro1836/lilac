#!/usr/bin/env bash
# Write release_body.md for the release pipeline, with a git-cliff changelog when available
set -euo pipefail

: "${RELEASE_VERSION_TAG:?RELEASE_VERSION_TAG must be set}"
GIT_CLIFF_VERSION="${GIT_CLIFF_VERSION:-2.14.2}"
output="${1:-release_body.md}"
workdir="$(mktemp -d)"
changelog=""

url="https://github.com/orhun/git-cliff/releases/download/v${GIT_CLIFF_VERSION}/git-cliff-${GIT_CLIFF_VERSION}-x86_64-unknown-linux-gnu.tar.gz"
if curl -fsSL "${url}" | tar -xz -C "${workdir}"; then
  cliff="$(find "${workdir}" -type f -name git-cliff | head -n 1)"
  changelog="$("${cliff}" --config cliff.toml --unreleased --tag "${RELEASE_VERSION_TAG}" --strip header | sed '/./,$!d' || true)"
else
  echo "git-cliff could not be downloaded, the release will have no generated changelog" >&2
fi

{
  if [ "${RELEASE_PRERELEASE:-true}" = "true" ]; then
    echo "> Pre-release: expect rough edges."
    echo
  fi
  if [ -n "${RELEASE_NOTES:-}" ]; then
    echo "${RELEASE_NOTES}"
    echo
  fi
  echo "The \`-debug\` APK is a debuggable build meant for troubleshooting only, install the regular one unless you specifically need it."
  if [ -n "${changelog}" ]; then
    echo
    echo "---"
    echo
    echo "${changelog}"
  fi
} > "${output}"

rm -rf "${workdir}"
echo "Wrote ${output}"
