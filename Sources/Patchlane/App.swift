import SwiftUI
import AppKit
import AudioCore

@main struct PatchlaneApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model=MixerModel.shared
    init() {
        if let index=CommandLine.arguments.firstIndex(of:"--driver-recovery-check") {
            guard CommandLine.arguments.count==index+2 else { print("Usage: --driver-recovery-check outputUID");exit(2) }
            exit(DriverRecoveryCheck.run(uid:CommandLine.arguments[index+1]))
        }
        if let index=CommandLine.arguments.firstIndex(of:"--shared-silent-check") {
            let args=Array(CommandLine.arguments.dropFirst(index+1))
            guard (args.count==4||args.count==5),let destination=UInt32(args[0]),let frames=Int32(args[1]),let rate=Double(args[2]),let delay=Double(args[3]),
                  let source=MixerModel.enumerate().first(where: { $0.uid=="audio.patchlane.virtual8" }) else {
                print("Usage: --shared-silent-check destinationID frames rate delaySourceFrames [seconds]");exit(2)
            }
            guard let seconds=args.count==5 ? Double(args[4]):1 else { print("Invalid duration");exit(2) }
            var report=[CChar](repeating:0,count:512)
            let result=lc_shared_silent_check(source.id,destination,frames,rate,delay,seconds,&report,Int32(report.count))
            print(String(cString:report));exit(result)
        }
        if let index=CommandLine.arguments.firstIndex(of:"--latency-check") {
            var arguments=(["latency-check"]+Array(CommandLine.arguments.dropFirst(index+1))).map { strdup($0) }
            let count=Int32(arguments.count)
            arguments.append(nil)
            let result=arguments.withUnsafeMutableBufferPointer { lc_latency_check(count,$0.baseAddress) }
            for argument in arguments { free(argument) }
            exit(result)
        }
        if let index=CommandLine.arguments.firstIndex(of:"--shared-output-check") {
            guard CommandLine.arguments.count>index+1,let device=UInt32(CommandLine.arguments[index+1]) else {
                print("Usage: --shared-output-check deviceID [delaySourceFrames]");exit(2)
            }
            let delay=CommandLine.arguments.count>index+2 ? Double(CommandLine.arguments[index+2]) : 0
            guard let delay,delay.isFinite,delay>=0,delay<65536 else { print("Invalid delay");exit(2) }
            let output=lc_shared_output_create()!
            guard lc_shared_output_start(output,device,0,0,1,32,44100,delay)==0 else {
                print(String(cString:lc_shared_output_error(output)));lc_shared_output_destroy(output);exit(1)
            }
            Thread.sleep(forTimeInterval:1)
            lc_shared_output_stop(output)
            let callbacks=lc_shared_output_callbacks(output),invalid=lc_shared_output_invalid_clock(output)
            print("delaySourceFrames=\(delay) callbacks=\(callbacks) invalidClock=\(invalid) missingFrames=\(lc_shared_output_missing_frames(output))")
            lc_shared_output_destroy(output)
            exit(callbacks>0 && invalid==0 ? 0:1)
        }
        if CommandLine.arguments.contains("--shared-ring-probe") {
            var message=[CChar](repeating:0,count:256)
            let result=lc_shared_probe(&message,Int32(message.count))
            print(String(cString:message));exit(result==0 ? 0:1)
        }
        if CommandLine.arguments.contains("--list-devices") {
            for d in MixerModel.enumerate() { print("\(d.id)\t\(d.name)\tin=\(d.inputs) out=\(d.outputs) \(d.rate)Hz\t\(d.uid)") }; exit(0)
        }
        if CommandLine.arguments.contains("--smoke-output") {
            let e=lc_create()!; defer { lc_destroy(e) }
            let ds=MixerModel.enumerate().filter { $0.outputs>0 }
            guard let d=ds.first(where: { $0.name.contains("BlackHole") }) ?? ds.first else { print("No output device"); exit(2) }
            lc_config_output(e,0,d.id)
            guard lc_start(e)==0 else { print(String(cString:lc_error(e))); exit(1) }
            Thread.sleep(forTimeInterval:1); lc_stop(e); print("AUHAL silent output OK: \(d.name)"); exit(0)
        }
    }
    var body: some Scene {
        WindowGroup("Patchlane",id:"mixer") { MixerView(model:model) }
            .defaultSize(width:1180,height:640)
            .windowResizability(.contentSize)
            .commands {
                CommandGroup(replacing:.newItem) {}
            }
        MenuBarExtra("Patchlane",systemImage:model.running ? "waveform":"waveform.slash") {
            Button(L("ミキサーを表示")) { NSApp.activate(ignoringOtherApps:true); if let window=NSApp.windows.first(where: { $0.title=="Patchlane" }) { window.makeKeyAndOrderFront(nil) } }
            Divider()
            ForEach(0..<4) { i in Button("\(busNames[i]) \(model.settings.buses[i].muted ? L("ミュート解除"):L("ミュート"))") { model.settings.buses[i].muted.toggle(); model.changed() } }
            Divider()
            Button(L("終了")) { NSApp.terminate(nil) }.keyboardShortcut("q")
        }
    }
}
class AppDelegate:NSObject,NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification:Notification) { NSApp.setActivationPolicy(.regular); NSApp.activate(ignoringOtherApps:true); DriverModel.shared.restoreAtLaunch() }
    func applicationWillTerminate(_ notification:Notification) { MixerModel.shared.shutdown(); DriverModel.shared.shutdownBridge() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender:NSApplication)->Bool { false }
}
