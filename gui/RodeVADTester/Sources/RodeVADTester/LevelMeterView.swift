import SwiftUI

/// Dependency-free horizontal level meter: the filled bar tracks RMS, a
/// thin marker line tracks peak, and fill color ramps green -> yellow ->
/// red as the signal approaches full scale. Built entirely from
/// GeometryReader + shapes -- no external charting/meter libraries.
struct LevelMeterView: View {
    let rms: Float   // expected 0...1 (values are already clamped by the daemon, but this view clamps again defensively)
    let peak: Float  // expected 0...1

    var body: some View {
        let clampedRMS = CGFloat(min(max(rms, 0), 1))
        let clampedPeak = CGFloat(min(max(peak, 0), 1))

        GeometryReader { geo in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color.gray.opacity(0.15))

                RoundedRectangle(cornerRadius: 3)
                    .fill(colorForLevel(clampedRMS))
                    .frame(width: geo.size.width * clampedRMS)
                    .animation(.linear(duration: 0.1), value: rms)

                Rectangle()
                    .fill(Color.primary.opacity(0.85))
                    .frame(width: 2)
                    .offset(x: max(0, geo.size.width * clampedPeak - 1))
                    .animation(.linear(duration: 0.1), value: peak)
            }
        }
        .frame(height: 14)
        .clipShape(RoundedRectangle(cornerRadius: 3))
    }

    private func colorForLevel(_ level: CGFloat) -> Color {
        switch level {
        case ..<0.6: return .green
        case 0.6..<0.85: return .yellow
        default: return .red
        }
    }
}
