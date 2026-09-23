import XCTest
@testable import Patchlane

final class LocalizationTests:XCTestCase {
    func testBothLanguagesHaveMatchingKeysAndFormatArguments() throws {
        func strings(_ language:String) throws -> [String:String] {
            let directory=try XCTUnwrap(Localization.bundle.url(forResource:language,withExtension:"lproj"))
            let data=try Data(contentsOf:directory.appendingPathComponent("Localizable.strings"))
            return try XCTUnwrap(PropertyListSerialization.propertyList(from:data,format:nil) as? [String:String])
        }
        let en=try strings("en"),ja=try strings("ja")
        XCTAssertEqual(Set(en.keys),Set(ja.keys))
        let regex=try NSRegularExpression(pattern:"%(?:[0-9.]+)?(?:llu|d|@|f)")
        func arguments(_ text:String)->[String] {
            regex.matches(in:text,range:NSRange(text.startIndex...,in:text)).map { String(text[Range($0.range,in:text)!]) }
        }
        for key in ja.keys { XCTAssertEqual(arguments(en[key]!),arguments(ja[key]!),key) }
        XCTAssertEqual(en["ミキサー動作中"],"Mixer running")
        XCTAssertEqual(ja["ミキサー動作中"],"ミキサー動作中")
    }
    func testLanguageBundlesResolveDynamicLabelsAndPrivacyText() throws {
        for language in ["en","ja"] {
            let directory=try XCTUnwrap(Localization.bundle.url(forResource:language,withExtension:"lproj"))
            let bundle=try XCTUnwrap(Bundle(url:directory))
            let format=Localization.text("不足 %llu フレーム",bundle:bundle)
            XCTAssertEqual(String(format:format,UInt64(490804)),language=="en" ? "Underrun: 490804 frames":"不足 490804 フレーム")
            let privacy=bundle.localizedString(forKey:"NSMicrophoneUsageDescription",value:nil,table:"InfoPlist")
            XCTAssertNotEqual(privacy,"NSMicrophoneUsageDescription")
        }
    }
}
