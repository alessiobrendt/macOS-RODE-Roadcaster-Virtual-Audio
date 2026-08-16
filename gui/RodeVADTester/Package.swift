// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "RodeVADTester",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "RodeVADTester",
            path: "Sources/RodeVADTester"
        )
    ]
)
