#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build-driver.sh
mkdir -p .build
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror Tests/DriverTests/BrokerRoutingTests.cpp -framework Security -framework CoreFoundation -lbsm -o .build/broker-routing-tests
.build/broker-routing-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tools/kernel-response.cpp -o .build/kernel-response
.build/kernel-response
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tests/DriverTests/InterpolationTests.cpp -o .build/interpolation-tests
.build/interpolation-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tools/reader-quality.cpp -o .build/reader-quality
.build/reader-quality
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tests/DriverTests/TimedReaderTests.cpp -o .build/timed-reader-tests
.build/timed-reader-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -ISources/AudioCore/include Tests/DriverTests/SharedOutputTests.cpp \
  -framework CoreAudio -framework CoreFoundation -framework Security -lbsm -o .build/shared-output-tests
.build/shared-output-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -ISources/AudioCore/include Tests/DriverTests/StereoSharedOutputTests.cpp -framework CoreAudio -framework CoreFoundation -framework Security -lbsm -o .build/stereo-shared-output-tests
.build/stereo-shared-output-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IDriver Tests/DriverTests/SharedClockTests.cpp -o .build/shared-clock-tests
.build/shared-clock-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IDriver Tests/DriverTests/DriverTests.cpp \
  -framework CoreAudio -framework CoreFoundation -o .build/driver-tests
.build/driver-tests "$PWD/build/Patchlane.driver"
.build/driver-tests "$PWD/build/Patchlane-2ch.driver" --stereo

xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tests/DriverTests/SharedRingTests.cpp -o .build/shared-ring-tests
.build/shared-ring-tests

xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tests/DriverTests/MachRingTests.cpp -o .build/mach-ring-tests
.build/mach-ring-tests
