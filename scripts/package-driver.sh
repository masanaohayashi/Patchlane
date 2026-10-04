#!/bin/bash
set -euo pipefail
export COPYFILE_DISABLE=1
cd "$(dirname "$0")/.."
bash scripts/build-app.sh
scripts/test-driver.sh
scripts/build-uninstaller.sh
staging=$(mktemp -d "$PWD/.build/driver-package.XXXXXX")
components=$(mktemp -d "$PWD/.build/package-components.XXXXXX")
trap 'rm -rf "$staging" "$components"' EXIT
mkdir -p "$staging/Applications" "$staging/Library/Audio/Plug-Ins/HAL" "$staging/Library/PrivilegedHelperTools" "$staging/Library/LaunchDaemons"
cp -R build/Patchlane.app "$staging/Applications/"
cp -R build/Patchlane.driver build/Patchlane-2ch.driver "$staging/Library/Audio/Plug-Ins/HAL/"
cp build/ring-broker "$staging/Library/PrivilegedHelperTools/audio.patchlane.ring-broker"
cp Broker/audio.patchlane.ring-broker.plist "$staging/Library/LaunchDaemons/"
chmod 755 "$staging/Library/PrivilegedHelperTools/audio.patchlane.ring-broker"
chmod 644 "$staging/Library/LaunchDaemons/audio.patchlane.ring-broker.plist"
pkgbuild --analyze --root "$staging" "$components/bundles.plist"
python3 - "$components/bundles.plist" <<'PYCONFIG'
import plistlib,sys
p=sys.argv[1]
with open(p,'rb') as f: bundles=plistlib.load(f)
for bundle in bundles:
    bundle['BundleIsRelocatable']=False
    bundle['BundleOverwriteAction']='upgrade'
with open(p,'wb') as f: plistlib.dump(bundles,f)
PYCONFIG
pkgbuild --root "$staging" --component-plist "$components/bundles.plist" --scripts "$PWD/scripts/driver-package" \
  --identifier audio.patchlane.driver --version 0.4.1 --ownership recommended \
  --install-location / "$components/Driver.pkg"
installer_sign=()
if [[ -n "${PATCHLANE_INSTALLER_IDENTITY:-}" ]]; then installer_sign=(--sign "$PATCHLANE_INSTALLER_IDENTITY" --timestamp); fi
productbuild "${installer_sign[@]}" --distribution scripts/install-distribution.xml --resources scripts/package-resources \
  --package-path "$components" "$PWD/build/Patchlane-Installer.pkg"
pkgutil --payload-files "$PWD/build/Patchlane-Installer.pkg"
