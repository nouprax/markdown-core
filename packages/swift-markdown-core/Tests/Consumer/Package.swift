// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MarkdownCoreConsumer",
    platforms: [.iOS(.v18), .macOS(.v15)],
    dependencies: [.package(path: "../../../..")],
    targets: [
        .testTarget(
            name: "ConsumerTests",
            dependencies: [.product(name: "MarkdownCore", package: "markdown-core")]
        )
    ]
)
