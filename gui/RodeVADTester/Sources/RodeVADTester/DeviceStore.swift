import Foundation

@MainActor
final class DeviceStore: ObservableObject {
    @Published var devices: [AudioDevice] = []
    @Published var selectedDeviceID: AudioDevice.ID?
    @Published var loadError: String?
    @Published var isLoading = false

    var selectedDevice: AudioDevice? {
        devices.first { $0.id == selectedDeviceID }
    }

    /// The device name we prefer to auto-select the first time the list
    /// loads successfully -- our own virtual driver, if it's installed
    /// and running.
    private static let preferredDeviceName = "RodeCaster Virtual Audio"

    func refresh() {
        isLoading = true
        loadError = nil
        let previousSelection = selectedDeviceID

        Task.detached(priority: .userInitiated) {
            do {
                let devices = try AudioDeviceLister.listDevices()
                await MainActor.run {
                    self.devices = devices
                    self.isLoading = false

                    // Keep the existing selection if it's still present;
                    // otherwise prefer our own virtual driver; otherwise
                    // fall back to the first device in the list.
                    if let previousSelection, devices.contains(where: { $0.id == previousSelection }) {
                        self.selectedDeviceID = previousSelection
                    } else if let preferred = devices.first(where: { $0.name == Self.preferredDeviceName }) {
                        self.selectedDeviceID = preferred.id
                    } else {
                        self.selectedDeviceID = devices.first?.id
                    }
                }
            } catch {
                await MainActor.run {
                    self.isLoading = false
                    self.loadError = error.localizedDescription
                }
            }
        }
    }
}
