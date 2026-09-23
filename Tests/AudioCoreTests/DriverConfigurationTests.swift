import XCTest
@testable import Patchlane

final class DriverConfigurationTests:XCTestCase {
    func testDriverWireFormatAndRoutingRoundTrip() throws {
        var config=DriverConfiguration()
        config.inputGain[2]=0.25
        config.outputGain[3]=0
        config.routes[0][3]=true
        let data=config.data
        XCTAssertEqual(data.count,100)
        XCTAssertEqual(Array(data.prefix(4)),[1,0,0,0])
        XCTAssertEqual(try DriverConfiguration(data:data),config)
        // C++ layout: version, four input gains, four output gains, 4x4 UInt32 routes.
        XCTAssertEqual(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset:12,as:UInt32.self) },Float(0.25).bitPattern)
        XCTAssertEqual(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset:48,as:UInt32.self) },1)
    }
    func testDriverRejectsUnsupportedOrCorruptWireData() {
        XCTAssertThrowsError(try DriverConfiguration(data:Data()))
        var config=DriverConfiguration(); config.inputGain[0] = .nan
        XCTAssertThrowsError(try DriverConfiguration(data:config.data))
        var data=DriverConfiguration().data; data[0]=2
        XCTAssertThrowsError(try DriverConfiguration(data:data))
        data=DriverConfiguration().data; data[36]=2
        XCTAssertThrowsError(try DriverConfiguration(data:data))
    }
}
