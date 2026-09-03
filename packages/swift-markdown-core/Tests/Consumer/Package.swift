// swift-tools-version: 6.3
import PackageDescription

let package = Package(
    name: "MarkdownCoreConsumer",
    platforms: [.iOS(.v26), .macOS(.v26)],
    dependencies: [.package(path: "../../../..")],
    targets: [
        .testTarget(
            name: "ConsumerTests",
            dependencies: [.product(name: "MarkdownCore", package: "markdown-core")]
        )
    ]
)
