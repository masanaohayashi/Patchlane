import XCTest
@testable import Patchlane

final class AudioModeLifecycleTests:XCTestCase {
    func testStartupSwitchAndDelayedPermissionReply() {
        var mode=AudioModeLifecycle()
        XCTAssertFalse(mode.shouldRunMixer) // CLI diagnostics must not autostart.
        mode.enabled=true
        XCTAssertTrue(mode.shouldRunMixer)
        mode.dedicated=true
        // Both the initial switch and a late microphone permission response
        // use this gate; neither may restart the mixer during driver ownership.
        XCTAssertFalse(mode.shouldRunMixer)
        var connection=DriverConnectionIntent()
        connection.request()
        connection.interrupt(now:0)
        XCTAssertTrue(connection.wanted)
        XCTAssertFalse(mode.shouldRunMixer) // Recovery keeps driver ownership.
        connection.stop()
        XCTAssertFalse(mode.shouldRunMixer) // Asynchronous teardown is pending.
        mode.dedicated=false // Only after teardown and timing restoration.
        XCTAssertTrue(mode.shouldRunMixer)
    }
    func testSleepOffAndShutdownCannotRestartAudio() {
        var mode=AudioModeLifecycle(enabled:true,dedicated:true)
        mode.sleeping=true
        mode.dedicated=false
        XCTAssertFalse(mode.shouldRunMixer)
        mode.sleeping=false
        XCTAssertTrue(mode.shouldRunMixer)
        mode.shuttingDown=true
        mode.dedicated=true
        mode.dedicated=false
        XCTAssertFalse(mode.shouldRunMixer)
    }
    func testLegacyBroadcastConfigurationIsIgnored() throws {
        var settings=Settings()
        settings.inputs[0].uid="test-input"
        settings.buses[0].uid="test-output"
        settings.audio.frames=64
        var json=try XCTUnwrap(JSONSerialization.jsonObject(with:JSONEncoder().encode(settings)) as? [String:Any])
        json["streams"]=[["legacy":"unused"]]
        json["ffmpeg"]="/old/ffmpeg"
        var audio=try XCTUnwrap(json["audioSettings"] as? [String:Any])
        audio["streamerCount"]=4;json["audioSettings"]=audio
        let decoded=try JSONDecoder().decode(Settings.self,from:JSONSerialization.data(withJSONObject:json))
        try decoded.validate()
        XCTAssertEqual(decoded.inputs[0].uid,"test-input")
        XCTAssertEqual(decoded.buses[0].uid,"test-output")
        XCTAssertEqual(decoded.audio.frames,64)
        let result=String(decoding:try JSONEncoder().encode(decoded),as:UTF8.self)
        for obsolete in ["streams","streamerCount","ffmpeg"] { XCTAssertFalse(result.contains(obsolete)) }
    }
}
