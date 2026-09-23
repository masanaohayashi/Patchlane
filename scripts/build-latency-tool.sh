#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -ISources/AudioCore/include \
  Tools/loopback-latency.cpp Sources/AudioCore/AudioCore.cpp Sources/AudioCore/DeviceBridge.cpp \
  -framework CoreAudio -framework CoreFoundation -framework AudioToolbox -framework Security -lbsm -o .build/loopback-latency
