// swift-tools-version: 6.3
import PackageDescription

// The opt-in timing lane is a package of its own: the development manifest's
// test build and the release manifest never carry it, and `pnpm
// benchmark:swift` runs it here.
let package = Package(
    name: "MarkdownCoreBenchmarks",
    platforms: [.iOS(.v26), .macOS(.v26)],
    dependencies: [.package(path: "../../..")],
    targets: [
        .executableTarget(
            name: "MarkdownCoreBenchmarks",
            dependencies: [.product(name: "MarkdownCore", package: "markdown-core")],
            path: "MarkdownCoreBenchmarks"
        )
    ]
)
