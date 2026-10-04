import XCTest
import AudioCore
@testable import Patchlane

final class DipoleTests:XCTestCase {
    @MainActor func testSwitchingOutputDeviceRestoresItsOwnDipoleState() throws {
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".json")
        defer{try? FileManager.default.removeItem(at:url)}
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{[]},readVolume:{_ in nil},writeVolume:{_,_ in 0}))
        defer{model.shutdown()}
        model.settings.buses[0].uid="external-headphones"
        model.changed(reconfigure:true)
        var headphoneParameters=DipoleParameters();headphoneParameters.spacingCM=10;headphoneParameters.geqDB[5]=4
        let headphoneID=try model.settings.saveDipolePreset(name:"JC Default",parameters:headphoneParameters,bus:0)
        model.settings.buses[0].dipole.enabled=true;model.changed()
        model.settings.buses[0].uid="macbook-speakers";model.changed(reconfigure:true)
        XCTAssertFalse(model.settings.buses[0].dipole.enabled)
        XCTAssertNil(model.settings.buses[0].dipole.presetID)
        XCTAssertEqual(model.settings.buses[0].dipole.parameters,DipoleParameters())
        var speakerParameters=DipoleParameters();speakerParameters.spacingCM=30
        let speakerID=try model.settings.saveDipolePreset(name:"Mac speakers",parameters:speakerParameters,bus:0)
        model.settings.buses[0].dipole.enabled=false;model.changed()
        model.settings.buses[0].uid="external-headphones";model.changed(reconfigure:true)
        XCTAssertTrue(model.settings.buses[0].dipole.enabled)
        XCTAssertEqual(model.settings.buses[0].dipole.presetID,headphoneID)
        XCTAssertEqual(model.settings.buses[0].dipole.parameters,headphoneParameters)
        model.settings.buses[0].uid="macbook-speakers";model.changed(reconfigure:true)
        XCTAssertFalse(model.settings.buses[0].dipole.enabled)
        XCTAssertEqual(model.settings.buses[0].dipole.presetID,speakerID)
        XCTAssertEqual(model.settings.buses[0].dipole.parameters,speakerParameters)
    }
    @MainActor func testDeviceDipolesPersistAcrossRestartAndOutputSlots() throws {
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".json")
        defer{try? FileManager.default.removeItem(at:url)}
        var settings=Settings();settings.buses[0].uid="headphones"
        var parameters=DipoleParameters();parameters.geqDB[3]=5
        let id=try settings.saveDipolePreset(name:"JC Default",parameters:parameters,bus:0)
        settings.buses[0].dipole.enabled=true;settings.captureOutputDeviceDipoles()
        let previous=settings.buses
        settings.buses[0].uid="speakers";settings.synchronizeOutputDeviceDipoles(previous:previous)
        settings.buses[0].dipole.parameters.trimDB = -3
        try settings.save(to:url)
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{[]},readVolume:{_ in nil},writeVolume:{_,_ in 0}))
        defer{model.shutdown()}
        XCTAssertFalse(model.settings.buses[0].dipole.enabled)
        XCTAssertEqual(model.settings.buses[0].dipole.parameters.trimDB,-3)
        model.settings.buses[2].uid="headphones";model.changed(reconfigure:true)
        XCTAssertTrue(model.settings.buses[2].dipole.enabled)
        XCTAssertEqual(model.settings.buses[2].dipole.presetID,id)
        XCTAssertEqual(model.settings.buses[2].dipole.parameters,parameters)
        model.settings.buses[2].uid="";model.changed(reconfigure:true)
        XCTAssertEqual(model.settings.buses[2].dipole,OutputDipoleSettings())
        XCTAssertNil(model.settings.outputDeviceDipoles[""])
        model.settings.buses[0].uid="headphones";model.changed(reconfigure:true)
        XCTAssertTrue(model.settings.buses[0].dipole.enabled)
        XCTAssertEqual(model.settings.buses[0].dipole.presetID,id)
        model.settings.buses[0].uid="speakers";model.changed(reconfigure:true)
        XCTAssertFalse(model.settings.buses[0].dipole.enabled)
        XCTAssertEqual(model.settings.buses[0].dipole.parameters.trimDB,-3)
    }
    func testPresetUpdateAndDeletionIncludeUnselectedDevices() throws {
        var settings=Settings();settings.buses[0].uid="headphones"
        var parameters=DipoleParameters()
        let id=try settings.saveDipolePreset(name:"JC Default",parameters:parameters,bus:0)
        settings.buses[0].dipole.enabled=true;settings.captureOutputDeviceDipoles()
        var previous=settings.buses
        settings.buses[0].uid="speakers";settings.synchronizeOutputDeviceDipoles(previous:previous)
        parameters.geqDB[4]=6
        XCTAssertEqual(try settings.saveDipolePreset(name:"JC Default",parameters:parameters,bus:0),id)
        XCTAssertEqual(settings.outputDeviceDipoles["headphones"]?.parameters,parameters)
        settings.deleteDipolePreset(id);try settings.validate()
        XCTAssertNil(settings.outputDeviceDipoles["headphones"]?.presetID)
        XCTAssertEqual(settings.outputDeviceDipoles["headphones"]?.enabled,true)
        XCTAssertEqual(settings.outputDeviceDipoles["headphones"]?.parameters,parameters)
        previous=settings.buses
        settings.buses[0].uid="headphones";settings.synchronizeOutputDeviceDipoles(previous:previous)
        XCTAssertTrue(settings.buses[0].dipole.enabled)
        XCTAssertNil(settings.buses[0].dipole.presetID)
        XCTAssertEqual(settings.buses[0].dipole.parameters,parameters)
    }
    func testLegacyDeviceAssignmentMigratesWithoutDiscardingSettings() throws {
        var settings=Settings();settings.buses[0].uid="selected-output"
        let id=try settings.saveDipolePreset(name:"Existing",parameters:DipoleParameters(),bus:0)
        settings.buses[0].dipole.enabled=true
        var json=try XCTUnwrap(JSONSerialization.jsonObject(with:JSONEncoder().encode(settings)) as? [String:Any])
        json.removeValue(forKey:"savedDeviceDipoles")
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".json")
        defer{try? FileManager.default.removeItem(at:url)}
        try JSONSerialization.data(withJSONObject:json).write(to:url)
        let loaded=try Settings.load(from:url)
        XCTAssertEqual(loaded.outputDeviceDipoles["selected-output"]?.presetID,id)
        XCTAssertEqual(loaded.outputDeviceDipoles["selected-output"]?.enabled,true)
        XCTAssertEqual(loaded.buses[0].dipole,settings.buses[0].dipole)
    }
    func testSameDeviceSharesChangesAcrossOutputSlots() {
        var settings=Settings(),previous=settings.buses
        settings.buses[0].uid="same-device";settings.buses[1].uid="same-device"
        settings.synchronizeOutputDeviceDipoles(previous:previous);previous=settings.buses
        settings.buses[1].dipole.enabled=true;settings.buses[1].dipole.parameters.geqDB[5]=2
        settings.synchronizeOutputDeviceDipoles(previous:previous)
        XCTAssertEqual(settings.buses[0].dipole,settings.buses[1].dipole)
        XCTAssertTrue(settings.buses[0].dipole.enabled)
        XCTAssertEqual(settings.outputDeviceDipoles["same-device"],settings.buses[0].dipole)
    }
    func testInactiveDeviceDipoleValidation() {
        var settings=Settings(),invalid=OutputDipoleSettings()
        invalid.presetID=UUID();settings.outputDeviceDipoles["disconnected-device"]=invalid
        XCTAssertThrowsError(try settings.validate())
        invalid.presetID=nil;invalid.parameters.trimDB = .nan
        settings.outputDeviceDipoles["disconnected-device"]=invalid
        XCTAssertThrowsError(try settings.validate())
        settings.outputDeviceDipoles=["":OutputDipoleSettings()]
        XCTAssertThrowsError(try settings.validate())
    }
    func testLegacySettingsDefaultToBypass() throws {
        var json=try XCTUnwrap(JSONSerialization.jsonObject(with:JSONEncoder().encode(Settings())) as? [String:Any])
        json.removeValue(forKey:"savedDipolePresets")
        var buses=try XCTUnwrap(json["buses"] as? [[String:Any]])
        for i in buses.indices { buses[i].removeValue(forKey:"dipoleSettings") }
        json["buses"]=buses
        let copy=try JSONDecoder().decode(Settings.self,from:JSONSerialization.data(withJSONObject:json))
        try copy.validate()
        XCTAssertTrue(copy.buses.allSatisfy{!$0.dipole.enabled})
        XCTAssertTrue(copy.dipolePresets.isEmpty)
    }
    func testNamedPresetRoundTripAndIndependentAssignments() throws {
        var s=Settings(),a=DipoleParameters(),b=DipoleParameters()
        a.spacingCM=10;a.distanceCM=60;a.geqDB[5]=4.2;b.spacingCM=30
        let first=try s.saveDipolePreset(name:"机",parameters:a,bus:0)
        let second=try s.saveDipolePreset(name:"リビング",parameters:b,bus:1)
        s.selectDipolePreset(first,bus:2);s.buses[2].dipole.enabled=true
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".json")
        defer{try? FileManager.default.removeItem(at:url)}
        try s.save(to:url)
        var restored=try Settings.load(from:url)
        XCTAssertEqual(restored.buses[0].dipole.parameters,a)
        XCTAssertEqual(restored.buses[1].dipole.presetID,second)
        XCTAssertEqual(restored.buses[2].dipole.presetID,first)
        XCTAssertTrue(restored.buses[2].dipole.enabled)
        restored.deleteDipolePreset(first);try restored.validate()
        XCTAssertNil(restored.buses[0].dipole.presetID);XCTAssertNil(restored.buses[2].dipole.presetID)
        XCTAssertEqual(restored.buses[2].dipole.parameters,a)
        XCTAssertEqual(restored.buses[1].dipole.presetID,second)
    }
    func testSameNameUpdatesLinkedOutputsAndValidation() throws {
        var s=Settings(),p=DipoleParameters()
        let id=try s.saveDipolePreset(name:"Desk",parameters:p,bus:0)
        s.selectDipolePreset(id,bus:1);p.geqDB[17] = -6
        XCTAssertEqual(try s.saveDipolePreset(name:"desk",parameters:p,bus:2),id)
        XCTAssertEqual(s.dipolePresets.count,1);XCTAssertEqual(s.buses[1].dipole.parameters,p)
        XCTAssertThrowsError(try s.saveDipolePreset(name:"   ",parameters:p,bus:0))
        p.geqDB=[];XCTAssertThrowsError(try p.validate())
        p=DipoleParameters();p.headCM = .nan;XCTAssertThrowsError(try p.validate())
    }
    func testControlCacheBypassAndGainChangesDoNotRebuild() {
        let cache=DipoleControlCache();var s=Settings();var built=[Int](),enabled=[Int:Bool]()
        func apply() { XCTAssertTrue(cache.apply(settings:s,configure:{b,_,_ in built.append(b);return 0},enable:{enabled[$0]=$1})) }
        apply();XCTAssertTrue(built.isEmpty)
        s.buses[1].dipole.enabled=true;apply();XCTAssertEqual(built,[1])
        s.inputs[0].db = -6;apply();XCTAssertEqual(built,[1])
        s.buses[1].dipole.enabled=false;apply();XCTAssertEqual(enabled[1],false)
        s.buses[1].dipole.enabled=true;apply();XCTAssertEqual(built,[1])
        s.buses[1].dipole.parameters.geqDB[0]=3;apply();XCTAssertEqual(built,[1,1])
        s.audio.sampleRate=48000;apply();XCTAssertEqual(built,[1,1,1])
    }
    func testDSPValidationBypassAndVariableBlocksAtAllRates() throws {
        for rate in [44100.0,48000,88200,96000] {
            var c=DipoleParameters().core
            let dsp=try XCTUnwrap(lc_dipole_test_create(&c,rate));defer{lc_dipole_test_destroy(dsp)}
            var input:[Float]=[0.2,-0.1,0.1,0.3]
            lc_dipole_test_process(dsp,&input,2,0);XCTAssertEqual(input,[0.2,-0.1,0.1,0.3])
            var energy:Float=0
            for block in 0..<350 {
                let frames=[1,17,32,255,511,1024][block%6]
                var signal=(0..<frames).flatMap { n -> [Float] in let v=Float(0.01*sin(Double(n+block*1000)*0.37));return [v,-v] }
                lc_dipole_test_process(dsp,&signal,Int32(frames),1)
                XCTAssertTrue(signal.allSatisfy{$0.isFinite})
                energy += signal.reduce(0){$0+abs($1)}
            }
            XCTAssertGreaterThan(energy,1)
            var silence=[Float](repeating:0,count:4096)
            lc_dipole_test_process(dsp,&silence,2048,0)
            silence=Array(repeating:0,count:4096)
            lc_dipole_test_process(dsp,&silence,2048,0)
            XCTAssertTrue(silence.allSatisfy{$0==0})
        }
        var invalid=DipoleParameters().core;invalid.spacingCM=0
        XCTAssertNil(lc_dipole_test_create(&invalid,44100))
    }
    func testDipoleImpulseArrivesWithinTwentyMilliseconds() throws {
        XCTAssertEqual(lc_dipole_latency(),256)
        for rate in [44100.0,48000,88200,96000] {
            var config=DipoleParameters().core
            let dsp=try XCTUnwrap(lc_dipole_test_create(&config,rate))
            defer{lc_dipole_test_destroy(dsp)}
            var warmup=[Float](repeating:0,count:40000)
            lc_dipole_test_process(dsp,&warmup,20000,1)
            var impulse=[Float](repeating:0,count:24000)
            impulse[0]=0.01;impulse[1]=0.01
            lc_dipole_test_process(dsp,&impulse,12000,1)
            let peak=(0..<12000).max{abs(impulse[2*$0])<abs(impulse[2*$1])}!
            XCTAssertLessThan(Double(peak)/rate,0.020)
        }
    }
    func testDipoleDoesNotAutomaticallyAttenuateMonoSignal() throws {
        for boost in [12.0,24.0] {
            var config=DipoleParameters().core
            config.maxBoostDB=boost
            let dsp=try XCTUnwrap(lc_dipole_test_create(&config,44100))
            defer{lc_dipole_test_destroy(dsp)}
            let frames=88200
            var samples=[Float](repeating:0,count:frames*2)
            for i in 0..<frames {
                let value=Float(0.01*sin(2*Double.pi*1000*Double(i)/44100))
                samples[2*i]=value;samples[2*i+1]=value
            }
            lc_dipole_test_process(dsp,&samples,Int32(frames),1)
            let energy=(44100..<frames).reduce(0.0){$0+Double(samples[2*$1])*Double(samples[2*$1])}
            let rms=sqrt(energy/44100)
            XCTAssertEqual(rms,0.01/sqrt(2),accuracy:0.0001)
        }
    }
    func testInputTrimReducesLevelInsteadOfBeingNormalizedAway() throws {
        var normal=DipoleParameters().core,quiet=normal
        quiet.trimDB = -6
        let a=try XCTUnwrap(lc_dipole_test_create(&normal,44100)),b=try XCTUnwrap(lc_dipole_test_create(&quiet,44100))
        defer{lc_dipole_test_destroy(a);lc_dipole_test_destroy(b)}
        var x=[Float](repeating:0.01,count:24000),y=x
        lc_dipole_test_process(a,&x,12000,1);lc_dipole_test_process(b,&y,12000,1)
        XCTAssertEqual(y[22000]/x[22000],Float(pow(10,-6.0/20)),accuracy:0.001)
    }
    func testOutputEffectIsIsolatedFromOtherBuses() throws {
        let engine=try XCTUnwrap(lc_create());defer{lc_destroy(engine)}
        var c=DipoleParameters().core
        c.trimDB = -6
        XCTAssertEqual(lc_output_dipole(engine,0,&c,44100),0);lc_output_dipole_enabled(engine,0,1)
        lc_route(engine,0,0,1);lc_route(engine,0,1,1)
        var last0=[Float](),last1=[Float]()
        for _ in 0..<400 {
            var input=[Float](repeating:0.1,count:64),a=[Float](repeating:0,count:64),b=a
            lc_test_feed(engine,0,&input,32,44100);lc_mix_read(engine,0,&a,32,44100);lc_mix_read(engine,1,&b,32,44100)
            last0=a;last1=b
        }
        XCTAssertEqual(last1[0],0.1,accuracy:0.0001)
        XCTAssertLessThan(last0[0],last1[0])
    }
}

import SwiftUI
import AppKit
final class DipoleEditorSmokeTests:XCTestCase {
    @MainActor func testEditorFitsAndRendersWithoutOpeningAudioDevices() throws {
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString+".json")
        let model=MixerModel(settingsURL:url,deviceAccess:MixerDeviceAccess(enumerate:{[]},readVolume:{_ in nil},writeVolume:{_,_ in 0}))
        model.settings.buses[0].dipole.parameters.geqDB[0]=6
        model.settings.buses[0].dipole.parameters.geqDB[2] = -6
        let view=NSHostingView(rootView:DipoleEditor(model:model,bus:0))
        let size=view.fittingSize
        XCTAssertLessThan(size.width,1180);XCTAssertLessThan(size.height,650)
        view.frame=NSRect(origin:.zero,size:size);view.layoutSubtreeIfNeeded()
        let bitmap=try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in:view.bounds))
        view.cacheDisplay(in:view.bounds,to:bitmap)
        let png=try XCTUnwrap(bitmap.representation(using:.png,properties:[:]))
        try png.write(to:URL(fileURLWithPath:FileManager.default.currentDirectoryPath).appendingPathComponent(".build/dipole-editor-preview.png"))
    }
}
