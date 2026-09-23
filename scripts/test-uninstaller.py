#!/usr/bin/env python3
"""Exercise removal against a temporary target volume, never the live system."""
import plistlib
import subprocess
import tempfile
from pathlib import Path

script = Path(__file__).resolve().parent.parent / "Uninstaller/uninstall.sh"
with tempfile.TemporaryDirectory(prefix="patchlane-uninstall-test-") as directory:
    root = Path(directory)
    bundles = {
        "Applications/Patchlane.app": "local.Patchlane",
        "Library/Audio/Plug-Ins/HAL/Patchlane.driver": "audio.patchlane.driver",
        "Library/Audio/Plug-Ins/HAL/Patchlane-2ch.driver": "audio.patchlane.driver.stereo",
    }
    for relative, identifier in bundles.items():
        contents = root / relative / "Contents"
        contents.mkdir(parents=True)
        (contents / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": identifier}))
    helpers = ["Library/LaunchDaemons/audio.patchlane.ring-broker.plist",
               "Library/PrivilegedHelperTools/audio.patchlane.ring-broker"]
    preserved = ["Library/Audio/Plug-Ins/HAL/BlackHole.driver/keep",
                 "Users/test/Library/Preferences/local.Patchlane.plist"]
    for relative in helpers + preserved:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture")
    for _ in range(2):
        subprocess.run([str(script), str(root)], check=True)
        assert all(not (root / path).exists() for path in list(bundles) + helpers)
        assert all((root / path).read_text() == "fixture" for path in preserved)
    contents = root / "Applications/Patchlane.app/Contents"
    contents.mkdir(parents=True)
    (contents / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "other.app"}))
    result = subprocess.run([str(script), str(root)], capture_output=True)
    assert result.returncode != 0 and contents.exists()
print("PASS uninstall: product removal, repeated run, preferences/other drivers preserved, identity mismatch rejected")
