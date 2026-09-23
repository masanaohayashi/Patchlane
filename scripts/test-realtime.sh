#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -dynamiclib Tests/DriverTests/RealtimeAudit.cpp -o .build/libRealtimeAudit.dylib -install_name @rpath/libRealtimeAudit.dylib
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -ISources/AudioCore/include Tests/DriverTests/RealtimeTests.cpp -L.build -lRealtimeAudit -Wl,-rpath,@executable_path -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -o .build/realtime-tests
.build/realtime-tests
for test in SharedOutputTests StereoSharedOutputTests; do
    xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -DPATCHLANE_REALTIME_AUDIT -ISources/AudioCore/include "Tests/DriverTests/$test.cpp" -L.build -lRealtimeAudit -Wl,-rpath,@executable_path -framework CoreAudio -framework CoreFoundation -framework Security -lbsm -o ".build/realtime-$test"
    ".build/realtime-$test"
done

xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -DPATCHLANE_REALTIME_AUDIT -IDriver Tests/DriverTests/DriverTests.cpp -L.build -lRealtimeAudit -Wl,-rpath,@executable_path -framework CoreAudio -framework CoreFoundation -o .build/realtime-driver-tests
.build/realtime-driver-tests "$PWD/build/Patchlane.driver"
.build/realtime-driver-tests "$PWD/build/Patchlane-2ch.driver" --stereo
