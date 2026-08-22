#!/usr/bin/env bash
# Fetch Meta OpenXR SDK and stitch our RoverUi/ into its samples tree so it builds.
# Idempotent — safe to re-run.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${REPO_DIR}/meta-openxr-sdk"
SDK_URL="https://github.com/meta-quest/Meta-OpenXR-SDK.git"

if [ ! -d "${SDK_DIR}" ]; then
  echo "[setup] cloning Meta OpenXR SDK into ${SDK_DIR}"
  git clone --depth 1 "${SDK_URL}" "${SDK_DIR}"
else
  echo "[setup] Meta SDK already present at ${SDK_DIR}"
fi

TARGET="${SDK_DIR}/Samples/XrSamples/RoverUi"
if [ -L "${TARGET}" ] || [ -d "${TARGET}" ]; then
  rm -rf "${TARGET}"
fi
ln -s "${REPO_DIR}/RoverUi" "${TARGET}"
echo "[setup] symlinked RoverUi into Meta SDK samples tree"

# Add RoverUi to Samples CMakeLists if not present
SAMPLES_CMAKE="${SDK_DIR}/Samples/XrSamples/CMakeLists.txt"
if ! grep -q "RoverUi" "${SAMPLES_CMAKE}"; then
  echo "add_subdirectory(RoverUi)" >> "${SAMPLES_CMAKE}"
  echo "[setup] added RoverUi to samples CMakeLists"
fi

echo
echo "[setup] done. To build:"
echo "  cd ${TARGET}/Projects/Android && ./gradlew assembleDebug"
echo "  APK will land at build/outputs/apk/debug/*.apk"
