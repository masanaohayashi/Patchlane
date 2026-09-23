import XCTest
@testable import Patchlane

final class GainSliderTests: XCTestCase {
    func testUnityDetentFromEitherDirection() {
        for value in [-0.5, -0.1, -0.001, 0, 0.001, 0.1, 0.5] {
            XCTAssertEqual(GainSlider.decibels(value), 0)
        }
    }
    func testOutsideDetentRemainsContinuous() {
        for value in [-60.0, -1, -0.501, 0.501, 1, 36] {
            XCTAssertEqual(GainSlider.decibels(value), value)
        }
    }
}
