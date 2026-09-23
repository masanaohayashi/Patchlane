#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build build
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=13.0 -Wall -Wextra -Werror \
  Broker/RingBroker.cpp -framework Security -framework CoreFoundation -lbsm -o build/ring-broker
scripts/sign-product.sh build/ring-broker
# Embed the designated requirement: HAL cannot read a helper outside its bundle.
python3 - <<'PY'
import json,subprocess
p=subprocess.run(['codesign','-d','-r-','build/ring-broker'],capture_output=True,text=True,check=True)
output=[l.removeprefix('# ') for l in (p.stdout+'\n'+p.stderr).splitlines()]
lines=[l.removeprefix('designated => ') for l in output if l.startswith('designated => ')]
if len(lines)!=1:raise SystemExit('Cannot obtain broker designated requirement')
with open('.build/BrokerRequirement.hpp','w') as f:f.write('#pragma once\n#define LCD_BROKER_REQUIREMENT '+json.dumps(lines[0])+'\n')
PY
