// swift-tools-version:5.10
import Foundation
import PackageDescription

// The encrypted history store is the Rust crate in ../history-store (shared with the Qt app).
// Build it first: cargo build --release --locked --manifest-path ../history-store/Cargo.toml
// (scripts/build-app.sh does that).
let packageDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let rustLibDir = packageDir.appendingPathComponent("../history-store/target/release").standardized.path

let package = Package(
    name: "WrenMac",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "wren", targets: ["WrenMac"]),
    ],
    targets: [
        .systemLibrary(name: "CHistoryStore", path: "Sources/CHistoryStore"),
        .target(
            name: "WrenCore",
            dependencies: ["CHistoryStore"],
            linkerSettings: [.unsafeFlags(["-L", rustLibDir])]
        ),
        .executableTarget(
            name: "WrenMac",
            dependencies: ["WrenCore"],
            resources: [.process("Resources")]
        ),
        .testTarget(
            name: "WrenCoreTests",
            dependencies: ["WrenCore"]
        ),
    ]
)
