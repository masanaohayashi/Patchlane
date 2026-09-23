#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror Broker/RingBroker.cpp -framework Security -framework CoreFoundation -lbsm -o .build/ring-broker
codesign --force --sign - .build/ring-broker
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror Tests/DriverTests/BrokerIdentityTests.cpp -framework Security -framework CoreFoundation -lbsm -o .build/broker-identity-tests
codesign --force --sign - .build/broker-identity-tests
.build/broker-identity-tests
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Werror -IShared Tests/DriverTests/BrokerRejectionTests.cpp -o .build/broker-rejection-tests
python3 - <<'PY'
import os,plistlib,subprocess
path=os.path.abspath('.build/test-broker.plist')
with open(path,'wb') as f:plistlib.dump(dict(Label='audio.patchlane.ring-broker-test',ProgramArguments=[os.path.abspath('.build/ring-broker')],MachServices={'audio.patchlane.ring-broker':True},StandardErrorPath=os.path.abspath('.build/broker-test.log')),f)
domain='gui/'+str(os.getuid())
subprocess.run(['launchctl','bootstrap',domain,path],check=True)
try:
 for _ in range(2):subprocess.run(['.build/broker-rejection-tests'],check=True)
except:
 subprocess.run(['launchctl','print',domain+'/audio.patchlane.ring-broker-test'])
 raise
finally:subprocess.run(['launchctl','bootout',domain+'/audio.patchlane.ring-broker-test'],check=True)
PY
