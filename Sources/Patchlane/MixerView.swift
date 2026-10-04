import SwiftUI

private let windowBackground=Color(nsColor:.windowBackgroundColor)
private let panel=Color(nsColor:.controlBackgroundColor)
private let separator=Color(nsColor:.separatorColor)

struct MixerView: View {
    @ObservedObject var model: MixerModel
    @State private var dipoleBus:Int?
    @ObservedObject private var driver=DriverModel.shared
    private var usesDriver:Bool { model.dedicatedMode || driver.connectionIntent.wanted }
    var body: some View {
        VStack(spacing:0) {
            HStack {
                Spacer()
                Circle().fill(model.running ? Color.primary:Color.secondary).frame(width:6,height:6)
                Text(usesDriver ? "DRIVER MODE · \(driver.bridgeStatus)":model.running ? L("ミキサー動作中"):L("音声機器を確認してください"))
            }.font(.system(size:11)).foregroundStyle(.secondary).padding(.horizontal,24).padding(.top,8)
            audioSettings.padding(.horizontal,24).padding(.vertical,14)
            Divider()
            mixer.padding(18)

        }.background(windowBackground)
        .frame(minWidth:1080)
        .fixedSize(horizontal:false,vertical:true)
        .sheet(isPresented:Binding(get:{dipoleBus != nil},set:{if !$0{dipoleBus=nil}})) { if let bus=dipoleBus { DipoleEditor(model:model,bus:bus) } }
        .alert(L("確認してください"),isPresented:Binding(get:{model.error != nil || driver.error != nil},set:{if !$0 { model.error=nil;driver.error=nil }})) { Button("OK") { model.error=nil } } message: { Text(model.error ?? driver.error ?? "") }
    }
    private var mixer: some View {
        VStack(alignment:.leading,spacing:14) {
            sectionTitle(L("入力"))
            HStack(alignment:.top,spacing:14) { ForEach(0..<4) { i in inputCard(i).disabled(usesDriver && i>0).opacity(usesDriver && i>0 ? 0.45:1) } }
            sectionTitle(L("出力"))
            HStack(alignment:.top,spacing:14) { ForEach(0..<4) { i in outputCard(i) } }
        }
    }
    private func sectionTitle(_ title:String)->some View {
        Text(title).font(.headline)
    }
    private func inputCard(_ i:Int)->some View {
        VStack(alignment:.leading,spacing:10) {
            HStack { Text(L("入力 %d",i+1)).font(.system(size:14,weight:.semibold)); Spacer(); mute($model.settings.inputs[i].muted) }
            devicePicker(selection:$model.settings.inputs[i].uid,input:true,dedicatedOnly:usesDriver && i==0)
                .onChange(of:model.settings.inputs[i].uid) { uid in let count=model.device(uid)?.inputs ?? 2; model.settings.inputs[i].left=0; model.settings.inputs[i].right=min(1,max(0,count-1)); model.changed(reconfigure:true) }
            HStack(spacing:6) {
                channelPicker("L",selection:$model.settings.inputs[i].left,count:model.device(model.settings.inputs[i].uid)?.inputs ?? 2)
                channelPicker("R",selection:$model.settings.inputs[i].right,count:model.device(model.settings.inputs[i].uid)?.inputs ?? 2)
            }.onChange(of:model.settings.inputs[i].left) { _ in model.changed(reconfigure:true) }.onChange(of:model.settings.inputs[i].right) { _ in model.changed(reconfigure:true) }
            Meter(peak:model.inputPeaks[i])
            volume($model.settings.inputs[i].db,range:-60...36)
            VStack(alignment:.leading,spacing:8) {
                Text("SEND TO").font(.system(size:9,weight:.medium,design:.monospaced)).tracking(1).foregroundStyle(.secondary)
                HStack(spacing:5) {
                    ForEach(0..<4) { b in
                        Toggle(busNames[b],isOn:Binding(
                            get:{ model.settings.inputs[i].routes[b] },
                            set:{ model.settings.inputs[i].routes[b]=$0; model.changed() }
                        )).toggleStyle(.button).controlSize(.small).frame(maxWidth:.infinity)
                            .accessibilityLabel(L("入力 %d を %@ に送る",i+1,busNames[b]))

                    }
                }
            }
        }.padding(12).frame(maxWidth:.infinity).background(panel).clipShape(RoundedRectangle(cornerRadius:8)).overlay(RoundedRectangle(cornerRadius:8).strokeBorder(separator,lineWidth:0.5))
    }
    private func outputCard(_ i:Int)->some View {
        VStack(alignment:.leading,spacing:10) {
            HStack { Text(busNames[i]).font(.system(size:14,weight:.semibold)); Spacer(); mute($model.settings.buses[i].muted) }
            devicePicker(selection:$model.settings.buses[i].uid,input:false).onChange(of:model.settings.buses[i].uid) { uid in
                let count=model.device(uid)?.outputs ?? 2
                if model.settings.buses[i].left>=count || model.settings.buses[i].right>=count { model.settings.buses[i].left=0;model.settings.buses[i].right=min(1,max(0,count-1)) }
                model.changed(reconfigure:true)
            }
            HStack(spacing:6) {
                channelPicker("L",selection:$model.settings.buses[i].left,count:model.device(model.settings.buses[i].uid)?.outputs ?? 2)
                channelPicker("R",selection:$model.settings.buses[i].right,count:model.device(model.settings.buses[i].uid)?.outputs ?? 2)
            }.onChange(of:model.settings.buses[i].left) { _ in model.changed(reconfigure:true) }.onChange(of:model.settings.buses[i].right) { _ in model.changed(reconfigure:true) }
            Meter(peak:model.outputPeaks[i])
            deviceVolume(model.settings.buses[i].uid)
            HStack {
                Toggle("Dipole",isOn:Binding(get:{model.settings.buses[i].dipole.enabled},set:{model.settings.buses[i].dipole.enabled=$0;model.changed()})).toggleStyle(.switch).controlSize(.mini)
                Spacer()
                Button { dipoleBus=i } label: { Image(systemName:"slider.horizontal.3") }.help(L("ダイポールとGEQの設定"))
            }
            Picker(L("プリセット"),selection:Binding<UUID?>(get:{model.settings.buses[i].dipole.presetID},set:{model.settings.selectDipolePreset($0,bus:i);model.changed()})) {
                Text(L("カスタム")).tag(UUID?.none)
                ForEach(model.settings.dipolePresets){preset in Text(preset.name).tag(Optional(preset.id))}
            }.labelsHidden().controlSize(.small)

        }.padding(12).frame(maxWidth:.infinity).background(panel).clipShape(RoundedRectangle(cornerRadius:8)).overlay(RoundedRectangle(cornerRadius:8).strokeBorder(separator,lineWidth:0.5))
    }
    private func deviceVolume(_ uid:String)->some View {
        let scalar=model.deviceVolumes[uid]
        return VStack(spacing:4) {
            HStack {
                Text("LEVEL").font(.system(size:9,design:.monospaced)).foregroundStyle(.secondary)
                Spacer()
                Text(scalar.map { String(format:"%.0f%%",$0*100) } ?? L("変更不可")).font(.system(size:11,design:.monospaced))
            }
            Slider(value:Binding(get:{ model.deviceVolumes[uid] ?? 0 },set:{ model.setDeviceVolume(uid,scalar:$0) }),in:0...1)
                .controlSize(.small).disabled(scalar == nil).accessibilityLabel(L("出力機器の音量"))
        }.help(scalar == nil ? L("この機器の音量は変更できません") : L("機器自身の音量を変更します。macOSや他のアプリと共通です"))
    }
    private func mute(_ binding:Binding<Bool>)->some View {
        Button { binding.wrappedValue.toggle(); model.changed() } label: { Image(systemName:binding.wrappedValue ? "speaker.slash.fill":"speaker.wave.1").foregroundStyle(binding.wrappedValue ? Color.red:.secondary).frame(width:24,height:20) }.buttonStyle(.plain).help(L("ミュート")).accessibilityLabel(L("ミュート")).accessibilityValue(binding.wrappedValue ? L("オン"):L("オフ"))
    }
    private func volume(_ binding:Binding<Double>,range:ClosedRange<Double>)->some View {
        VStack(spacing:4) {
            HStack { Text("LEVEL").font(.system(size:9,design:.monospaced)).foregroundStyle(.secondary); Spacer(); Text(binding.wrappedValue <= -60 ? "−∞ dB":String(format:"%+.1f dB",binding.wrappedValue)).font(.system(size:11,design:.monospaced)).onTapGesture(count:2) { binding.wrappedValue=0; model.changed() }.help(L("ダブルクリックで 0 dB")) }
            Slider(value:Binding(get:{ binding.wrappedValue },set:{ binding.wrappedValue=GainSlider.decibels($0) }),in:range).controlSize(.small).onChange(of:binding.wrappedValue) { _ in model.changed() }.accessibilityLabel(L("音量 dB"))
        }
    }
    private func devicePicker(selection:Binding<String>,input:Bool,dedicatedOnly:Bool=false)->some View {
        Picker(L("機器"),selection:selection) {
            Text(input ? L("入力なし"):L("出力なし")).tag("")
            ForEach(model.devices.filter { (input ? $0.inputs>0:$0.outputs>0) && (!dedicatedOnly || ["audio.patchlane.virtual2","audio.patchlane.virtual8"].contains($0.uid)) }) { d in Text(d.name).tag(d.uid) }
            if !selection.wrappedValue.isEmpty && model.device(selection.wrappedValue)==nil { Text(L("未接続の機器")).tag(selection.wrappedValue) }
        }.labelsHidden().controlSize(.small).frame(maxWidth:.infinity).accessibilityLabel(input ? L("入力機器"):L("出力機器"))
    }
    private func channelPicker(_ name:String,selection:Binding<Int>,count:Int)->some View {
        HStack(spacing:4) { Text(name).font(.system(size:10,design:.monospaced)).foregroundStyle(.secondary); Picker(name,selection:selection) { ForEach(0..<max(1,count),id:\.self) { Text("\($0+1)").tag($0) } }.labelsHidden().controlSize(.mini) }
    }
    private var audioSettings: some View {
        HStack(spacing:20) {
            Toggle(L("専用ドライバー"),isOn:Binding(
                get:{ driver.connectionIntent.wanted },
                set:{ $0 ? driver.startBridge():driver.stopBridge() }
            )).toggleStyle(.switch).disabled(!driver.connectionIntent.wanted && driver.bridgeBusy)
            Picker(L("サンプルレート"),selection:$model.settings.audio.sampleRate) {
                ForEach(AudioSettings.rateOptions,id:\.self) { Text("\($0) Hz").tag($0) }
            }.frame(width:235)
            Picker(L("サンプルバッファ長"),selection:$model.settings.audio.frames) {
                ForEach(AudioSettings.frameOptions,id:\.self) { Text("\($0)").tag($0) }
            }.frame(width:235)
            Stepper(L("追加バッファ: %d",model.settings.audio.delayBuffers),value:$model.settings.audio.delayBuffers,in:0...8)
            Text(L("約 %.2f ms",model.settings.audio.bufferDelayMilliseconds)).monospacedDigit().foregroundStyle(.secondary).help(L("内部バッファによる遅延の目安。機器の遅延は含みません"))
            Spacer(minLength:0)
        }.controlSize(.small)
            .onChange(of:model.settings.audio) { _ in model.changed(reconfigure:true) }
    }

}
struct Meter: View {
    let peak:Float
    var level:Double { min(1,max(0,(20*log10(Double(max(peak,0.000001)))+60)/60)) }
    var body:some View {
        VStack(spacing:5) {
            GeometryReader { geo in
                ZStack(alignment:.leading) {
                    RoundedRectangle(cornerRadius:3).fill(Color(nsColor:.quaternaryLabelColor))
                    RoundedRectangle(cornerRadius:3).fill(peak>=1 ? Color.red:Color.secondary).frame(width:max(0,geo.size.width*level))
                    HStack(spacing:0) { ForEach(0..<30) { _ in Spacer(minLength:0); Rectangle().fill(panel).frame(width:2) } }
                }
            }.frame(height:10)
            HStack { Text("−60"); Spacer(); Text("−24"); Spacer(); Text("−12"); Spacer(); Text(peak>=1 ? "CLIP":"0") }.font(.system(size:8,design:.monospaced)).foregroundStyle(peak>=1 ? Color.red:.secondary)
        }.accessibilityElement(children:.ignore).accessibilityLabel(L("レベルメーター")).accessibilityValue(String(format:"%.1f dB",20*log10(max(Double(peak),0.000001))))
    }
}
