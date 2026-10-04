#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
for variant in neon scalar; do
    flags=(-DPATCHLANE_DIPOLE_OPTIMIZED)
    if [ "$variant" = scalar ]; then flags=(-DPATCHLANE_DIPOLE_SCALAR); fi
    xcrun clang++ -std=c++17 -O3 -Wall -Wextra -Werror "${flags[@]}" Tests/DipoleTests/DSPTests.cpp -framework Accelerate -o ".build/dipole-$variant-tests"
    ".build/dipole-$variant-tests"
done
