import Foundation

enum GainSlider {
    static func decibels(_ proposed: Double) -> Double {
        abs(proposed) <= 0.5 ? 0 : proposed
    }
}
