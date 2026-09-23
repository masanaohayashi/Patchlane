import XCTest
import AudioCore
@testable import Patchlane

final class MixerTests:XCTestCase {
    var engine:OpaquePointer!
    override func setUp() { engine=lc_create() }
    override func tearDown() { lc_destroy(engine) }
    func feed(_ input:Int,_ left:Float,_ right:Float,frames:Int=4096,rate:Double=48000) {
        var data=[Float](); for _ in 0..<frames { data += [left,right] }
        lc_test_feed(engine,Int32(input),data,Int32(frames),rate)
    }
    func render(_ bus:Int,frames:Int=480,rate:Double=48000)->[Float] {
        var out=[Float](repeating:999,count:frames*2);lc_mix_read(engine,Int32(bus),&out,Int32(frames),rate); return out
    }
    func testFirst32FrameBlockIsNotHeldFor20Milliseconds() {
        lc_route(engine,0,0,1)
        feed(0,0.5,0.5,frames:32,rate:44100)
        let first=render(0,frames:32,rate:44100)
        XCTAssertTrue(first.contains { $0 > 0 }, "32 frames must be rendered without a fixed 20ms prefill")
    }
    func test32And128FrameImpulsePositionAndContinuousDelivery() {
        for frames in [32,128] {
            lc_mix_reset(engine,0);lc_route(engine,0,0,1)
            // Distinct ramps expose duplicated, dropped, or delayed samples.
            for block in 0..<2000 {
                let value=Float(block % 19 + 1)/40
                feed(0,value,-value,frames:frames,rate:44100)
                let result=render(0,frames:frames,rate:44100)
                XCTAssertTrue(result.allSatisfy { $0 != 0 }, "Missing samples at block \(block), frames \(frames)")
                if block>100 { XCTAssertEqual(result[0],value,accuracy:0.00001);XCTAssertEqual(result[1],-value,accuracy:0.00001) }
            }
            var impulse=[Float](repeating:0,count:frames*2);impulse[14]=0.5
            lc_test_feed(engine,0,impulse,Int32(frames),44100)
            let result=render(0,frames:frames,rate:44100)
            XCTAssertEqual(result.enumerated().max(by: { $0.element < $1.element })?.offset,14)
        }
    }
    func testExplicitExtraBufferPreservesInitialAudio() {
        XCTAssertEqual(lc_config_timing(engine,32,44100),0)
        XCTAssertEqual(lc_config_delay(engine,64),0)
        lc_route(engine,0,0,1)
        var impulse=[Float](repeating:0,count:64);impulse[0]=0.5
        lc_test_feed(engine,0,impulse,32,44100)
        XCTAssertTrue(render(0,frames:32,rate:44100).allSatisfy { $0==0 })
        feed(0,0,0,frames:32,rate:44100)
        XCTAssertTrue(render(0,frames:32,rate:44100).allSatisfy { $0==0 })
        feed(0,0,0,frames:32,rate:44100)
        let result=render(0,frames:32,rate:44100)
        XCTAssertGreaterThan(result[0],0)
        XCTAssertEqual(result.enumerated().max(by: { $0.element<$1.element })?.offset,0)
    }
    func testIndependentRoutingAndStereo() {
        feed(0,0.2,-0.1); feed(1,0.3,0.15)
        lc_route(engine,0,0,1);lc_route(engine,1,1,1)
        for _ in 0..<10 { feed(0,0.2,-0.1,frames:480);feed(1,0.3,0.15,frames:480); _=render(0);_=render(1) }
        feed(0,0.2,-0.1,frames:480);feed(1,0.3,0.15,frames:480)
        let main=render(0),aux=render(1),silent=render(2)
        XCTAssertEqual(main[0],0.2,accuracy:0.001);XCTAssertEqual(main[1],-0.1,accuracy:0.001)
        XCTAssertEqual(aux[0],0.3,accuracy:0.001);XCTAssertEqual(aux[1],0.15,accuracy:0.001)
        XCTAssertTrue(silent.allSatisfy{$0==0})
    }
    func testSummingGainsAndClipping() {
        for i in 0..<4 { feed(i,0.8,0.8);lc_route(engine,Int32(i),0,1) }
        lc_output_gain(engine,0,0.5)
        for _ in 0..<10 { for i in 0..<4 { feed(i,0.8,0.8,frames:480) };_=render(0) }
        for i in 0..<4 { feed(i,0.8,0.8,frames:480) }
        XCTAssertEqual(render(0)[0],1,accuracy:0.001)
        XCTAssertGreaterThan(lc_output_peak(engine,0),1)
        XCTAssertEqual(lc_output_peak(engine,0),0)
        lc_output_gain(engine,0,0.1)
        for _ in 0..<10 { for i in 0..<4 { feed(i,0.8,0.8,frames:480) };_=render(0) }
        for i in 0..<4 { feed(i,0.8,0.8,frames:480) }
        XCTAssertEqual(render(0)[0],0.32,accuracy:0.001)
    }
    func testMuteAndRouteFadeToSilence() {
        feed(0,0.5,0.5);lc_route(engine,0,0,1)
        for _ in 0..<10 { feed(0,0.5,0.5,frames:480);_=render(0) }
        lc_route(engine,0,0,0)
        for _ in 0..<10 { feed(0,0.5,0.5,frames:480);_=render(0) }
        feed(0,0.5,0.5,frames:480)
        XCTAssertLessThan(abs(render(0)[0]),0.00001)
    }
    func testUnderrunDoesNotReplayStaleAudio() {
        feed(0,0.5,0.5);lc_route(engine,0,0,1)
        for _ in 0..<30 { _=render(0) }
        XCTAssertTrue(render(0).allSatisfy{$0==0})
        for _ in 0..<4 { feed(0,0.25,0.25,frames:480);_=render(0) }
        feed(0,0.25,0.25,frames:480)
        XCTAssertGreaterThan(render(0)[0],0.1)
    }
    func testDifferentSampleRatesAndWraparound() {
        lc_route(engine,0,0,1)
        for _ in 0..<400 {
            feed(0,0.25,-0.25,frames:441,rate:44100)
            let audio=render(0)
            XCTAssertTrue(audio.allSatisfy{$0.isFinite && abs($0)<=0.251})
        }
        feed(0,0.25,-0.25,frames:441,rate:44100)
        let audio=render(0);XCTAssertEqual(audio[0],0.25,accuracy:0.001)
    }
    func testInvalidDeviceFailsAndCanRestart() {
        XCTAssertEqual(lc_config_input(engine,0,0xfffffff,0,1),0)
        XCTAssertNotEqual(lc_start(engine),0)
        XCTAssertFalse(String(cString:lc_error(engine)).isEmpty)
        XCTAssertEqual(lc_config_input(engine,0,0,0,1),0)
        XCTAssertEqual(lc_start(engine),0);lc_stop(engine)
        XCTAssertEqual(lc_start(engine),0);lc_stop(engine)
    }
    func testNonFiniteSamplesAreSanitized() {
        feed(0,.nan,.infinity);lc_route(engine,0,0,1)
        XCTAssertTrue(render(0).allSatisfy{$0==0})
    }
}
final class ConfigurationTests:XCTestCase {
    func testRoundTripAndSecretsNotExported() throws {
        var s=Settings();s.inputs[2].routes=[false,true,false,true];s.buses[2].uid="test-device"
        let data=try JSONEncoder().encode(s),copy=try JSONDecoder().decode(Settings.self,from:data)
        try copy.validate();XCTAssertEqual(copy.inputs[2].routes,s.inputs[2].routes);XCTAssertEqual(copy.buses[2].uid,"test-device")
        XCTAssertFalse(String(decoding:data,as:UTF8.self).contains("password"))
    }
    func testTimingSettingsAndLegacyMigration() throws {
        var settings=Settings();settings.audio.frames=32;settings.audio.sampleRate=44100
        let data=try JSONEncoder().encode(settings)
        let copy=try JSONDecoder().decode(Settings.self,from:data)
        XCTAssertEqual(copy.audio.frames,32);XCTAssertEqual(copy.audio.sampleRate,44100)
        var json=try XCTUnwrap(JSONSerialization.jsonObject(with:data) as? [String:Any]);json.removeValue(forKey:"audioSettings")
        let legacy=try JSONDecoder().decode(Settings.self,from:JSONSerialization.data(withJSONObject:json))
        XCTAssertEqual(legacy.audio.frames,32)
        settings.audio.frames=31;XCTAssertThrowsError(try settings.validate())
    }
    func testMalformedSettingsRejected() {
        var s=Settings();s.inputs=[];XCTAssertThrowsError(try s.validate())
        s=Settings();s.inputs[0].routes=[];XCTAssertThrowsError(try s.validate())
        s=Settings();s.buses[0].db = .nan;XCTAssertThrowsError(try s.validate())
    }
}
