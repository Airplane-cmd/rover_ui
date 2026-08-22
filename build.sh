#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$REPO_DIR/setup.sh"
APROJ="$REPO_DIR/meta-openxr-sdk/Samples/XrSamples/RoverUi/Projects/Android"
cat > "$APROJ/local.properties" <<EOL
sdk.dir=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk.dir=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}
EOL
cd "$APROJ"
./gradlew assembleDebug "$@"
APK=$(find build/outputs/apk/debug -name '*.apk' | head -1)
echo
echo "Built: $APK"
