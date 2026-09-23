#!/bin/bash
set -euo pipefail
# Target volume is explicit so tests can use an isolated fixture.
volume="${1:?Missing target volume}"
root="${volume%/}"
# Remove only the named product bundles, after checking their identities.
remove_bundle() {
    local path="$root$1" expected="$2" actual
    if [[ ! -e "$path" && ! -L "$path" ]]; then return; fi
    if [[ -L "$path" ]]; then
        /bin/echo "Refusing symlink: $path" >&2; exit 1
    fi
    actual=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$path/Contents/Info.plist")
    if [[ "$actual" != "$expected" ]]; then
        /bin/echo "Unexpected bundle identity: $path" >&2; exit 1
    fi
    /bin/rm -rf -- "$path"
}
remove_bundle /Applications/Patchlane.app local.Patchlane
remove_bundle /Library/Audio/Plug-Ins/HAL/Patchlane.driver audio.patchlane.driver
remove_bundle /Library/Audio/Plug-Ins/HAL/Patchlane-2ch.driver audio.patchlane.driver.stereo
/bin/rm -f -- "$root/Library/LaunchDaemons/audio.patchlane.ring-broker.plist" \
    "$root/Library/PrivilegedHelperTools/audio.patchlane.ring-broker"
# Unload the installed service and HAL after deleting their files.
# Preserve user preferences and saved mixer configurations.
for receipt in audio.patchlane.driver; do
    if /usr/sbin/pkgutil --volume "$volume" --pkg-info "$receipt" >/dev/null 2>&1; then
        /usr/sbin/pkgutil --volume "$volume" --forget "$receipt"
    fi
done
for relative in /Applications/Patchlane.app /Library/Audio/Plug-Ins/HAL/Patchlane.driver \
    /Library/Audio/Plug-Ins/HAL/Patchlane-2ch.driver \
    /Library/LaunchDaemons/audio.patchlane.ring-broker.plist \
    /Library/PrivilegedHelperTools/audio.patchlane.ring-broker; do
    if [[ -e "$root$relative" || -L "$root$relative" ]]; then
        /bin/echo "Removal incomplete: $relative" >&2; exit 1
    fi
done
if [[ "$volume" == / ]]; then
    for service in system/audio.patchlane.ring-broker; do
        if /bin/launchctl print "$service" >/dev/null 2>&1; then /bin/launchctl bootout "$service"; fi
    done
    if /usr/bin/pgrep -x coreaudiod >/dev/null; then
        /usr/bin/killall -TERM coreaudiod
    fi
    verifier="${2:?Missing device removal verifier}"
    "$verifier" --verify-driver-removed
fi
/bin/echo 'Patchlane removed.'
