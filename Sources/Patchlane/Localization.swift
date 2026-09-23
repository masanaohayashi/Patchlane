import Foundation

// App bundles carry .lproj resources; command-line builds and tests use SwiftPM.
enum Localization {
    static let bundle:Bundle = Bundle.main.url(forResource:"Localizable",withExtension:"strings") != nil ? .main:.module
    static func text(_ key:String,bundle:Bundle=bundle)->String {
        bundle.localizedString(forKey:key,value:key,table:nil)
    }
}
func L(_ key:String,_ arguments:CVarArg...)->String {
    let format=Localization.text(key)
    return arguments.isEmpty ? format:String(format:format,locale:Locale.current,arguments:arguments)
}
