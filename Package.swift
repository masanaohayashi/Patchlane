// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "Patchlane", defaultLocalization: "en", platforms: [.macOS(.v13)], products: [.executable(name: "Patchlane", targets: ["Patchlane"])], targets: [
    .target(name: "AudioCore", publicHeadersPath: "include", cxxSettings: [.unsafeFlags(["-std=c++17"])], linkerSettings: [.linkedFramework("AudioToolbox"), .linkedFramework("CoreAudio"), .linkedFramework("Security"), .linkedLibrary("bsm")]),
    .executableTarget(name: "Patchlane", dependencies: ["AudioCore"], resources: [.process("Resources")], linkerSettings: [.linkedFramework("AppKit"), .linkedFramework("AVFoundation"), .linkedFramework("Security")]),
    .testTarget(name: "AudioCoreTests", dependencies: ["AudioCore", "Patchlane"])
], cxxLanguageStandard: .cxx17)
