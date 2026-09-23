import Foundation

struct DriverPreferences: Codable, Equatable {
    var enabled=false
    var sourceUID="audio.patchlane.virtual8"
    var outputUID=""
    var bus=0
    var left=0
    var right=1
    var frames=32
    var rate=44100
    var delay=80
    var configurations:[String:Data]=[:]
    static var file:URL { Settings.file.deletingLastPathComponent().appendingPathComponent("driver.json") }
    func validate() throws {
        guard ["audio.patchlane.virtual2","audio.patchlane.virtual8"].contains(sourceUID),
              (0..<4).contains(bus),(0..<256).contains(left),(0..<256).contains(right),
              AudioSettings.frameOptions.contains(frames),AudioSettings.rateOptions.contains(rate),
              (0...32768).contains(delay) else { throw AppError(L("専用ドライバーの保存設定が無効です。")) }
        for data in configurations.values { _ = try DriverConfiguration(data:data) }
    }
    func save(to url:URL=Self.file) throws {
        try validate()
        try FileManager.default.createDirectory(at:url.deletingLastPathComponent(),withIntermediateDirectories:true)
        try JSONEncoder().encode(self).write(to:url,options:.atomic)
    }
    static func load(from url:URL=Self.file) throws -> Self {
        let value=try JSONDecoder().decode(Self.self,from:Data(contentsOf:url))
        try value.validate();return value
    }
}
