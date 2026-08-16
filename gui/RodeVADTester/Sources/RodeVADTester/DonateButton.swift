import SwiftUI
import AppKit

/// A small, unobtrusive pink-heart donate button, opening
/// https://www.paypal.com/paypalme/alessiobrendt in the user's default
/// browser via NSWorkspace. Placed in AppShellView's toolbar/footer --
/// never blocks or gates any core functionality.
struct DonateButton: View {
    private static let donateURL = URL(string: "https://www.paypal.com/paypalme/alessiobrendt")!

    var body: some View {
        Button {
            NSWorkspace.shared.open(Self.donateURL)
        } label: {
            Label("Donate", systemImage: "heart.fill")
                .foregroundStyle(.pink)
        }
        .help("Support this project -- opens PayPal in your browser")
    }
}
