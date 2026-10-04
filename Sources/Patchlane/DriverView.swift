import SwiftUI
import CoreAudio
import AudioCore
import AppKit

// Versioned CFData format shared with Driver/DriverDSP.hpp. The HAL marshals
// CFPropertyList values across the driver boundary; raw struct properties don't.
struct DriverConfiguration: Equatable {
    var inputGain=[Float](repeating:1,count:4)
    var outputGain=[Float](repeating:1,count:4)
    var routes=(0..<4).map { i in (0..<4).map { i == $0 } }
    var data: Data {
        var words: [UInt32]=[1]
        words += inputGain.map(\.bitPattern)
        words += outputGain.map(\.bitPattern)
        words += routes.flatMap { $0.map { UInt32($0 ? 1:0) } }
        return words.withUnsafeBytes { Data($0) }
    }
    init() {}
    init(data:Data) throws {
        guard data.count==100 else { throw DriverError.message(L("ドライバーの設定形式が一致しません。")) }
        let words: [UInt32]=data.withUnsafeBytes { bytes in (0..<25).map { bytes.loadUnaligned(fromByteOffset:$0*4,as:UInt32.self) } }
        guard words[0]==1 else { throw DriverError.message(L("未対応のドライバーバージョンです。")) }
        let inputs=words[1..<5].map(Float.init(bitPattern:))
        let outputs=words[5..<9].map(Float.init(bitPattern:))
        guard inputs.allSatisfy({ $0.isFinite && $0>=0 && $0<=64 }), outputs.allSatisfy({ $0.isFinite && $0>=0 && $0<=1 }),words[9...].allSatisfy({ $0<=1 }) else {
            throw DriverError.message(L("ドライバーから無効な設定を受信しました。"))
        }
        inputGain=inputs; outputGain=outputs
        routes=(0..<4).map { i in (0..<4).map { words[9+i*4+$0] != 0 } }
    }
}
enum DriverError: LocalizedError {
    case message(String)
    var errorDescription:String? { if case .message(let s)=self { return s }; return nil }
}
// The shared screen owns settings. This model owns only the dedicated lifecycle.
struct DriverConnectionPlan {
    struct Destination { let bus:Int;let device:Device;let left:Int;let right:Int }
    let source:Device
    let input:InputSettings
    let outputs:[Destination]
    let timing:AudioSettings
    init(settings:Settings,devices:[Device]) throws {
        try settings.validate()
        let input=settings.inputs[0];self.input=input;timing=settings.audio
        guard ["audio.patchlane.virtual2","audio.patchlane.virtual8"].contains(input.uid),
              let source=devices.first(where: { $0.uid==input.uid }),
              input.left<source.inputs,input.right<source.inputs else { throw AppError(L("入力1に専用ドライバーと有効なチャンネルを選択してください。")) }
        self.source=source
        outputs=try settings.buses.enumerated().compactMap { bus,value in
            guard !value.uid.isEmpty else { return nil }
            guard value.uid != source.uid,let device=devices.first(where: { $0.uid==value.uid }),
                  value.left<device.outputs,value.right<device.outputs,value.left != value.right else { throw AppError(L("%@ の出力機器・チャンネルを確認してください。",busNames[bus])) }
            return Destination(bus:bus,device:device,left:value.left,right:value.right)
        }
        guard !outputs.isEmpty else { throw AppError(L("出力機器を選択してください。")) }
    }
}
final class DriverModel:ObservableObject {
    static let shared=DriverModel()
    @Published private(set) var connectionIntent=DriverConnectionIntent()
    @Published private(set) var bridgeRunning=false
    @Published private(set) var bridgeBusy=false
    @Published private(set) var bridgeStatus=L("停止中")
    @Published var error:String?
    private let handles=(0..<4).map { _ in lc_shared_output_create()! }
    private let queue=DispatchQueue(label:"Patchlane.driverControl",qos:.userInitiated)
    private let dipoleCache=DipoleControlCache()
    private var dipoleWork:DispatchWorkItem?
    private let preferencesQueue=DispatchQueue(label:"Patchlane.driverPreferences",qos:.utility)
    private var reconnectDetail:String?
    private var reconnectStatus:String {
        guard let detail=reconnectDetail else { return L("再接続中") }
        let prefix="Acquire shared ring: "
        if detail.hasPrefix(prefix),let code=Int64(detail.dropFirst(prefix.count)) {
            return L("再接続中（エラーコード: %@）",String(code))
        }
        return L("再接続中（%@）",detail)
    }
    private var preferredDedicated=false
    private var savedSource="audio.patchlane.virtual2"
    private var generation=UUID()
    private var pendingStart:DispatchWorkItem?
    private var mixerSuspending=false
    private var suspensionToken=UUID()
    private var desired=Settings()
    private var identity:MixerConnectionSettings?
    private var active:[DriverConnectionPlan.Destination]=[]
    private var activeSource:Device?
    private var timer:Timer?
    private var sleepObserver:NSObjectProtocol?
    private var wakeObserver:NSObjectProtocol?
    private var outputHealth=DriverOutputHealth()
    private var nextSourceCheck:Double=0
    // Accessed only on queue, after physical callbacks have stopped.
    private var originalDriver:(AudioDeviceID,Data)?
    init() {
        if let saved=try? DriverPreferences.load() { preferredDedicated=saved.enabled;savedSource=saved.sourceUID }
        sleepObserver=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.willSleepNotification,object:nil,queue:.main) { [weak self] _ in
            guard let self else { return }
            self.connectionIntent.sleep(now:ProcessInfo.processInfo.systemUptime)
            self.stopBridge(preserveIntent:true)
        }
        wakeObserver=NSWorkspace.shared.notificationCenter.addObserver(forName:NSWorkspace.didWakeNotification,object:nil,queue:.main) { [weak self] _ in
            self?.connectionIntent.wake(now:ProcessInfo.processInfo.systemUptime)
        }
        timer=Timer.scheduledTimer(withTimeInterval:1,repeats:true) { [weak self] _ in self?.checkConnection() }
    }
    func restoreAtLaunch() {
        MixerModel.shared.enableAutomaticMixer(dedicated:preferredDedicated)
        if preferredDedicated { startBridge(restoring:true) }
    }
    private func persist() {
        let s=desired
        let first=s.buses.first(where: { !$0.uid.isEmpty })
        let value=DriverPreferences(enabled:preferredDedicated,sourceUID:savedSource,outputUID:first?.uid ?? "",
            left:first?.left ?? 0,right:first?.right ?? 1,frames:s.audio.frames,rate:s.audio.sampleRate,delay:s.audio.delay)
        preferencesQueue.async {
            do { try value.save() } catch { let message=error.localizedDescription;DispatchQueue.main.async { self.error=message } }
        }
    }
    func configure(_ settings:Settings) {
        desired=settings
        applyLevels(settings)
        applyDipoles(settings)
        guard connectionIntent.wanted else { return }
        if identity != MixerConnectionSettings(settings) {
            connectionIntent.interrupt(now:ProcessInfo.processInfo.systemUptime)
            if !mixerSuspending { restart() }
        }
    }
    func startBridge(restoring:Bool=false) {
        guard !connectionIntent.wanted,!bridgeBusy else { return }
        let model=MixerModel.shared
        if !["audio.patchlane.virtual2","audio.patchlane.virtual8"].contains(model.settings.inputs[0].uid) {
            model.settings.inputs[0].uid=savedSource
            model.settings.inputs[0].left=0;model.settings.inputs[0].right=1
        }
        desired=model.settings;savedSource=desired.inputs[0].uid
        preferredDedicated=true;persist();connectionIntent.request()
        if restoring { connectionIntent.interrupt(now:ProcessInfo.processInfo.systemUptime) }
        bridgeBusy=true;mixerSuspending=true
        let suspension=UUID();suspensionToken=suspension
        model.suspendForDriver {
            guard self.suspensionToken==suspension else { return }
            self.mixerSuspending=false
            guard self.connectionIntent.wanted else { return }
            self.restart()
        }
        model.save()
    }
    private static func configuration(_ device:AudioDeviceID)->Data? {
        var address=AudioObjectPropertyAddress(mSelector:0x6c636667,mScope:kAudioObjectPropertyScopeGlobal,mElement:0)
        var value:Unmanaged<CFData>?;var size=UInt32(MemoryLayout<Unmanaged<CFData>?>.size)
        guard AudioObjectGetPropertyData(device,&address,0,nil,&size,&value)==noErr,let value else { return nil }
        return value.takeRetainedValue() as Data
    }
    private static func setConfiguration(_ data:Data,device:AudioDeviceID) throws {
        var address=AudioObjectPropertyAddress(mSelector:0x6c636667,mScope:kAudioObjectPropertyScopeGlobal,mElement:0)
        let cf=data as CFData;var ref=Unmanaged.passUnretained(cf).toOpaque()
        let status=withExtendedLifetime(cf) { AudioObjectSetPropertyData(device,&address,0,nil,UInt32(MemoryLayout.size(ofValue:ref)),&ref) }
        guard status==noErr else { throw AppError(L("専用ドライバーの設定に失敗しました（%d）。",status)) }
    }
    private func stopOutputs() {
        for h in handles.reversed() { lc_shared_output_stop(h) }
        if let (device,data)=originalDriver {
            if Self.configuration(device)==DriverConfiguration().data { try? Self.setConfiguration(data,device:device) }
            originalDriver=nil
        }
    }
    private func restart() {
        guard connectionIntent.wanted,!connectionIntent.sleeping,!mixerSuspending else { return }
        let token=UUID();generation=token;pendingStart?.cancel()
        let settings=desired
        identity=MixerConnectionSettings(settings)
        bridgeBusy=true;bridgeRunning=false;bridgeStatus=connectionIntent.recovering ? reconnectStatus:L("接続中");error=nil
        queue.async { self.stopOutputs() }
        let work=DispatchWorkItem {
            do {
                let plan=try DriverConnectionPlan(settings:settings,devices:MixerModel.enumerate())
                guard let old=Self.configuration(plan.source.id) else { throw AppError(L("専用ドライバーの設定を取得できません。")) }
                self.originalDriver=(plan.source.id,old)
                // Gain/routing now share the app's input/output controls. The
                // HAL publishes unchanged channel pairs for these readers.
                try Self.setConfiguration(DriverConfiguration().data,device:plan.source.id)
                for out in plan.outputs {
                    let h=self.handles[out.bus]
                    guard lc_shared_output_channels(h,Int32(plan.input.left),Int32(plan.input.right))==0 else { throw AppError(L("入力チャンネルの設定に失敗しました。")) }
                    guard lc_shared_output_start_source(h,plan.source.id,out.device.id,0,Int32(out.left),Int32(out.right),Int32(plan.timing.frames),Double(plan.timing.sampleRate),Double(plan.timing.bufferDelayFrames))==0 else {
                        throw AppError(String(cString:lc_shared_output_error(h)))
                    }
                }
                DispatchQueue.main.async {
                    guard self.generation==token else { return }
                    self.active=plan.outputs;self.activeSource=plan.source
                    self.bridgeBusy=false;self.bridgeRunning=true;self.connectionIntent.succeeded()
                    self.reconnectDetail=nil;self.bridgeStatus=L("動作中");self.outputHealth=DriverOutputHealth()
                    self.nextSourceCheck=ProcessInfo.processInfo.systemUptime+10
                    self.savedSource=settings.inputs[0].uid;self.persist()
                }
            } catch {
                let message=error.localizedDescription
                self.stopOutputs()
                DispatchQueue.main.async {
                    guard self.generation==token else { return }
                    self.bridgeBusy=false;self.bridgeRunning=false;self.error=nil
                    self.reconnectDetail=message
                    // Retain dedicated ownership so a corrected selection can reconnect.
                    self.connectionIntent.interrupt(now:ProcessInfo.processInfo.systemUptime)
                    self.bridgeStatus=self.reconnectStatus
                }
            }
        }
        pendingStart=work
        queue.asyncAfter(deadline:.now()+0.12,execute:work)
        applyLevels(settings)
        applyDipoles(settings)
    }
    private func applyDipoles(_ settings:Settings) {
        dipoleWork?.cancel()
        let work=DispatchWorkItem { [weak self] in
            guard let self else { return }
            let ok=self.dipoleCache.apply(settings:settings,
                configure:{ bus,c,rate in lc_shared_output_dipole(self.handles[bus],&c,rate) },
                enable:{ bus,on in lc_shared_output_dipole_enabled(self.handles[bus],on ? 1:0) })
            if !ok { DispatchQueue.main.async { self.error=L("ダイポールFIRを生成できませんでした。") } }
        }
        dipoleWork=work;queue.asyncAfter(deadline:.now()+0.08,execute:work)
    }
    private func applyLevels(_ settings:Settings) {
        let input=settings.inputs[0]
        let gain:Float=input.muted || input.db <= -60 ? 0:Float(pow(10,input.db/20))
        for i in 0..<4 { lc_shared_output_levels(handles[i],gain,input.routes[i] ? 1:0,settings.buses[i].muted ? 1:0) }
    }
    func peaks()->(Float,[Float]) {
        var input:Float=0;var outputs=[Float](repeating:0,count:4)
        for i in 0..<4 {
            let value=lc_shared_output_input_peak(handles[i]),out=lc_shared_output_peak(handles[i])
            if bridgeRunning { input=max(input,value);outputs[i]=out }
        }
        return (input,outputs)
    }
    func stopBridge(preserveIntent:Bool=false) {
        if !preserveIntent { reconnectDetail=nil;error=nil;preferredDedicated=false;connectionIntent.stop();suspensionToken=UUID();mixerSuspending=false;persist() }
        pendingStart?.cancel();let token=UUID();generation=token;identity=nil
        bridgeBusy=true;bridgeRunning=false;bridgeStatus=preserveIntent ? reconnectStatus:L("停止中")
        queue.async {
            self.stopOutputs()
            DispatchQueue.main.async {
                guard self.generation==token else { return }
                self.bridgeBusy=false;self.active=[];self.activeSource=nil
                if !self.connectionIntent.wanted { MixerModel.shared.resumeFromDriver() }
            }
        }
    }
    private func checkConnection() {
        guard !bridgeBusy else { return }
        let now=ProcessInfo.processInfo.systemUptime
        if !bridgeRunning {
            if connectionIntent.shouldRetry(now:now) { connectionIntent.attempting(now:now);restart() }
            return
        }
        let unhealthy=outputHealth.observe(active.map {
            let h=handles[$0.bus]
            return DriverOutputHealth.Sample(callbacks:lc_shared_output_callbacks(h),
                missing:lc_shared_output_missing_frames(h),received:lc_shared_output_received_frames(h))
        })
        let devices=MixerModel.shared.devices
        let missingDevice=active.contains { out in devices.first(where: { $0.uid==out.device.uid })?.id != out.device.id }
        let changedSource=devices.first(where: { $0.uid==activeSource?.uid })?.id != activeSource?.id
        if unhealthy || missingDevice || changedSource || active.contains(where: { lc_shared_output_needs_reconnect(handles[$0.bus]) != 0 }) {
            connectionIntent.interrupt(now:now);stopBridge(preserveIntent:true);return
        }
        let missing=active.reduce(UInt64(0)) { total,out in
            let h=handles[out.bus],all=lc_shared_output_missing_frames(h),initial=lc_shared_output_initial_missing_frames(h)
            return total+(all>=initial ? all-initial:0)
        }
        bridgeStatus=missing==0 ? L("動作中"):L("不足 %llu フレーム",missing)
        if now>=nextSourceCheck {
            nextSourceCheck=now+10
            let indices=active.map(\.bus)
            queue.async { for i in indices { _=lc_shared_output_refresh_source(self.handles[i]) } }
        }
    }
    func shutdownBridge() {
        persist();pendingStart?.cancel();dipoleWork?.cancel();connectionIntent.stop();generation=UUID();suspensionToken=UUID()
        queue.sync { stopOutputs() };preferencesQueue.sync {}
    }
}
