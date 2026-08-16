import SwiftUI

/// The "Levels" tab: a grid of 5 devices x L/R level meters, subscribed to
/// LevelsPoller. LevelsPoller is scoped to this tab -- it only polls
/// while this view is on screen (start() in onAppear, stop() in
/// onDisappear), not for the app's entire lifetime.
struct MetersView: View {
    @EnvironmentObject var levelsPoller: LevelsPoller

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            if levelsPoller.isStale {
                staleBanner
            }

            ScrollView {
                VStack(spacing: 12) {
                    ForEach(levelsPoller.levels) { level in
                        deviceRow(level)
                    }
                }
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear { levelsPoller.start() }
        .onDisappear { levelsPoller.stop() }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Live Levels")
                .font(.title2).bold()
            Text("The actual post-mix signal being written to the RodeCaster's Main Multitrack device, updated ~8x/second. This is for \"is there signal, roughly how hot\" sanity-checking -- not broadcast-grade metering.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var staleBanner: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text("No live data. Either the router daemon isn't running, or it hasn't written a levels update recently. Check the Daemon tab.")
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .background(Color.orange.opacity(0.12))
        .cornerRadius(8)
    }

    private func deviceRow(_ level: DeviceLevel) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(level.name)
                .font(.headline)
            HStack(spacing: 8) {
                Text("L").font(.caption.monospaced()).frame(width: 14, alignment: .center)
                LevelMeterView(rms: level.rmsL, peak: level.peakL)
            }
            HStack(spacing: 8) {
                Text("R").font(.caption.monospaced()).frame(width: 14, alignment: .center)
                LevelMeterView(rms: level.rmsR, peak: level.peakR)
            }
        }
        .padding(10)
        .background(Color.gray.opacity(0.06))
        .cornerRadius(8)
        .opacity(levelsPoller.isStale ? 0.4 : 1.0)
    }
}
