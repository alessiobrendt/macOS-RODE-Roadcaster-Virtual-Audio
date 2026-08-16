import Foundation

/// One device's editable channel-mapping row.
struct ChannelMapEntry: Identifiable {
    let key: String
    let name: String
    var startChannel: Int // 1-based; occupies startChannel and startChannel+1
    var id: String { key }
}

/// Owns config/channel-map.conf: loading it (falling back to
/// daemon/channel-map.example.conf as a starting template if it doesn't
/// exist yet), client-side validation for immediate UX feedback,
/// atomic-write saving, and the save -> stop daemon -> start daemon
/// "Apply" flow.
///
/// IMPORTANT: this class's validate() mirrors daemon/rodevad-router.c's
/// LoadChannelMapFromConfig rules (range 1-9, no overlapping pairs, all 5
/// keys present) purely so the UI can give instant feedback while
/// editing. It is NOT the safety authority -- the daemon re-validates the
/// saved file itself from scratch at startup and refuses to run with a
/// bad mapping regardless of what this client-side check concluded.
@MainActor
final class ChannelMapStore: ObservableObject {
    @Published var entries: [ChannelMapEntry] = []
    @Published var hasUnsavedChanges = false
    @Published var validationError: String?
    @Published var isBusy = false
    @Published var lastActionMessage: String?
    @Published var lastActionError: String?

    private var savedSnapshot: [String: Int] = [:]

    /// Fixed order/names, matching daemon/rodevad-router.c's
    /// kVirtualDeviceIdentities and its .key field exactly.
    private static let deviceOrder: [(key: String, name: String)] = [
        ("system", "RVAD System"),
        ("game", "RVAD Game"),
        ("music", "RVAD Music"),
        ("virtuala", "RVAD Virtual A"),
        ("virtualb", "RVAD Virtual B")
    ]

    /// Matches daemon/rodevad-router.c's kDefaultChannelMap (1-based here;
    /// the daemon's own table is 0-based internally).
    private static let defaultStartChannels: [String: Int] = [
        "system": 1, "game": 3, "music": 5, "virtuala": 7, "virtualb": 9
    ]

    func load() {
        let text: String?
        if let configURL = try? ProjectLayout.channelMapConfig(),
           let data = try? Data(contentsOf: configURL) {
            text = String(data: data, encoding: .utf8)
        } else if let exampleURL = try? ProjectLayout.channelMapExampleConfig(),
                  let data = try? Data(contentsOf: exampleURL) {
            text = String(data: data, encoding: .utf8)
        } else {
            text = nil
        }

        let parsed = text.map { Self.parse($0) } ?? [:]
        entries = Self.deviceOrder.map { entry in
            let value = parsed[entry.key] ?? Self.defaultStartChannels[entry.key] ?? 1
            return ChannelMapEntry(key: entry.key, name: entry.name, startChannel: value)
        }
        savedSnapshot = Dictionary(uniqueKeysWithValues: entries.map { ($0.key, $0.startChannel) })
        hasUnsavedChanges = false
        lastActionMessage = nil
        lastActionError = nil
        validate()
    }

    func updateStartChannel(forKey key: String, to value: Int) {
        guard let idx = entries.firstIndex(where: { $0.key == key }) else { return }
        entries[idx].startChannel = value
        recomputeUnsavedChanges()
        validate()
    }

    /// Resets every entry's startChannel back to the compiled-in defaults
    /// (system=1, game=3, music=5, virtuala=7, virtualb=9 -- matching
    /// daemon/rodevad-router.c's sChannelMap initializer and
    /// daemon/channel-map.example.conf). This is a LOCAL EDIT ONLY, exactly
    /// like changing a single Stepper by hand: it does not touch
    /// config/channel-map.conf on disk and does not restart the daemon by
    /// itself. The existing hasUnsavedChanges/validationError flow is
    /// reused (via recomputeUnsavedChanges()/validate()) so "Apply
    /// (restarts daemon)" is still required afterward to actually save and
    /// restart with the reset values.
    func resetToDefaults() {
        for idx in entries.indices {
            entries[idx].startChannel = Self.defaultStartChannels[entries[idx].key] ?? entries[idx].startChannel
        }
        recomputeUnsavedChanges()
        validate()
    }

    private func recomputeUnsavedChanges() {
        let current = Dictionary(uniqueKeysWithValues: entries.map { ($0.key, $0.startChannel) })
        hasUnsavedChanges = current != savedSnapshot
    }

    func validate() {
        for entry in entries {
            if entry.startChannel < 1 || entry.startChannel > 9 {
                validationError = "\(entry.name): channel \(entry.startChannel) is out of range (must be 1-9)."
                return
            }
        }
        for a in 0..<entries.count {
            for b in (a + 1)..<entries.count {
                let aStart = entries[a].startChannel, aEnd = aStart + 1
                let bStart = entries[b].startChannel, bEnd = bStart + 1
                if aStart <= bEnd && bStart <= aEnd {
                    validationError = "\(entries[a].name) (channels \(aStart)-\(aEnd)) overlaps \(entries[b].name) (channels \(bStart)-\(bEnd))."
                    return
                }
            }
        }
        validationError = nil
    }

    /// Parses the same `key=value` format the daemon reads (blank lines
    /// and `#` comments ignored).
    static func parse(_ text: String) -> [String: Int] {
        var result: [String: Int] = [:]
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let kv = line.split(separator: "=", maxSplits: 1)
            guard kv.count == 2, let value = Int(kv[1].trimmingCharacters(in: .whitespaces)) else { continue }
            result[String(kv[0]).trimmingCharacters(in: .whitespaces)] = value
        }
        return result
    }

    /// Writes config/channel-map.conf atomically (temp file + replace).
    /// Throws (and does not write anything) if the current entries fail
    /// client-side validation.
    func save() throws {
        validate()
        guard validationError == nil else {
            throw NSError(domain: "ChannelMapStore", code: 1, userInfo: [NSLocalizedDescriptionKey: validationError ?? "Invalid configuration."])
        }

        let configURL = try ProjectLayout.channelMapConfig()
        try FileManager.default.createDirectory(at: configURL.deletingLastPathComponent(), withIntermediateDirectories: true)

        var text = "# Written by RodeVADTester -- see daemon/channel-map.example.conf for the documented format.\n"
        for entry in entries {
            text += "\(entry.key)=\(entry.startChannel)\n"
        }

        let tmpURL = configURL.appendingPathExtension("tmp")
        try text.write(to: tmpURL, atomically: true, encoding: .utf8)
        _ = try FileManager.default.replaceItemAt(configURL, withItemAt: tmpURL)

        savedSnapshot = Dictionary(uniqueKeysWithValues: entries.map { ($0.key, $0.startChannel) })
        hasUnsavedChanges = false
    }

    /// save() -> stop the daemon -> start it again, surfacing progress
    /// and any error from each step. There is no live-reload in this
    /// round -- an explicit Apply (which this method implements) is the
    /// only way a changed mapping takes effect, by design (see README
    /// "Routing daemon").
    func applyAndRestart(daemonController: DaemonController) async {
        guard !isBusy else { return }
        isBusy = true
        lastActionError = nil
        defer { isBusy = false }

        lastActionMessage = "Saving config/channel-map.conf..."
        do {
            try save()
        } catch {
            lastActionError = "Could not save config/channel-map.conf: \(error.localizedDescription)"
            lastActionMessage = nil
            return
        }

        lastActionMessage = "Stopping the router daemon..."
        await daemonController.stop()
        if let stopError = daemonController.lastActionError {
            lastActionError = "Applied the new mapping, but stopping the daemon reported: \(stopError)"
        }

        lastActionMessage = "Starting the router daemon with the new mapping..."
        await daemonController.start()
        if let startError = daemonController.lastActionError {
            lastActionError = "Saved and stopped, but starting the daemon reported: \(startError)"
            lastActionMessage = nil
            return
        }

        lastActionMessage = "Applied. Check the Daemon tab / logs to confirm it started cleanly with the new mapping."
    }
}
