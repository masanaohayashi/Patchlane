import XCTest
@testable import Patchlane

final class DriverOutputHealthTests:XCTestCase {
    func testRunningCallbacksWithContinuousMissingFramesRequestRecovery() {
        var health=DriverOutputHealth()
        XCTAssertFalse(health.observe([.init(callbacks:100,missing:0,received:44100)]))
        XCTAssertFalse(health.observe([.init(callbacks:200,missing:44100,received:44100)]))
        XCTAssertTrue(health.observe([.init(callbacks:300,missing:88200,received:44100)]))
    }
    func testHistoricalLossAndIsolatedGlitchDoNotRestart() {
        var h=DriverOutputHealth()
        XCTAssertFalse(h.observe([.init(callbacks:100,missing:490804,received:44100)]))
        XCTAssertFalse(h.observe([.init(callbacks:200,missing:490804,received:88200)]))
        XCTAssertFalse(h.observe([.init(callbacks:300,missing:534904,received:88200)]))
        XCTAssertFalse(h.observe([.init(callbacks:400,missing:534904,received:132300)]))
        XCTAssertFalse(h.observe([.init(callbacks:500,missing:534936,received:176368)]))
    }
    func testOneStarvedOutputIsNotHiddenByHealthyOutputs() {
        var h=DriverOutputHealth()
        for tick in 0...2 {
            let bad=DriverOutputHealth.Sample(callbacks:UInt64(tick+1),missing:UInt64(tick*44100),received:44100)
            let good=DriverOutputHealth.Sample(callbacks:UInt64(tick+1),missing:0,received:UInt64((tick+1)*44100))
            XCTAssertEqual(h.observe([bad,good,good,good]),tick==2)
        }
    }
    func testWaitingForFirstAudioAndCounterResetAreSafe() {
        var h=DriverOutputHealth()
        for tick in 0...4 {
            XCTAssertFalse(h.observe([.init(callbacks:UInt64(tick+1),missing:UInt64(tick*44100),received:0)]))
        }
        XCTAssertFalse(h.observe([.init(callbacks:1,missing:0,received:32)]))
        XCTAssertFalse(h.observe([.init(callbacks:2,missing:0,received:64)]))
    }
    func testStoppedCallbacksStillTriggerRecovery() {
        var h=DriverOutputHealth()
        let sample=DriverOutputHealth.Sample(callbacks:100,missing:0,received:44100)
        XCTAssertFalse(h.observe([sample]))
        XCTAssertFalse(h.observe([sample]))
        XCTAssertTrue(h.observe([sample]))
    }
}
