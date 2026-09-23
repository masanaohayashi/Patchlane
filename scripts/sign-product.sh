#!/bin/bash
set -euo pipefail
identity="${PATCHLANE_SIGN_IDENTITY:--}"
args=(--force --sign "$identity")
if [[ "$identity" != - ]]; then args+=(--timestamp --options runtime); fi
if [[ $# -gt 1 ]]; then args+=(--entitlements "$2"); fi
codesign "${args[@]}" "$1"
codesign --verify --strict "$1"
