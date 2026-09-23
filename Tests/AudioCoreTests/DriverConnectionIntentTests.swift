import XCTest
@testable import Patchlane

final class DriverConnectionIntentTests:XCTestCase {
    func testSleepRecoveryRequiresPriorIntentAndWaitsForWake() {
        var intent=DriverConnectionIntent()
        intent.sleep(now:0);intent.wake(now:10)
        XCTAssertFalse(intent.shouldRetry(now:20))
        intent.request();intent.succeeded();intent.sleep(now:30)
        XCTAssertFalse(intent.shouldRetry(now:100))
        intent.wake(now:100)
        XCTAssertFalse(intent.shouldRetry(now:101))
        XCTAssertTrue(intent.shouldRetry(now:102))
    }
    func testStopWhileDisconnectedCancelsRetryAndWake() {
        var intent=DriverConnectionIntent();intent.request();intent.succeeded()
        intent.interrupt(now:0);intent.stop()
        XCTAssertFalse(intent.shouldRetry(now:20))
        intent.sleep(now:30);intent.wake(now:40)
        XCTAssertFalse(intent.shouldRetry(now:100))
    }
    func testRecoveryBacksOffButInitialFailureDoesNotLoop() {
        var intent=DriverConnectionIntent();intent.request();intent.failed()
        XCTAssertFalse(intent.wanted)
        intent.request();intent.succeeded();intent.interrupt(now:10)
        XCTAssertTrue(intent.shouldRetry(now:12))
        intent.attempting(now:12);intent.failed()
        XCTAssertFalse(intent.shouldRetry(now:16))
        XCTAssertTrue(intent.shouldRetry(now:17))
        intent.succeeded();XCTAssertFalse(intent.shouldRetry(now:100))
    }
    func testSettingsChangeKeepsDriverOwnershipUntilValidAndCanBeCancelled() {
        var intent=DriverConnectionIntent()
        intent.request();intent.succeeded()
        // Reconfiguration tears down the current connection without enabling
        // the normal mixer. An intermediate invalid L/R selection is editable.
        intent.interrupt(now:10)
        intent.failed()
        XCTAssertTrue(intent.wanted)
        XCTAssertTrue(intent.recovering)
        intent.succeeded()
        XCTAssertTrue(intent.wanted)
        XCTAssertFalse(intent.recovering)
        intent.interrupt(now:20)
        intent.stop()
        XCTAssertFalse(intent.shouldRetry(now:30))
        XCTAssertFalse(intent.wanted)
    }

}
