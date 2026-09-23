import AppKit
import AVFoundation
import AudioCore
import Combine

struct Device: Identifiable, Equatable {
    var id: UInt32
    var uid: String
    var name: String
    var inputs: Int
    var outputs: Int
    var rate: Double
}
// Injectable control-plane operations; tests can simulate a stalled HAL without
// opening audio devices or depending on the user's installed drivers.
struct MixerDeviceAccess {
    var enumerate:()->[Device] = MixerModel.enumerate
    var readVolume:(Device)->DeviceVolume.Control? = DeviceVolume.read
    var writeVolume:(DeviceVolume.Control,Double)->OSStatus = DeviceVolume.write
}
final class MixerModel: ObservableObject {
    static let shared = MixerModel()
    @Published var settings = Settings()
    @Published var devices: [Device] = []
    @Published var deviceVolumes: [String:Double] = [:]
    @Published var running = false
    @Published var error: String?
    @Published var inputPeaks = [Float](repeating:0,count:4)
    @Published var outputPeaks = [Float](repeating:0,count:4)
    @Published private(set) var dedicatedMode=false
    private var lifecycle=AudioModeLifecycle()
    private let settingsURL:URL
    private let deviceAccess:MixerDeviceAccess
    private var permissionPending=false
    func enableAutomaticMixer(dedicated:Bool=false) { lifecycle.dedicated=dedicated; dedicatedMode=dedicated; lifecycle.enabled=true; start() }
    func suspendForDriver(completion:@escaping ()->Void) { lifecycle.dedicated=true; dedicatedMode=true; stop(completion:completion) }
    func resumeFromDriver() {
        lifecycle.dedicated=false; dedicatedMode=false
        refreshDevices(); start()
    }
    private let engine = lc_create()!
    // Device lifecycle only. Core Audio owns the independent realtime callbacks.
    private let controlQueue = DispatchQueue(label:"Patchlane.mixerControl",qos:.userInitiated)
    private let persistenceQueue = DispatchQueue(label:"Patchlane.mixerPreferences",qos:.utility)
    private var startPending=false
    private var connectionSettings:MixerConnectionSettings?
    private var devicesInitialized=false
    private var deviceRefreshPending=false
    private var volumeRefreshPending=false
    private var volumeRevision:UInt64=0
    private var controlGeneration=UUID()
    private var meters: Timer?
    private var watcher: Timer?
    private var volumeWatcher: Timer?
    private var saveWork: DispatchWorkItem?
    private var sleepToken: NSObjectProtocol?
    private var wakeToken: NSObjectProtocol?
    init(settingsURL:URL=Settings.file,deviceAccess:MixerDeviceAccess=MixerDeviceAccess()) {
        self.settingsURL=settingsURL;self.deviceAccess=deviceAccess
        signal(SIGPIPE,SIG_IGN)
        if FileManager.default.fileExists(atPath:settingsURL.path) {
            do { settings=try Settings.load(from:settingsURL) } catch { self.error=L("保存済み設定を読み込めませんでした: %@",error.localizedDescription) }
        }
        refreshDevices()
        refreshDeviceVolumes()
        volumeWatcher=Timer.scheduledTimer(withTimeInterval:0.25,repeats:true) { [weak self] _ in self?.refreshDeviceVolumes() }
        meters=Timer.scheduledTimer(withTimeInterval:1.0/30,repeats:true) { [weak self] _ in self?.updateMeters() }
        watcher=Timer.scheduledTimer(withTimeInterval:2,repeats:true) { [weak self] _ in self?.refreshDevices() }
        sleepToken=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.willSleepNotification,object:nil,queue:.main) { [weak self] _ in
            guard let self=self else { return }; self.lifecycle.sleeping=true; self.stop()
        }
        wakeToken=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.didWakeNotification,object:nil,queue:.main) { [weak self] _ in
            DispatchQueue.main.asyncAfter(deadline:.now()+2) { guard let self=self else { return }; self.lifecycle.sleeping=false; self.refreshDevices(); self.start() }
        }
    }
    static func enumerate() -> [Device] {
        let capacity=max(64,Int(lc_devices(nil,0))+16)
        var list=[LCDevice](repeating:LCDevice(),count:capacity)
        let count=min(capacity,Int(lc_devices(&list,Int32(capacity))))
        return list.prefix(count).map { d in
            var d=d
            let name=withUnsafePointer(to:&d.name) { $0.withMemoryRebound(to:CChar.self,capacity:256) { String(cString:$0) } }
            let uid=withUnsafePointer(to:&d.uid) { $0.withMemoryRebound(to:CChar.self,capacity:256) { String(cString:$0) } }
            return Device(id:d.id,uid:uid,name:name,inputs:Int(d.inputs),outputs:Int(d.outputs),rate:d.rate)
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
    func refreshDevices() {
        guard !deviceRefreshPending,!lifecycle.shuttingDown else { return }
        deviceRefreshPending=true
        // HAL queries can block on a driver. Never query from the main run loop.
        controlQueue.async {
            let fresh=self.deviceAccess.enumerate()
            DispatchQueue.main.async {
                self.deviceRefreshPending=false
                guard !self.lifecycle.shuttingDown else { return }
                self.devicesInitialized=true
                let used=Set(self.settings.inputs.map(\.uid)+self.settings.buses.map(\.uid)).subtracting([""])
                let previous=self.devices.filter { used.contains($0.uid) }
                let current=fresh.filter { used.contains($0.uid) }
                if fresh != self.devices { self.devices=fresh }
                // A start owns timing changes until it publishes the resulting
                // device snapshot. Do not restart in response to its own setup.
                if self.running && previous != current { self.stop() }
                if self.lifecycle.shouldRunMixer && !self.running { self.start() }
                self.refreshDeviceVolumes()
            }
        }
    }
    func device(_ uid:String) -> Device? { devices.first { $0.uid == uid } }
    func refreshDeviceVolumes() {
        guard !volumeRefreshPending,!lifecycle.shuttingDown else { return }
        let selected=Set(settings.buses.map(\.uid)).subtracting([""])
        let outputs=devices.filter { selected.contains($0.uid) }
        guard !outputs.isEmpty else { if !deviceVolumes.isEmpty { deviceVolumes=[:] };return }
        let revision=volumeRevision
        volumeRefreshPending=true
        controlQueue.async {
            var fresh:[String:Double]=[:]
            for device in outputs {
                if let control=self.deviceAccess.readVolume(device) { fresh[device.uid]=control.scalar }
            }
            DispatchQueue.main.async {
                self.volumeRefreshPending=false
                guard !self.lifecycle.shuttingDown,self.volumeRevision==revision,
                      Set(self.settings.buses.map(\.uid)).subtracting([""])==selected else { return }
                if fresh != self.deviceVolumes { self.deviceVolumes=fresh }
            }
        }
    }
    func setDeviceVolume(_ uid:String, scalar:Double) {
        guard let device=device(uid) else { return }
        volumeRevision &+= 1
        deviceVolumes[uid]=scalar
        controlQueue.async {
            guard let control=self.deviceAccess.readVolume(device) else {
                DispatchQueue.main.async { self.refreshDeviceVolumes() };return
            }
            let status=self.deviceAccess.writeVolume(control,scalar)
            DispatchQueue.main.async {
                if status != 0 { self.error=L("機器の音量を変更できませんでした（%d）。",status) }
                self.refreshDeviceVolumes()
            }
        }
    }
    func changed(reconfigure:Bool=false) {
        if lifecycle.dedicated { DriverModel.shared.configure(settings) }
        if reconfigure { refreshDeviceVolumes() }
        applyLevels()
        if reconfigure && lifecycle.shouldRunMixer && connectionSettings != MixerConnectionSettings(settings) { stop(); start() }
        saveWork?.cancel()
        let work=DispatchWorkItem { [weak self] in self?.save() }; saveWork=work
        DispatchQueue.main.asyncAfter(deadline:.now()+0.5,execute:work)
    }
    func save() {
        let snapshot=settings,url=settingsURL
        persistenceQueue.async {
            do { try snapshot.save(to:url) }
            catch { let message=error.localizedDescription;DispatchQueue.main.async { self.error=message } }
        }
    }
    func applyLevels() {
        for i in 0..<4 {
            let s=settings.inputs[i]; lc_input_gain(engine,Int32(i),s.muted || s.db <= -60 ? 0:Float(pow(10,s.db/20)))
            let b=settings.buses[i]; lc_output_gain(engine,Int32(i),b.muted ? 0:1)
            for bus in 0..<4 { lc_route(engine,Int32(i),Int32(bus),s.routes[bus] ? 1:0) }
        }
    }
    func start() {
        guard lifecycle.shouldRunMixer, !running, !startPending, !permissionPending else { return }
        guard devicesInitialized else { refreshDevices();return }
        if settings.inputs.contains(where: { !$0.uid.isEmpty }) {
            switch AVCaptureDevice.authorizationStatus(for:.audio) {
            case .notDetermined:
                permissionPending=true
                AVCaptureDevice.requestAccess(for:.audio) { granted in DispatchQueue.main.async { self.permissionPending=false; if granted { self.start() } else { self.error=L("マイクへのアクセスが許可されていません。システム設定 → プライバシーとセキュリティ → マイクを確認してください。") } } }; return
            case .denied,.restricted: error=L("システム設定 → プライバシーとセキュリティ → マイクで Patchlane を許可してください。"); return
            default: break
            }
        }
        for s in settings.inputs where !s.uid.isEmpty {
            guard let d=device(s.uid),d.inputs>0 else { error=L("入力機器が接続されていません。選択を確認してください。"); return }
        }
        for s in settings.buses where !s.uid.isEmpty {
            guard let d=device(s.uid),d.outputs>0 else { error=L("出力機器が接続されていません。選択を確認してください。"); return }
        }
        applyLevels()
        let ins=settings.inputs,outs=settings.buses,ds=devices
        let timing=settings.audio
        let generation=UUID();controlGeneration=generation;startPending=true
        connectionSettings=MixerConnectionSettings(settings)
        controlQueue.async {
            let result: Int32 = {
                guard lc_config_delay(self.engine,Int32(timing.delay))==0,
                      lc_config_timing(self.engine,Int32(timing.frames),Double(timing.sampleRate))==0 else { return -1 }
                for i in 0..<4 {
                    let s=ins[i],d=ds.first { $0.uid==s.uid }
                    lc_config_input(self.engine,Int32(i),d?.id ?? 0,Int32(s.left),Int32(s.right))
                    guard lc_config_output_channels(self.engine,Int32(i),Int32(outs[i].left),Int32(outs[i].right))==0 else { return -1 }
                    lc_config_output(self.engine,Int32(i),ds.first { $0.uid==outs[i].uid }?.id ?? 0)
                }
                return lc_start(self.engine)
            }()
            let message=result == 0 ? nil:String(cString:lc_error(self.engine))
            let devices=self.deviceAccess.enumerate()
            DispatchQueue.main.async {
                guard self.controlGeneration==generation else { return }
                self.startPending=false
                if result != 0 { self.connectionSettings=nil;self.error=message;return }
                self.devices=devices;self.running=true;self.error=nil;self.save()
            }
        }
    }
    func stop(completion:(()->Void)?=nil) {
        controlGeneration=UUID();startPending=false;running=false;connectionSettings=nil
        inputPeaks=Array(repeating:0,count:4); outputPeaks=Array(repeating:0,count:4)
        controlQueue.async {
            lc_stop(self.engine)
            if let completion { DispatchQueue.main.async(execute:completion) }
        }
    }
    private func updateMeters() {
        if dedicatedMode {
            let (input,outputs)=DriverModel.shared.peaks()
            inputPeaks[0]=max(input,inputPeaks[0]*0.84)
            for i in 1..<4 { inputPeaks[i]=0 }
            for i in 0..<4 { outputPeaks[i]=max(outputs[i],outputPeaks[i]*0.84) }
            return
        }
        guard running else { return }
        for i in 0..<4 {
            inputPeaks[i]=max(lc_input_peak(engine,Int32(i)),inputPeaks[i]*0.84)
            outputPeaks[i]=max(lc_output_peak(engine,Int32(i)),outputPeaks[i]*0.84)
        }
    }
    func shutdown() {
        lifecycle.shuttingDown=true;saveWork?.cancel();save();stop()
        // Termination waits for teardown; audio callbacks never wait on these queues.
        controlQueue.sync {};persistenceQueue.sync {}
    }
}
