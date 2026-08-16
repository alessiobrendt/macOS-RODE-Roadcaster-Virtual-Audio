import Foundation

/// One device's parsed level snapshot, straight from
/// state/rodevad-router.levels.
struct DeviceLevel: Identifiable {
    let key: String
    let name: String
    let peakL: Float
    let peakR: Float
    let rmsL: Float
    let rmsR: Float
    var id: String { key }
}

/// Polls state/rodevad-router.levels roughly every 120ms while a
/// levels-showing tab is visible (start()/stop() are called from that
/// view's onAppear/onDisappear -- this class never polls on its own
/// otherwise). This is for "is there signal, roughly how hot"
/// sanity-checking, not broadcast-grade metering -- see README "Routing
/// daemon".
@MainActor
final class LevelsPoller: ObservableObject {
    @Published var levels: [DeviceLevel] = []
    @Published var isStale: Bool = true

    private var timer: Timer?
    private static let pollInterval: TimeInterval = 0.12
    nonisolated private static let staleThresholdSeconds: TimeInterval = 1.0

    /// Fixed device order/names, matching kVirtualDeviceIdentities in
    /// daemon/rodevad-router.c exactly (key strings are the levels file's
    /// line-prefix format -- see README "Levels file format").
    nonisolated private static let deviceOrder: [(key: String, name: String)] = [
        ("system", "RVAD System"),
        ("game", "RVAD Game"),
        ("music", "RVAD Music"),
        ("virtuala", "RVAD Virtual A"),
        ("virtualb", "RVAD Virtual B")
    ]

    func start() {
        stop()
        poll()
        timer = Timer.scheduledTimer(withTimeInterval: Self.pollInterval, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.poll() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func poll() {
        Task.detached(priority: .utility) {
            let (levels, stale) = Self.readAndParse()
            await MainActor.run {
                self.levels = levels
                self.isStale = stale
            }
        }
    }

    nonisolated private static func readAndParse() -> ([DeviceLevel], Bool) {
        guard let url = try? ProjectLayout.levelsFile(),
              let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else {
            // Missing/unreadable file -- most commonly "the daemon isn't
            // running yet" -- is not an error state to surface loudly,
            // just "no data, stale".
            return (deviceOrder.map { DeviceLevel(key: $0.key, name: $0.name, peakL: 0, peakR: 0, rmsL: 0, rmsR: 0) }, true)
        }
        return parse(text)
    }

    /// Parses the daemon's levels file format:
    ///
    ///   version=1
    ///   timestamp=1755364821.123
    ///   system peakL=0.482 peakR=0.451 rmsL=0.201 rmsR=0.190
    ///   game peakL=0.000 peakR=0.000 rmsL=0.000 rmsR=0.000
    ///   ...
    ///
    /// Returns levels in deviceOrder (matching the daemon's fixed
    /// line order), and whether the snapshot should be treated as stale:
    /// timestamp older (or, degenerately, newer -- clock skew) than
    /// staleThresholdSeconds, or the file was empty/unparseable/missing
    /// any device lines at all.
    nonisolated static func parse(_ text: String) -> ([DeviceLevel], Bool) {
        var timestamp: Double?
        var rawFields: [String: [String: Float]] = [:]

        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("timestamp=") {
                timestamp = Double(line.dropFirst("timestamp=".count))
                continue
            }
            if line.hasPrefix("version=") { continue }

            let parts = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let keyPart = parts.first else { continue }
            var fields: [String: Float] = [:]
            for part in parts.dropFirst() {
                let kv = part.split(separator: "=", maxSplits: 1)
                guard kv.count == 2, let value = Float(kv[1]) else { continue }
                fields[String(kv[0])] = value
            }
            rawFields[String(keyPart)] = fields
        }

        let levels = deviceOrder.map { entry -> DeviceLevel in
            let fields = rawFields[entry.key] ?? [:]
            return DeviceLevel(
                key: entry.key,
                name: entry.name,
                peakL: fields["peakL"] ?? 0,
                peakR: fields["peakR"] ?? 0,
                rmsL: fields["rmsL"] ?? 0,
                rmsR: fields["rmsR"] ?? 0
            )
        }

        var stale = true
        if let timestamp {
            let age = Date().timeIntervalSince1970 - timestamp
            stale = abs(age) > staleThresholdSeconds
        }
        if rawFields.isEmpty { stale = true }

        return (levels, stale)
    }
}
