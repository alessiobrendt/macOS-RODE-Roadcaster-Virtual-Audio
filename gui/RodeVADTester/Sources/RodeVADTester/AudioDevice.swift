import Foundation

/// One CoreAudio output device, as reported by `testtone --list-machine`.
///
/// `testtone` is the single source of truth for device enumeration here
/// (see `tools/testtone.c: ListDevicesMachine`) -- this GUI intentionally
/// does not reimplement CoreAudio device enumeration in Swift, to avoid
/// having two independently-maintained copies of that logic that could
/// drift apart.
struct AudioDevice: Identifiable, Hashable {
    /// Index within the filtered (output-capable) device list, as
    /// reported by testtone. Not used for selection here (we select by
    /// UID, which is unambiguous even if the device list changes between
    /// calls) but kept around for display/debugging.
    let index: Int
    let channelCount: Int
    let name: String
    let uid: String

    var id: String { uid }

    var displayName: String {
        "\(name) (\(channelCount) ch)"
    }
}

enum AudioDeviceListError: LocalizedError {
    case testtoneNotFound
    case processFailed(String)
    case malformedOutput(String)

    var errorDescription: String? {
        switch self {
        case .testtoneNotFound:
            return "Could not find the testtone binary next to this app (expected build/testtone)."
        case .processFailed(let detail):
            return "testtone --list-machine failed: \(detail)"
        case .malformedOutput(let line):
            return "testtone --list-machine produced an unexpected line: \(line)"
        }
    }
}

enum AudioDeviceLister {
    /// Runs `testtone --list-machine` and parses its tab-separated
    /// output (index\tchannels\tname\tuid, one device per line, no
    /// header) into `AudioDevice` values.
    static func listDevices() throws -> [AudioDevice] {
        let testtoneURL = try TestToneLocator.locate()

        let process = Process()
        process.executableURL = testtoneURL
        process.arguments = ["--list-machine"]

        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        do {
            try process.run()
        } catch {
            throw AudioDeviceListError.processFailed(error.localizedDescription)
        }
        process.waitUntilExit()

        let outData = stdoutPipe.fileHandleForReading.readDataToEndOfFile()
        let errData = stderrPipe.fileHandleForReading.readDataToEndOfFile()

        guard process.terminationStatus == 0 else {
            let errText = String(data: errData, encoding: .utf8) ?? "unknown error"
            throw AudioDeviceListError.processFailed("exit code \(process.terminationStatus): \(errText)")
        }

        let output = String(data: outData, encoding: .utf8) ?? ""
        var devices: [AudioDevice] = []
        for line in output.split(separator: "\n", omittingEmptySubsequences: true) {
            let fields = line.components(separatedBy: "\t")
            guard fields.count == 4,
                  let idx = Int(fields[0]),
                  let channels = Int(fields[1]) else {
                throw AudioDeviceListError.malformedOutput(String(line))
            }
            devices.append(AudioDevice(index: idx, channelCount: channels, name: fields[2], uid: fields[3]))
        }
        return devices
    }
}
