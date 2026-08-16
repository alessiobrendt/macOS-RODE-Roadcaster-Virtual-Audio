import SwiftUI

/// The "Channel Test" tab (formerly the entire app, back when it was just
/// ContentView -- renamed as part of the multi-tab expansion). Body/logic
/// is otherwise unchanged from the original ContentView: still just
/// shells out to testtone per channel. The one wiring change is that
/// `store` is now an injected, shared DeviceStore (owned by AppShellView
/// and also used by DashboardView) rather than a private @StateObject
/// each tab created its own copy of.
///
/// Also has an "Auto Test" mode: plays every channel of the selected
/// device in sequence (channel 1, then 2, ...), reusing the exact same
/// per-channel play/status state (`playingChannels`/`channelStatus`) the
/// manual per-row Play buttons already use, so a row looks identical
/// ("Playing…") whether it was started manually or by Auto Test -- there
/// is deliberately no separate parallel UI state for the two modes.
struct ChannelTesterView: View {
    @EnvironmentObject var store: DeviceStore

    /// Per-channel UI state, keyed by 1-based channel number.
    @State private var playingChannels: Set<Int> = []
    @State private var channelStatus: [Int: String] = [:]
    @State private var testToneDuration: Double = 1.5

    /// Auto Test state. The running sequence is a single Task stored here
    /// so the Stop button can cancel it; cancellation is checked between
    /// channels (never mid-tone -- the in-flight `testtone` invocation for
    /// the current channel is always allowed to finish naturally).
    @State private var isAutoTesting = false
    @State private var autoTestTask: Task<Void, Never>?

    /// Pause between channels during Auto Test, long enough that
    /// consecutive channels are clearly distinguishable rather than
    /// blending together.
    private static let autoTestGapSeconds: Double = 0.4

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
        .onDisappear {
            // Don't leave a background test sequence running (and playing
            // real audio) after navigating away from this tab.
            stopAutoTest()
        }
        .onChange(of: store.selectedDeviceID) { _ in
            // Changing devices mid-sequence would run into a stale
            // channel count (or the wrong device entirely) -- stop
            // cleanly rather than continuing.
            stopAutoTest()
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Channel Test")
                .font(.title2)
                .bold()
            Text("Plays a short test tone to a single output channel at a time via the testtone CLI, so you can confirm each channel of the virtual driver (or any other CoreAudio output device) actually carries audio. Auto Test plays through every channel of the selected device in sequence.")
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
                .disabled(store.isLoading || isAutoTesting)

                Button {
                    store.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(store.isLoading || isAutoTesting)

                if store.isLoading {
                    ProgressView().controlSize(.small)
                }

                Button {
                    if isAutoTesting {
                        stopAutoTest()
                    } else {
                        startAutoTest()
                    }
                } label: {
                    if isAutoTesting {
                        Label("Stop", systemImage: "stop.fill")
                    } else {
                        Label("Auto Test", systemImage: "play.rectangle.fill")
                    }
                }
                .disabled(!isAutoTesting && store.selectedDevice == nil)

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
            // Disabled both while this specific channel is already
            // playing, and (to avoid overlapping/conflicting playback)
            // while Auto Test is running through the whole device.
            .disabled(playingChannels.contains(channel) || isAutoTesting)
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

    /// Manual single-channel Play button handler. Guards against
    /// overlapping with either another manual play of the same channel or
    /// an in-progress Auto Test sequence.
    private func play(channel: Int, on device: AudioDevice) {
        guard !playingChannels.contains(channel), !isAutoTesting else { return }
        let duration = testToneDuration
        Task.detached(priority: .userInitiated) {
            await playChannelAsync(channel: channel, on: device, duration: duration)
        }
    }

    /// The actual play-one-channel work, shared by both the manual Play
    /// button and Auto Test: updates playingChannels/channelStatus exactly
    /// the same way in both cases, so a row looks identical regardless of
    /// which mode started it.
    private func playChannelAsync(channel: Int, on device: AudioDevice, duration: Double) async {
        await MainActor.run {
            playingChannels.insert(channel)
            channelStatus[channel] = "Playing channel \(channel)…"
        }
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

    /// Plays every channel of the currently-selected device in sequence,
    /// pausing `autoTestGapSeconds` between channels. Cancellation (via
    /// stopAutoTest()) is only checked *between* channels -- an in-flight
    /// channel is always allowed to finish naturally rather than being
    /// killed mid-tone. Also bails out early if the selected device
    /// changes underneath it (double-checked here in addition to the
    /// .onChange(of: store.selectedDeviceID) guard on the view itself, in
    /// case both fire around the same time).
    private func startAutoTest() {
        guard !isAutoTesting, let device = store.selectedDevice else { return }
        isAutoTesting = true

        let duration = testToneDuration
        let channelCount = max(device.channelCount, 1)
        let deviceID = device.id

        autoTestTask = Task.detached(priority: .userInitiated) {
            for channel in 1...channelCount {
                if Task.isCancelled { break }

                let stillSelected = await MainActor.run { store.selectedDevice?.id == deviceID }
                guard stillSelected else { break }

                await playChannelAsync(channel: channel, on: device, duration: duration)

                if Task.isCancelled { break }
                if channel < channelCount {
                    try? await Task.sleep(nanoseconds: UInt64(Self.autoTestGapSeconds * 1_000_000_000))
                }
            }

            await MainActor.run {
                isAutoTesting = false
                autoTestTask = nil
            }
        }
    }

    /// Cancels the running Auto Test sequence, if any. Cancellation is
    /// cooperative (checked between channels in startAutoTest()'s loop),
    /// never an abrupt kill of an in-flight `testtone` process.
    private func stopAutoTest() {
        autoTestTask?.cancel()
    }
}
