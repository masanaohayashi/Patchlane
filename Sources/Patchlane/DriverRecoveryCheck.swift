import AppKit
import CoreAudio

// Explicit CLI integration diagnostic. Posts only in-process notifications;
// never requests system sleep, restarts Core Audio, or generates test audio.
enum DriverRecoveryCheck {
    private static func property<T>(_ id:AudioDeviceID,_ selector:AudioObjectPropertySelector,_ initial:T) throws -> T {
        var address=AudioObjectPropertyAddress(mSelector:selector,mScope:kAudioObjectPropertyScopeGlobal,mElement:kAudioObjectPropertyElementMain)
        var value=initial, size=UInt32(MemoryLayout<T>.size)
        let status=withUnsafeMutableBytes(of:&value) { bytes in
            AudioObjectGetPropertyData(id,&address,0,nil,&size,bytes.baseAddress!)
        }
        guard status==noErr,size==MemoryLayout<T>.size else { throw DriverError.message("Cannot read device property: \(status)") }
        return value
    }
    private static func pump() { _=RunLoop.current.run(mode:.default,before:Date(timeIntervalSinceNow:0.02)) }
    private static func wait(_ label:String,_ condition:()->Bool) throws {
        let deadline=ProcessInfo.processInfo.systemUptime+10
        while !condition() {
            guard ProcessInfo.processInfo.systemUptime<deadline else { throw DriverError.message("Timed out: \(label)") }
            pump()
        }
    }
    static func run(uid:String) -> Int32 {
        var active:DriverModel?
        defer { active?.shutdownBridge() }
        do {
            guard Thread.isMainThread,let device=MixerModel.enumerate().first(where:{$0.uid==uid}),device.outputs>=2 else {
                throw DriverError.message("Output device not found")
            }
            guard try property(device.id,kAudioDevicePropertyDeviceIsRunningSomewhere,UInt32(0))==0 else {
                throw DriverError.message("Output device is in use; left unchanged")
            }
            let originalRate=try property(device.id,kAudioDevicePropertyNominalSampleRate,Double(0))
            let originalFrames=try property(device.id,kAudioDevicePropertyBufferFrameSize,UInt32(0))
            let driver=DriverModel.shared;active=driver
            MixerModel.shared.settings.inputs[0].uid="audio.patchlane.virtual2"
            MixerModel.shared.settings.buses[0].uid=uid
            MixerModel.shared.settings.audio.frames=32;MixerModel.shared.settings.audio.sampleRate=44100;MixerModel.shared.settings.audio.delay=80
            driver.startBridge()
            try wait("initial connection: \(driver.error ?? "")") { driver.bridgeRunning }
            let center=NSWorkspace.shared.notificationCenter
            center.post(name:NSWorkspace.willSleepNotification,object:nil)
            try wait("sleep teardown") { driver.connectionIntent.sleeping && !driver.bridgeRunning && !driver.bridgeBusy }
            guard driver.connectionIntent.wanted else { throw DriverError.message("Sleep discarded connection intent") }
            guard try property(device.id,kAudioDevicePropertyNominalSampleRate,Double(0))==originalRate,
                  try property(device.id,kAudioDevicePropertyBufferFrameSize,UInt32(0))==originalFrames else {
                throw DriverError.message("Sleep did not restore device timing")
            }
            center.post(name:NSWorkspace.didWakeNotification,object:nil)
            try wait("wake reconnect: \(driver.error ?? "")") { driver.bridgeRunning && !driver.connectionIntent.sleeping }
            center.post(name:NSWorkspace.willSleepNotification,object:nil)
            try wait("second sleep teardown") { driver.connectionIntent.sleeping && !driver.bridgeRunning && !driver.bridgeBusy }
            driver.stopBridge() // User stop while recovery is pending.
            try wait("manual stop") { !driver.bridgeBusy && !driver.connectionIntent.wanted }
            center.post(name:NSWorkspace.didWakeNotification,object:nil)
            try wait("second wake delivery") { !driver.connectionIntent.sleeping }
            let deadline=ProcessInfo.processInfo.systemUptime+3.5
            while ProcessInfo.processInfo.systemUptime<deadline {
                pump()
                guard !driver.bridgeRunning && !driver.bridgeBusy && !driver.connectionIntent.wanted else {
                    throw DriverError.message("Manual stop was undone by a wake or late completion")
                }
            }
            driver.shutdownBridge()
            guard try property(device.id,kAudioDevicePropertyNominalSampleRate,Double(0))==originalRate,
                  try property(device.id,kAudioDevicePropertyBufferFrameSize,UInt32(0))==originalFrames else {
                throw DriverError.message("Final device timing was not restored")
            }
            print("PASS DriverModel integration: real shared output, simulated sleep/wake, reconnect, stop cancels recovery, timing restored")
            return 0
        } catch {
            print("FAIL DriverModel recovery: \(error.localizedDescription)")
            return 1
        }
    }
}
