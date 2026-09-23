import AppKit
import CoreAudio

func shellQuote(_ value: String) -> String {
    "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
}

func appleScriptString(_ value: String) -> String {
    "\"" + value.replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"") + "\""
}

final class UninstallerDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.main.async { self.run(); NSApp.terminate(nil) }
    }

    func show(_ title: String, _ message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "閉じる")
        alert.runModal()
    }

    func run() {
        let confirmation = NSAlert()
        confirmation.messageText = "Patchlaneをアンインストール"
        confirmation.informativeText = "Patchlaneと専用ドライバー（2ch／8ch）を削除します。保存した設定や他社のドライバーは残します。"
        confirmation.addButton(withTitle: "アンインストール")
        confirmation.addButton(withTitle: "キャンセル")
        guard confirmation.runModal() == .alertFirstButtonReturn else { return }
        let running = (["local.Patchlane"].flatMap { NSRunningApplication.runningApplications(withBundleIdentifier:$0) })
        if !running.isEmpty {
            let quit = NSAlert()
            quit.messageText = "Patchlaneが起動しています"
            quit.informativeText = "Patchlaneを終了してから削除を続行します。"
            quit.addButton(withTitle: "終了して続行")
            quit.addButton(withTitle: "キャンセル")
            guard quit.runModal() == .alertFirstButtonReturn else { return }
            for app in running { _ = app.terminate() }
            let deadline = Date(timeIntervalSinceNow: 10)
            while running.contains(where: { !$0.isTerminated }) && Date() < deadline {
                _ = RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
            }
            guard (["local.Patchlane"].flatMap { NSRunningApplication.runningApplications(withBundleIdentifier:$0) }).isEmpty else {
                show("削除は実行していません", "Patchlaneの終了を確認できませんでした。アプリを終了してから再実行してください。強制終了はしていません。")
                return
            }
        }
        guard let script = Bundle.main.url(forResource: "uninstall", withExtension: "sh") else {
            show("アンインストールできません", "削除プログラムが見つかりません。アンインストーラーを作り直してください。")
            return
        }
        // macOS owns the authentication dialog; this app never reads a password.
        let command = "/bin/bash " + shellQuote(script.path) + " / " + shellQuote(Bundle.main.executablePath!)
        let source = "do shell script " + appleScriptString(command) + " with administrator privileges"
        var error: NSDictionary?
        guard let authorization = NSAppleScript(source: source) else {
            show("アンインストールできません", "管理者認証の処理を準備できませんでした。")
            return
        }
        authorization.executeAndReturnError(&error)
        if let error {
            if (error[NSAppleScript.errorNumber] as? NSNumber)?.intValue == -128 {
                show("キャンセルしました", "管理者認証がキャンセルされたため、削除処理を実行していません。")
            } else {
                show("アンインストールを完了できませんでした", (error[NSAppleScript.errorMessage] as? String ?? "削除処理でエラーが発生しました。") + "\n一部のファイルが削除済みの場合があります。")
            }
            return
        }
        let targets = ["/Applications/Patchlane.app",
                       "/Library/Audio/Plug-Ins/HAL/Patchlane.driver",
                       "/Library/Audio/Plug-Ins/HAL/Patchlane-2ch.driver",
                       "/Library/PrivilegedHelperTools/audio.patchlane.ring-broker",
                       "/Library/LaunchDaemons/audio.patchlane.ring-broker.plist"]
        let remaining = targets.filter { FileManager.default.fileExists(atPath: $0) }
        guard remaining.isEmpty else {
            show("削除が完了していません", "次のファイルが残っています：\n" + remaining.joined(separator: "\n"))
            return
        }
        show("アンインストールが完了しました。", "万が一動作が不安定な場合は再起動してください。")
    }
}

// Run in a fresh process after Core Audio restarts, so no stale HAL cache is used.
func dedicatedDevicesRemain() -> Bool? {
    var address=AudioObjectPropertyAddress(mSelector:kAudioHardwarePropertyDevices,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
    var size:UInt32=0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size)==noErr, size>0 else { return nil }
    var devices=[AudioObjectID](repeating:0,count:Int(size)/MemoryLayout<AudioObjectID>.size)
    let status=devices.withUnsafeMutableBytes { AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject),&address,0,nil,&size,$0.baseAddress!) }
    guard status==noErr else { return nil }
    var readable=0
    for device in devices {
        var uidAddress=AudioObjectPropertyAddress(mSelector:kAudioDevicePropertyDeviceUID,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
        var reference:Unmanaged<CFString>?
        var bytes=UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device,&uidAddress,0,nil,&bytes,&reference)==noErr, let reference else { return nil }
        let uid=reference.takeRetainedValue()
        readable += 1
        if ["audio.patchlane.virtual8","audio.patchlane.virtual2"].contains(uid as String) { return true }
    }
    return readable>0 ? false:nil
}

if CommandLine.arguments.contains("--verify-driver-removed") {
    let deadline=Date(timeIntervalSinceNow:15)
    var absentChecks=0
    while Date()<deadline {
        if dedicatedDevicesRemain()==false { absentChecks += 1 } else { absentChecks=0 }
        if absentChecks>=3 { print("PASS dedicated audio devices removed"); exit(0) }
        _=RunLoop.current.run(mode:.default,before:Date(timeIntervalSinceNow:0.25))
    }
    fputs("ファイルは削除済みですが、音声機器の解除を確認できませんでした。Macを再起動してください。\n",stderr)
    exit(1)
} else if CommandLine.arguments.contains("--self-test") {
    precondition(Bundle.main.url(forResource: "uninstall", withExtension: "sh") != nil)
    precondition(shellQuote("a'b") == "'a'\\''b'")
    precondition(appleScriptString("a\"b\\c") == "\"a\\\"b\\\\c\"")
    print("PASS uninstaller app: bundled removal script and command quoting; no removal performed")
} else {
    let app = NSApplication.shared
    let delegate = UninstallerDelegate()
    app.delegate = delegate
    app.setActivationPolicy(.regular)
    app.run()
}
