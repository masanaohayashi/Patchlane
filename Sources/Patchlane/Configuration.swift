import Foundation

let busNames = ["Main", "Aux 1", "Aux 2", "Aux 3"]
struct InputSettings: Codable {
    var uid = ""
    var left = 0
    var right = 1
    var db: Double = 0
    var muted = false
    var routes = [true, false, false, false]
}
struct BusSettings: Codable {
    var uid = ""
    private var leftChannel:Int? = nil
    private var rightChannel:Int? = nil
    var left:Int { get { leftChannel ?? 0 } set { leftChannel=newValue } }
    var right:Int { get { rightChannel ?? 1 } set { rightChannel=newValue } }
    var db: Double = -6
    var muted = false
}
struct AudioSettings: Codable, Equatable {
    var frames = 32
    var sampleRate = 44100
    private var extraFrames:Int? = nil
    private var extraBuffers:Int? = nil
    var delayBuffers:Int {
        get { extraBuffers ?? ((extraFrames ?? 0)+frames-1)/max(1,frames) }
        set { extraBuffers=newValue;extraFrames=nil }
    }
    var delay:Int { get { delayBuffers*frames } set { delayBuffers=(newValue+frames-1)/max(1,frames) } }
    // Normal mixing already retains one callback block. Shared-ring output
    // supplies that base block explicitly; both expose only additional blocks.
    var bufferDelayFrames:Int { (delayBuffers+1)*frames }
    var bufferDelayMilliseconds:Double { Double(bufferDelayFrames)*1000/Double(sampleRate) }
    static func == (a:Self,b:Self)->Bool { a.frames==b.frames && a.sampleRate==b.sampleRate && a.delayBuffers==b.delayBuffers }
    static let frameOptions = [32,64,128,256,512,1024,2048]
    static let rateOptions = [44100,48000,88200,96000]
}
struct Settings: Codable {
    var version = 1
    private var audioSettings: AudioSettings? = nil
    var audio: AudioSettings { get { audioSettings ?? AudioSettings() } set { audioSettings=newValue } }
    var inputs = Array(repeating: InputSettings(), count: 4)
    var buses = Array(repeating: BusSettings(), count: 4)
    func validate() throws {
        guard AudioSettings.frameOptions.contains(audio.frames), AudioSettings.rateOptions.contains(audio.sampleRate),
              (0...8).contains(audio.delayBuffers), version == 1, inputs.count == 4, buses.count == 4 else { throw AppError(L("対応していない設定ファイルです。")) }
        for i in inputs { guard i.routes.count == 4, i.left >= 0, i.right >= 0, i.left < 256, i.right < 256, i.db.isFinite, (-60...36).contains(i.db) else { throw AppError(L("入力設定が無効です。")) } }
        for b in buses { guard (0..<256).contains(b.left),(0..<256).contains(b.right),b.db.isFinite, (-60...0).contains(b.db) else { throw AppError(L("出力設定が無効です。")) } }
    }
    static var file: URL { FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Patchlane/settings.json") }
    func save(to url: URL = Settings.file) throws {
        try validate()
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
    static func load(from url: URL = Settings.file) throws -> Settings {
        let s = try JSONDecoder().decode(Settings.self, from: Data(contentsOf: url))
        try s.validate(); return s
    }
}
struct AppError: LocalizedError { var message: String; init(_ message: String) { self.message = message }; var errorDescription: String? { message } }

// Only values that require rebuilding the device connection. Gains, routes and
// meters never participate in connection identity.
struct MixerConnectionSettings: Equatable {
    struct Input: Equatable { let uid:String;let left:Int;let right:Int }
    let inputs:[Input]
    let outputs:[Input]
    let audio:AudioSettings
    init(_ settings:Settings) {
        inputs=settings.inputs.map { Input(uid:$0.uid,left:$0.left,right:$0.right) }
        outputs=settings.buses.map { Input(uid:$0.uid,left:$0.left,right:$0.right) }
        audio=settings.audio
    }
}
