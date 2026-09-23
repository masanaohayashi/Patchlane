import XCTest
@testable import Patchlane

final class UnifiedRoutingTests:XCTestCase {
    let source=Device(id:10,uid:"audio.patchlane.virtual2",name:"Source",inputs:2,outputs:2,rate:44100)
    let output=Device(id:20,uid:"physical",name:"Output",inputs:0,outputs:8,rate:44100)
    func testDedicatedSingleInputFansOutToAllCommonOutputs() throws {
        var settings=Settings()
        settings.inputs[0].uid=source.uid;settings.inputs[0].routes=[true,true,true,true]
        settings.inputs[1].uid="unused-physical-input"
        settings.audio.frames=64;settings.audio.sampleRate=48000;settings.audio.delayBuffers=2
        for i in 0..<4 { settings.buses[i].uid=output.uid;settings.buses[i].left=i*2;settings.buses[i].right=i*2+1 }
        let plan=try DriverConnectionPlan(settings:settings,devices:[source,output])
        XCTAssertEqual(plan.outputs.map(\.bus),[0,1,2,3])
        XCTAssertEqual(plan.outputs.map(\.left),[0,2,4,6])
        XCTAssertEqual(plan.source.id,source.id)
        XCTAssertEqual(plan.timing.bufferDelayFrames,192)
        XCTAssertEqual(plan.timing.sampleRate,48000)
    }
    func testBufferCountTracksBlockLengthAndPersists() throws {
        var settings=Settings();settings.audio.delayBuffers=2
        XCTAssertEqual(settings.audio.delay,64)
        settings.audio.frames=128
        XCTAssertEqual(settings.audio.delay,256)
        let copy=try JSONDecoder().decode(Settings.self,from:JSONEncoder().encode(settings))
        XCTAssertEqual(copy.audio,settings.audio)
        settings.audio.delayBuffers=0
        XCTAssertEqual(settings.audio.delay,0)
        XCTAssertEqual(Settings().audio.delay,0)
    }
    func testBothModesRetainOneBaseBufferWithZeroAdditionalBuffers() throws {
        var settings=Settings()
        settings.inputs[0].uid=source.uid;settings.buses[0].uid=output.uid
        settings.audio.frames=32;settings.audio.sampleRate=48000
        XCTAssertEqual(settings.audio.delay,0)
        XCTAssertEqual(settings.audio.bufferDelayMilliseconds,1000.0/1500,accuracy:0.000001)
        let plan=try DriverConnectionPlan(settings:settings,devices:[source,output])
        XCTAssertEqual(plan.timing.delayBuffers,0)
        XCTAssertEqual(plan.timing.bufferDelayFrames,32)
        XCTAssertEqual(settings.audio.bufferDelayMilliseconds,1000.0/1500,accuracy:0.000001)
        settings.audio.delayBuffers=2
        XCTAssertEqual(settings.audio.bufferDelayMilliseconds,2,accuracy:0.000001)
        XCTAssertEqual(try DriverConnectionPlan(settings:settings,devices:[source,output]).timing.bufferDelayFrames,96)
    }
    func testInvalidDedicatedChannelsAndSelfOutputRejected() {
        var s=Settings();s.inputs[0].uid=source.uid;s.buses[0].uid=output.uid
        s.inputs[0].right=2
        XCTAssertThrowsError(try DriverConnectionPlan(settings:s,devices:[source,output]))
        s.inputs[0].right=1;s.buses[0].uid=source.uid
        XCTAssertThrowsError(try DriverConnectionPlan(settings:s,devices:[source,output]))
    }
}
