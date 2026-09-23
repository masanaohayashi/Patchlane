#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build-broker.sh
bundle="$PWD/build/Patchlane.driver"
mkdir -p "$bundle/Contents/MacOS"
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=13.0 -fblocks -fvisibility=hidden \
  -Wall -Wextra -Werror -I.build -bundle Driver/PatchlaneDriver.cpp \
  -framework CoreAudio -framework CoreFoundation -framework Security -lbsm -o "$bundle/Contents/MacOS/PatchlaneDriver"
cp Driver/Info.plist "$bundle/Contents/Info.plist"
scripts/sign-product.sh "$bundle"
codesign --verify --strict "$bundle"
printf '%s\n' "$bundle"

# Separate HAL bundle and UID: stereo clients see exactly two channels.
bundle="$PWD/build/Patchlane-2ch.driver"
mkdir -p "$bundle/Contents/MacOS"
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=13.0 -fblocks -fvisibility=hidden \
  -Wall -Wextra -Werror -DLCD_CHANNELS=2 -I.build -bundle Driver/PatchlaneDriver.cpp \
  -framework CoreAudio -framework CoreFoundation -framework Security -lbsm -o "$bundle/Contents/MacOS/PatchlaneDriver"
cp Driver/Info-2ch.plist "$bundle/Contents/Info.plist"
scripts/sign-product.sh "$bundle"
codesign --verify --strict "$bundle"
printf '%s\n' "$bundle"
