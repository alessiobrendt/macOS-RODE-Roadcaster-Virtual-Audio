import SwiftUI

struct ContentView: View {
    @StateObject private var store = DeviceStore()

    /// Per-channel UI state, keyed by 1-based channel number.
    @State private var playingChannels: Set<Int> = []
    @State private var channelStatus: [Int: String] = [:]
    @State private var testToneDuration: Double = 1.5

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            Divider()

            if let error = store.loadError {
                errorBanner(error)
            }

            if let device = store.selectedDevice {
                channelList(for: device)
            } else if !store.isLoading {
                Text("No output device selected.")
                    .foregroundStyle(.secondary)
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear {
            store.refresh()
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("RodeCaster Virtual Audio — Channel Tester")
                .font(.title2)
                .bold()
            Text("Plays a short test tone to a single output channel at a time via the testtone CLI, so you can confirm each channel of the virtual driver (or any other CoreAudio output device) actually carries audio.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 12) {
                Picker("Device:", selection: $store.selectedDeviceID) {
                    ForEach(store.devices) { device in
                        Text(device.displayName).tag(Optional(device.id))
                    }
                }
                .labelsHidden()
                .frame(minWidth: 260)

                Button {
                    store.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(store.isLoading)

                if store.isLoading {
                    ProgressView().controlSize(.small)
                }

                Spacer()

                HStack(spacing: 6) {
                    Text("Tone length:")
                        .foregroundStyle(.secondary)
                    Stepper(value: $testToneDuration, in: 0.5...5.0, step: 0.5) {
                        Text(String(format: "%.1fs", testToneDuration))
                    }
                }
            }
        }
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text(message)
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .background(Color.orange.opacity(0.12))
        .cornerRadius(8)
    }

    private func channelList(for device: AudioDevice) -> some View {
        ScrollView {
            VStack(spacing: 8) {
                ForEach(1...max(device.channelCount, 1), id: \.self) { channel in
                    channelRow(channel: channel, device: device)
                }
            }
        }
    }

    private func channelRow(channel: Int, device: AudioDevice) -> some View {
        HStack {
            Text("Channel \(channel)")
                .frame(width: 100, alignment: .leading)
                .font(.body.monospacedDigit())

            Button {
                play(channel: channel, on: device)
            } label: {
                if playingChannels.contains(channel) {
                    Label("Playing…", systemImage: "speaker.wave.2.fill")
                } else {
                    Label("Play", systemImage: "play.fill")
                }
            }
            .disabled(playingChannels.contains(channel))
            .frame(width: 120)

            Text(channelStatus[channel] ?? "")
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(Color.gray.opacity(0.06))
        .cornerRadius(6)
    }

    private func play(channel: Int, on device: AudioDevice) {
        guard !playingChannels.contains(channel) else { return }
        playingChannels.insert(channel)
        channelStatus[channel] = "Playing channel \(channel)…"

        let duration = testToneDuration
        Task.detached(priority: .userInitiated) {
            do {
                try await TestToneRunner.play(device: device, channel: channel, duration: duration)
                await MainActor.run {
                    playingChannels.remove(channel)
                    channelStatus[channel] = "Done"
                }
            } catch {
                await MainActor.run {
                    playingChannels.remove(channel)
                    channelStatus[channel] = "Error: \(error.localizedDescription)"
                }
            }
        }
    }
}
