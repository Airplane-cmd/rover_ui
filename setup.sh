#!/usr/bin/env bash
# Fetch Meta OpenXR SDK and rsync RoverUi/ into its samples tree.
# Idempotent — safe (and expected) to re-run after every source edit.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${REPO_DIR}/meta-openxr-sdk"
SDK_URL="https://github.com/meta-quest/Meta-OpenXR-SDK.git"

if [ ! -d "${SDK_DIR}" ]; then
  echo "[setup] cloning Meta OpenXR SDK into ${SDK_DIR}"
  git clone --depth 1 "${SDK_URL}" "${SDK_DIR}"
fi

TARGET="${SDK_DIR}/Samples/XrSamples/RoverUi"
if [ -L "${TARGET}" ]; then rm "${TARGET}"; fi
mkdir -p "${TARGET}"
rsync -a --delete --exclude build --exclude .cxx --exclude local.properties "${REPO_DIR}/RoverUi/" "${TARGET}/"
echo "[setup] synced RoverUi into Meta SDK samples tree"
echo "[setup] done. Build via: ./build.sh"
