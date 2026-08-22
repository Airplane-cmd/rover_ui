# rover_ui

Custom OpenXR launcher for Meta Quest 3.

Goal: head-locked panels + passthrough for stationary use (bed / couch / walking).
Bypasses Meta Horizon Shell's world-anchored panel behavior by rendering our own
head-locked composition layers.

## Status

v0.0 — scaffolding based on Meta's XrPassthrough OpenXR sample. Not yet a launcher; renders the sample's original test scene.

Roadmap:
- v0.1: replace sample scene with 1 head-locked textured quad + controllers visible
- v0.2: WebView panel (browse in bed)
- v0.3: multi-panel layout, controller ray-cast interaction
- v0.4: app launcher grid (launch other Quest apps from within)
- v1.0: polish, save/restore layouts, media player, daily-driver ready

## Build

Requires:
- Android SDK + NDK 27 (installed at path pointed to by \`local.properties\` \`ndk.dir=\`)
- \`ANDROID_HOME\` env var pointing to the SDK
- Java 17+

Steps:
\`\`\`bash
# 1) Fetch Meta OpenXR SDK and stitch RoverUi/ into its samples tree
./setup.sh

# 2) Build the debug APK
cd meta-openxr-sdk/Samples/XrSamples/RoverUi/Projects/Android
./gradlew assembleDebug

# APK lands at:
#   meta-openxr-sdk/Samples/XrSamples/RoverUi/Projects/Android/build/outputs/apk/debug/*.apk
\`\`\`

## Install on Quest

\`\`\`bash
adb install -r meta-openxr-sdk/Samples/XrSamples/RoverUi/Projects/Android/build/outputs/apk/debug/roverui-arm64-v8a-debug.apk
\`\`\`

Launch from Bloom → Unknown Sources → "Rover UI" (or launch via \`monkey -p com.gantrping.rover 1\`).

## Architecture

- \`RoverUi/Src/\` — native C++ OpenXR loop (starting from Meta's XrPassthrough sample)
- \`RoverUi/java/com/oculus/NativeActivity.java\` — Java entry stub
- \`RoverUi/Projects/Android/\` — Gradle + AndroidManifest
- \`RoverUi/CMakeLists.txt\` — CMake build for native lib \`roverui.so\`
- \`meta-openxr-sdk/\` (not committed) — Meta's OpenXR SDK, fetched by setup.sh, provides \`SampleXrFramework\` and OpenXR loader AAR

## License

Our code (RoverUi/**): MIT. See [LICENSE](LICENSE).

Meta's OpenXR SDK (fetched by setup.sh) remains under the Oculus SDK License Agreement:
https://developer.oculus.com/licenses/oculussdk/


## Companion: wake_fix.sh

If you run rover_ui with Guardian killed (walkable / no boundary), pressing Quest's power button to sleep + waking will break vrshell UI (side-effect of the "sweet-spot" state). `scripts/wake_fix.sh` is a background daemon that auto-recovers.

Install (once, on Quest):
```bash
adb push scripts/wake_fix.sh /data/local/tmp/wake_fix.sh
# via Termux runner wrapper (see quest-termux-claude memory):
adb shell "/data/local/tmp/quest_termux_run.sh cp /data/local/tmp/wake_fix.sh /data/data/com.termux/files/home/wake_fix.sh

## Companion: wake_fix.sh

If you run rover_ui with Guardian killed (walkable / no boundary), pressing Quest's power button to sleep + waking will break vrshell UI (side-effect of the "sweet-spot" state). `scripts/wake_fix.sh` is a background daemon that auto-recovers.

Install (once, on Quest):

    # push the script into Quest's temp
    adb push scripts/wake_fix.sh /data/local/tmp/wake_fix.sh

    # from adb-shell (root grants via Magisk pre-approved for termux uid),
    # copy into Termux home and wire up Termux:Boot autostart
    adb shell 'su 10159 -c "cp /data/local/tmp/wake_fix.sh ~/wake_fix.sh && chmod +x ~/wake_fix.sh && mkdir -p ~/.termux/boot && printf %s\n \"#!/data/data/com.termux/files/usr/bin/bash\" \"exec ~/wake_fix.sh\" > ~/.termux/boot/wake-fix && chmod +x ~/.termux/boot/wake-fix"'

Then reboot Quest once. Termux:Boot APK required (from F-Droid).
