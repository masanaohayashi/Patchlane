import Foundation
import AudioCore

struct DipoleParameters: Codable, Equatable {
    var spacingCM: Double = 20
    var distanceCM: Double = 100
    var headCM: Double = 17.5
    var maxBoostDB: Double = 12
    var tonalReference: Int = 1
    var trimDB: Double = 0
    var geqDB = [Double](repeating: 0,count: 31)
    static let frequencies: [Double] = [20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000]
    func validate() throws {
        guard spacingCM.isFinite,distanceCM.isFinite,headCM.isFinite,maxBoostDB.isFinite,trimDB.isFinite,
              (2...200).contains(spacingCM),(20...500).contains(distanceCM),(10...25).contains(headCM),
              (0...24).contains(maxBoostDB),(0...2).contains(tonalReference),(-12...12).contains(trimDB),
              atan2(spacingCM/2,distanceCM)<=Double.pi/4,geqDB.count==31,
              geqDB.allSatisfy({$0.isFinite && (-12...12).contains($0)}) else { throw AppError(L("ダイポール設定が無効です。")) }
    }
    var core: LCDipoleConfig {
        var c=LCDipoleConfig()
        c.spacingCM=spacingCM;c.distanceCM=distanceCM;c.headCM=headCM;c.maxBoostDB=maxBoostDB;c.trimDB=trimDB;c.tonalReference=Int32(tonalReference)
        withUnsafeMutableBytes(of: &c.geqDB) { bytes in
            let p=bytes.bindMemory(to:Double.self)
            for i in 0..<min(31,geqDB.count) { p[i]=geqDB[i] }
        }
        return c
    }
}
struct OutputDipoleSettings: Codable, Equatable {
    var enabled=false
    var presetID: UUID? = nil
    var parameters=DipoleParameters()
}
struct DipolePreset: Codable, Equatable, Identifiable {
    var id=UUID()
    var name: String
    var parameters: DipoleParameters
}
extension Settings {
    // Stable Core Audio UIDs survive reconnects and changing numeric device IDs.
    // One physical output has one remembered state, independent of Main/Aux.
    mutating func captureOutputDeviceDipoles() {
        var remembered=outputDeviceDipoles,seen=Set<String>()
        for bus in buses where !bus.uid.isEmpty && seen.insert(bus.uid).inserted {
            remembered[bus.uid]=bus.dipole
        }
        outputDeviceDipoles=remembered
    }
    mutating func restoreOutputDeviceDipoles() {
        var remembered=outputDeviceDipoles
        for i in buses.indices where !buses[i].uid.isEmpty {
            let uid=buses[i].uid
            if let state=remembered[uid] { buses[i].dipole=state }
            else { remembered[uid]=buses[i].dipole } // Migrate the selected output in old files.
        }
        outputDeviceDipoles=remembered
    }
    mutating func synchronizeOutputDeviceDipoles(previous:[BusSettings]) {
        guard previous.count==buses.count else { restoreOutputDeviceDipoles();return }
        var remembered=outputDeviceDipoles,edited=Set<String>()
        // Capture the old association before restoring a newly selected device.
        for bus in previous where !bus.uid.isEmpty && remembered[bus.uid]==nil { remembered[bus.uid]=bus.dipole }
        for i in buses.indices where !buses[i].uid.isEmpty && buses[i].uid==previous[i].uid && buses[i].dipole != previous[i].dipole {
            remembered[buses[i].uid]=buses[i].dipole;edited.insert(buses[i].uid)
        }
        for i in buses.indices {
            let uid=buses[i].uid
            if uid != previous[i].uid {
                buses[i].dipole=remembered[uid] ?? OutputDipoleSettings()
                if !uid.isEmpty { remembered[uid]=buses[i].dipole }
            } else if edited.contains(uid),let state=remembered[uid] {
                buses[i].dipole=state
            }
        }
        outputDeviceDipoles=remembered
    }
    mutating func selectDipolePreset(_ id:UUID?,bus:Int) {
        guard buses.indices.contains(bus) else { return }
        buses[bus].dipole.presetID=id
        if let preset=dipolePresets.first(where:{$0.id==id}) { buses[bus].dipole.parameters=preset.parameters }
    }
    @discardableResult mutating func saveDipolePreset(name:String,parameters:DipoleParameters,bus:Int) throws -> UUID {
        let name=name.trimmingCharacters(in:.whitespacesAndNewlines)
        guard !name.isEmpty,name.count<=80,buses.indices.contains(bus) else { throw AppError(L("プリセット名は1〜80文字で入力してください。最大128件保存できます。")) }
        try parameters.validate()
        if let index=dipolePresets.firstIndex(where:{$0.name.caseInsensitiveCompare(name) == .orderedSame}) {
            let id=dipolePresets[index].id
            dipolePresets[index].parameters=parameters
            for i in buses.indices where buses[i].dipole.presetID==id { buses[i].dipole.parameters=parameters }
            var remembered=outputDeviceDipoles
            for uid in remembered.keys where remembered[uid]?.presetID==id { remembered[uid]?.parameters=parameters }
            outputDeviceDipoles=remembered
            selectDipolePreset(id,bus:bus);return id
        }
        guard dipolePresets.count<128 else { throw AppError(L("プリセット名は1〜80文字で入力してください。最大128件保存できます。")) }
        let preset=DipolePreset(name:name,parameters:parameters)
        dipolePresets.append(preset);selectDipolePreset(preset.id,bus:bus)
        return preset.id
    }
    mutating func deleteDipolePreset(_ id:UUID) {
        dipolePresets.removeAll {$0.id==id}
        for i in buses.indices where buses[i].dipole.presetID==id { buses[i].dipole.presetID=nil }
        var remembered=outputDeviceDipoles
        for uid in remembered.keys where remembered[uid]?.presetID==id { remembered[uid]?.presetID=nil }
        outputDeviceDipoles=remembered
    }
}

// Owned by a serial control queue. On/off uses an atomic and does not synthesize filters.
final class DipoleControlCache {
    private struct Identity:Equatable { var parameters:DipoleParameters;var rate:Double }
    private var current=[Int:Identity]()
    func apply(settings:Settings,configure:(Int,inout LCDipoleConfig,Double)->Int32,enable:(Int,Bool)->Void) -> Bool {
        var success=true
        for bus in 0..<4 {
            let output=settings.buses[bus].dipole
            guard output.enabled else { enable(bus,false);continue }
            let identity=Identity(parameters:output.parameters,rate:Double(settings.audio.sampleRate))
            if current[bus] != identity {
                var config=output.parameters.core
                if configure(bus,&config,identity.rate)==0 { current[bus]=identity }
                else { enable(bus,false);success=false;continue }
            }
            enable(bus,true)
        }
        return success
    }
}
