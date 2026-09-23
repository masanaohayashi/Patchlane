import XCTest
@testable import Patchlane

final class MixerControlTests:XCTestCase {
    private func temporarySettings() -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).appendingPathComponent("settings.json")
    }
    private func pump(until ready:()->Bool) {
        let deadline=Date(timeIntervalSinceNow:5)
        while !ready(),Date()<deadline { _=RunLoop.current.run(mode:.default,before:Date(timeIntervalSinceNow:0.01)) }
        XCTAssertTrue(ready())
    }
    func testPendingStartCannotReenableMixerAfterDriverTakesOwnership() {
        XCTAssertTrue(Thread.isMainThread)
        let url=temporarySettings()
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{ [] }))
        defer { model.shutdown();try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        // Empty settings open no physical devices and do not touch user preferences.
        model.enableAutomaticMixer()
        XCTAssertFalse(model.running)
        var stopped=false
        model.suspendForDriver { stopped=true }
        pump { stopped }
        XCTAssertTrue(model.dedicatedMode)
        XCTAssertFalse(model.running)
        model.resumeFromDriver()
        pump { model.running }
        XCTAssertFalse(model.dedicatedMode)
    }
    func testRapidRestartAndTerminationIgnoreOldCompletions() {
        XCTAssertTrue(Thread.isMainThread)
        let url=temporarySettings()
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{ [] }))
        defer { try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        model.enableAutomaticMixer()
        model.stop()
        model.start()
        pump { model.running }
        model.changed(reconfigure:true)
        model.shutdown()
        // Drain already queued main-thread completion notifications.
        _=RunLoop.current.run(mode:.default,before:Date(timeIntervalSinceNow:0.05))
        XCTAssertFalse(model.running)
        model.start()
        XCTAssertFalse(model.running)
    }
    func testUnchangedReconfigureDoesNotInterruptRunningMixer() throws {
        let url=temporarySettings()
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{ [] }))
        defer { model.shutdown();try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        model.enableAutomaticMixer()
        pump { model.running }
        // This is the onChange call used by the settings controls. Unrelated
        // meter redraws must not stop audio when the connection is unchanged.
        for _ in 0..<10 {
            model.changed(reconfigure:true)
            XCTAssertTrue(model.running,"Unchanged settings stopped the mixer")
        }
    }

    func testOnlyConnectionChangesRestartMixer() {
        let url=temporarySettings()
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{ [] }))
        defer { model.shutdown();try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        model.enableAutomaticMixer();pump { model.running }
        model.settings.inputs[0].db = -3
        model.settings.inputs[0].routes[1]=true
        model.changed(reconfigure:true)
        XCTAssertTrue(model.running)
        model.settings.audio.frames=64
        model.changed(reconfigure:true)
        XCTAssertFalse(model.running)
        pump { model.running }
        model.changed(reconfigure:true)
        XCTAssertTrue(model.running)
    }
    func testSlowDeviceVolumeReadDoesNotBlockMainRunLoop() {
        let url=temporarySettings()
        let device=Device(id:123,uid:"fixture",name:"Fixture",inputs:0,outputs:2,rate:44100)
        let entered=expectation(description:"HAL query started off main")
        entered.assertForOverFulfill=false
        let release=DispatchSemaphore(value:0)
        let access=MixerDeviceAccess(enumerate:{
            XCTAssertFalse(Thread.isMainThread)
            return [device]
        },readVolume:{ d in
            XCTAssertFalse(Thread.isMainThread)
            entered.fulfill()
            _=release.wait(timeout:.now()+2)
            return DeviceVolume.Control(device:d.id,addresses:[],values:[0.5])
        })
        let model=MixerModel(settingsURL:url,deviceAccess:access)
        defer { release.signal();model.shutdown();try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        model.settings.buses[0].uid=device.uid
        wait(for:[entered],timeout:1)
        let responsive=expectation(description:"UI remains responsive during HAL wait")
        DispatchQueue.main.async { responsive.fulfill() }
        wait(for:[responsive],timeout:0.5)
        release.signal()
        pump { model.deviceVolumes[device.uid]==0.5 }
    }

}
