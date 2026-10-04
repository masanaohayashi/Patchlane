import SwiftUI
import AppKit
import AudioCore

// Bipolar fill uses zero as its origin; native knob/tracking/accessibility remain.
private final class DipoleSliderCell:NSSliderCell {
    override func drawBar(inside rect:NSRect,flipped:Bool) {
        let vertical=isVertical
        let track=vertical ? NSRect(x:rect.midX-2,y:rect.minY,width:4,height:rect.height):NSRect(x:rect.minX,y:rect.midY-2,width:rect.width,height:4)
        NSColor.separatorColor.setFill();NSBezierPath(roundedRect:track,xRadius:2,yRadius:2).fill()
        guard doubleValue != 0,isEnabled else { return }
        let knob=knobRect(flipped:flipped),zero=vertical ? track.midY:track.midX
        let value=vertical ? min(track.maxY,max(track.minY,knob.midY)):min(track.maxX,max(track.minX,knob.midX))
        let fill=vertical ? NSRect(x:track.minX,y:min(zero,value),width:track.width,height:abs(value-zero)):NSRect(x:min(zero,value),y:track.minY,width:abs(value-zero),height:track.height)
        NSColor.systemBlue.setFill();NSBezierPath(roundedRect:fill,xRadius:2,yRadius:2).fill()
    }
}
private struct DipoleBandSlider:NSViewRepresentable {
    @Binding var value:Double
    var label:String
    var vertical=true
    final class Coordinator:NSObject {
        var parent:DipoleBandSlider
        init(_ parent:DipoleBandSlider){self.parent=parent}
        @objc func changed(_ sender:NSSlider){parent.value=(sender.doubleValue*10).rounded()/10}
    }
    func makeCoordinator()->Coordinator{Coordinator(self)}
    func makeNSView(context:Context)->NSSlider {
        let slider=NSSlider(frame:vertical ? NSRect(x:0,y:0,width:24,height:150):NSRect(x:0,y:0,width:190,height:22));slider.cell=DipoleSliderCell()
        slider.minValue = -12;slider.maxValue=12;slider.isVertical=vertical;slider.isContinuous=true
        slider.target=context.coordinator;slider.action=#selector(Coordinator.changed(_:))
        slider.setAccessibilityLabel(label);return slider
    }
    func updateNSView(_ slider:NSSlider,context:Context){context.coordinator.parent=self;slider.doubleValue=value;slider.needsDisplay=true}
}

struct DipoleEditor:View {
    @ObservedObject var model:MixerModel
    let bus:Int
    @Environment(\.dismiss) private var dismiss
    @State private var name=""
    @State private var message:String?
    private struct SaveRequest {
        let name:String
        let parameters:DipoleParameters
        let existingName:String?
    }
    @State private var pendingSave:SaveRequest?
    @State private var confirmOverwrite=false
    private var selectedPreset:DipolePreset? { model.settings.dipolePresets.first{$0.id==model.settings.buses[bus].dipole.presetID} }
    private var parameters:Binding<DipoleParameters> { Binding(get:{model.settings.buses[bus].dipole.parameters},set:{ value in
        model.settings.buses[bus].dipole.parameters=value;model.settings.buses[bus].dipole.presetID=nil;model.changed()
    }) }
    var body:some View {
        VStack(alignment:.leading,spacing:18) {
            HStack {
                Text(L("%@ のステレオダイポール",busNames[bus])).font(.title2)
                Spacer()
                Toggle(L("有効"),isOn:Binding(get:{model.settings.buses[bus].dipole.enabled},set:{model.settings.buses[bus].dipole.enabled=$0;model.changed()})).toggleStyle(.switch)
                Button(L("閉じる")){dismiss()}.keyboardShortcut(.cancelAction)
            }
            HStack {
                Picker(L("プリセット"),selection:Binding<UUID?>(get:{model.settings.buses[bus].dipole.presetID},set:{model.settings.selectDipolePreset($0,bus:bus);model.changed()})) {
                    Text(L("カスタム")).tag(UUID?.none)
                    ForEach(model.settings.dipolePresets){preset in Text(preset.name).tag(Optional(preset.id))}
                }.frame(width:310)
                TextField(L("保存する名前"),text:$name).frame(width:230)
                Button(L("保存")) { requestSave() }.disabled(name.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty)
                Button(L("選択プリセットを削除")) {
                    if let id=model.settings.buses[bus].dipole.presetID { model.settings.deleteDipolePreset(id);model.changed();model.save() }
                }.disabled(model.settings.buses[bus].dipole.presetID==nil)
            }
            HStack(spacing:18) {
                dimension(L("中心間隔 cm"),value:parameters.spacingCM,range:2...200)
                dimension(L("聴取距離 cm"),value:parameters.distanceCM,range:20...500)
                dimension(L("頭幅 cm"),value:parameters.headCM,range:10...25)
                dimension(L("最大増幅 dB"),value:parameters.maxBoostDB,range:0...24)
                Picker(L("補正方式"),selection:parameters.tonalReference) {
                    Text(L("耳元のモデル")).tag(0)
                    Text(L("中央音フラット")).tag(1)
                    Text(L("片側入力フラット")).tag(2)
                }.frame(width:210)
            }
            Text(L("既存補正 → 31バンドGEQ → ダイポール · 左右共通 · 20Hz〜20kHz · 各±12dB")).foregroundStyle(.secondary)
            HStack(alignment:.top,spacing:7) {
                ForEach(0..<31,id:\.self){i in
                    VStack(spacing:6){
                        Text(String(format:"%+.1f",parameters.wrappedValue.geqDB[i])).font(.system(size:10,design:.monospaced))
                        DipoleBandSlider(value:Binding(get:{parameters.wrappedValue.geqDB[i]},set:{var p=parameters.wrappedValue;p.geqDB[i]=$0;parameters.wrappedValue=p}),label:frequency(i)).frame(width:24,height:150)
                        Text(frequency(i)).font(.system(size:9))
                    }.frame(width:28)
                }
            }
            HStack {
                Button(L("GEQを0dBに戻す")){var p=parameters.wrappedValue;p.geqDB=Array(repeating:0,count:31);p.trimDB=0;parameters.wrappedValue=p}
                Text(L("入力ゲイン"))
                DipoleBandSlider(value:parameters.trimDB,label:L("入力ゲイン"),vertical:false).frame(width:190,height:22)
                Text(String(format:"%+.1f dB",parameters.wrappedValue.trimDB)).monospacedDigit()
                Spacer()
                Text(L("最小位相 · FFTバッファ %.1f ms",Double(lc_dipole_latency())*1000/Double(model.settings.audio.sampleRate))).foregroundStyle(.secondary)
            }
            Text(L("低域の左右差を保持。配置に合わせて入力してください。設定変更は再生中に適用されます。")).font(.caption).foregroundStyle(.secondary)
            if let message { Text(message).foregroundStyle(.red) }
        }.padding(24).frame(width:1140).controlSize(.small)
        .onAppear { name=selectedPreset?.name ?? "" }
        .onChange(of:model.settings.buses[bus].dipole.presetID) { _ in
            // Parameter edits make the preset Custom; retain its name so Save
            // can update the original after confirmation.
            if let preset=selectedPreset { name=preset.name }
        }
        .onChange(of:model.settings.buses[bus].uid) { _ in name=selectedPreset?.name ?? "" }
        .alert(L("プリセットを上書きしますか？"),isPresented:$confirmOverwrite,presenting:pendingSave) { request in
            Button(L("上書き")) { savePreset(request) }
            Button(L("キャンセル"),role:.cancel) { pendingSave=nil }
        } message: { request in
            Text(L("「%@」の設定を上書きします。このプリセットを使う出力デバイスにも反映されます。",request.existingName ?? request.name))
        }
    }
    private func requestSave() {
        let trimmed=name.trimmingCharacters(in:.whitespacesAndNewlines)
        let existing=model.settings.dipolePresets.first{$0.name.caseInsensitiveCompare(trimmed) == .orderedSame}
        let request=SaveRequest(name:trimmed,parameters:parameters.wrappedValue,existingName:existing?.name)
        if existing != nil { pendingSave=request;confirmOverwrite=true }
        else { savePreset(request) }
    }
    private func savePreset(_ request:SaveRequest) {
        do {
            try model.settings.saveDipolePreset(name:request.name,parameters:request.parameters,bus:bus)
            name=selectedPreset?.name ?? request.name
            model.changed();model.save();message=nil;pendingSave=nil
        } catch { message=error.localizedDescription }
    }
    private func frequency(_ i:Int)->String{let f=DipoleParameters.frequencies[i];return f>=1000 ? String(format:"%gk",f/1000):String(format:"%g",f)}
    private func dimension(_ title:String,value:Binding<Double>,range:ClosedRange<Double>)->some View {
        HStack(spacing:5){Text(title);TextField(title,value:Binding(get:{value.wrappedValue},set:{value.wrappedValue=min(range.upperBound,max(range.lowerBound,$0))}),format:.number).frame(width:55)}
    }
}
