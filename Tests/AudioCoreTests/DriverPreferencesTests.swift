import XCTest
@testable import Patchlane

final class DriverPreferencesTests:XCTestCase {
    func testDedicatedChoiceAndConnectionSurviveRelaunch() throws {
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).appendingPathComponent("driver.json")
        defer { try? FileManager.default.removeItem(at:url.deletingLastPathComponent()) }
        var config=DriverConfiguration();config.inputGain[0]=0.5
        var saved=DriverPreferences(enabled:true,sourceUID:"audio.patchlane.virtual2",outputUID:"physical-output",bus:0,left:2,right:3,frames:64,rate:48000,delay:96,configurations:["audio.patchlane.virtual2":config.data])
        try saved.save(to:url)
        let restored=try DriverPreferences.load(from:url)
        XCTAssertEqual(restored,saved)
        var lifecycle=AudioModeLifecycle(enabled:true,dedicated:restored.enabled)
        XCTAssertFalse(lifecycle.shouldRunMixer)
        // Shutdown of the connection must not overwrite the user's saved mode.
        var connection=DriverConnectionIntent();connection.request();connection.stop()
        XCTAssertTrue(try DriverPreferences.load(from:url).enabled)
        saved.enabled=false;try saved.save(to:url)
        lifecycle.dedicated=try DriverPreferences.load(from:url).enabled
        XCTAssertTrue(lifecycle.shouldRunMixer)
    }
    func testOldConnectionMethodDoesNotPreventLoading() throws {
        // Old aggregate selection is ignored; the app now always uses the ring.
        let original=DriverPreferences(enabled:true)
        var json=try XCTUnwrap(JSONSerialization.jsonObject(with:JSONEncoder().encode(original)) as? [String:Any])
        json["shared"]=false
        let restored=try JSONDecoder().decode(DriverPreferences.self,from:JSONSerialization.data(withJSONObject:json))
        XCTAssertEqual(restored,original)
    }
    func testRestoredConnectionWaitsForDeviceAndCanBeCancelled() {
        var intent=DriverConnectionIntent()
        intent.request();intent.interrupt(now:0)
        intent.failed()
        XCTAssertTrue(intent.wanted)
        XCTAssertTrue(intent.shouldRetry(now:2))
        intent.stop()
        XCTAssertFalse(intent.shouldRetry(now:10))
    }
    func testInvalidStoredAudioConfigurationRejected() throws {
        var saved=DriverPreferences();saved.frames=7
        XCTAssertThrowsError(try saved.validate())
        saved.frames=32;saved.configurations["audio.patchlane.virtual8"]=Data([1,2])
        XCTAssertThrowsError(try saved.validate())
    }
}
