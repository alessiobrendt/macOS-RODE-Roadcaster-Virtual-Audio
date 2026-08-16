// swift-tools-version:5.9
import PackageDescription

// The package/directory name (gui/RodeVADTester/Sources/RodeVADTester)
// stays as-is -- only the *product/executable* is renamed to "VAD" (the
// user-visible name; "RodeVADTester" started life as a per-channel test
// tone tool and has since grown into the full control-surface app, so
// the app itself is now branded "VAD -- Virtual Audio Driver"). Renaming
// the source directory too would be a large, purely-cosmetic ripple
// across every file's location for no user-visible benefit, so it was
// deliberately left alone; the executable TARGET name below is what
// actually determines the built binary's name (.build/release/VAD) and
// therefore the app bundle name (build/VAD.app) via the Makefile.
let package = Package(
    name: "VAD",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "VAD",
            path: "Sources/RodeVADTester"
        )
    ]
)
