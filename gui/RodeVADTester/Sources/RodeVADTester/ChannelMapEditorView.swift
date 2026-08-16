import SwiftUI

/// The "Channel Mapping" tab: 5 Stepper rows (1-9 each) for which
/// Multitrack channel pair each virtual device is copied into, with
/// inline overlap highlighting and an explicit "Apply (restarts daemon)"
/// button -- no live-reload, by design (see README "Routing daemon").
struct ChannelMapEditorView: View {
    @EnvironmentObject var channelMapStore: ChannelMapStore
    @EnvironmentObject var daemonController: DaemonController

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            if let error = channelMapStore.validationError {
                banner(text: error, systemImage: "exclamationmark.triangle.fill", color: .red)
            }
            if let message = channelMapStore.lastActionMessage {
                banner(text: message, systemImage: "info.circle.fill", color: .blue)
            }
            if let error = channelMapStore.lastActionError {
                banner(text: error, systemImage: "exclamationmark.triangle.fill", color: .red)
            }

            VStack(spacing: 8) {
                ForEach(channelMapStore.entries) { entry in
                    row(for: entry)
                }
            }

            HStack(alignment: .top, spacing: 12) {
                Text("Applying restarts the router daemon -- routing is briefly interrupted while it does.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()

                Button {
                    channelMapStore.resetToDefaults()
                } label: {
                    Label("Reset to Defaults", systemImage: "arrow.counterclockwise")
                }
                .disabled(isBusyElsewhere)
                .help("Resets the steppers below to the compiled-in defaults (1/3/5/7/9). Local edit only -- press Apply afterward to actually save and restart with these values.")

                Button {
                    Task { await channelMapStore.applyAndRestart(daemonController: daemonController) }
                } label: {
                    if channelMapStore.isBusy {
                        ProgressView().controlSize(.small)
                    } else {
                        Text("Apply (restarts daemon)")
                    }
                }
                .disabled(isApplyDisabled)
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear { channelMapStore.load() }
    }

    private var isBusyElsewhere: Bool {
        daemonController.isBusy || channelMapStore.isBusy
    }

    private var isApplyDisabled: Bool {
        isBusyElsewhere || channelMapStore.validationError != nil || !channelMapStore.hasUnsavedChanges
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Channel Mapping")
                .font(.title2).bold()
            Text("Which 2-channel slice of the RodeCaster's 10-channel Main Multitrack device each virtual device is copied into. This is our own guess at RODE's original layout, not confirmed against real hardware feedback -- adjust here if audio comes out of unexpected channels once tested live.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func row(for entry: ChannelMapEntry) -> some View {
        let overlapping = isOverlapping(entry)
        return HStack {
            Text(entry.name)
                .frame(width: 140, alignment: .leading)

            Stepper(
                value: Binding(
                    get: { entry.startChannel },
                    set: { channelMapStore.updateStartChannel(forKey: entry.key, to: $0) }
                ),
                in: 1...9
            ) {
                Text("Channels \(entry.startChannel)-\(entry.startChannel + 1)")
                    .font(.body.monospacedDigit())
            }
        }
        .padding(8)
        .background(overlapping ? Color.red.opacity(0.12) : Color.gray.opacity(0.06))
        .cornerRadius(6)
    }

    private func isOverlapping(_ entry: ChannelMapEntry) -> Bool {
        guard let error = channelMapStore.validationError else { return false }
        return error.contains(entry.name)
    }

    private func banner(text: String, systemImage: String, color: Color) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: systemImage).foregroundStyle(color)
            Text(text).font(.callout).fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .background(color.opacity(0.12))
        .cornerRadius(8)
    }
}
