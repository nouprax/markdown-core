// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "swift-markdown-core",
    platforms: [
        .iOS(.v18),
        .macOS(.v15),
    ],
    products: [
        .library(name: "MarkdownCore", targets: ["MarkdownCore"])
    ],
    targets: [
        .target(
            name: "MarkdownCoreC",
            path: "packages/markdown-core",
            sources: [
                "core/markdown_core.c", "core/node.c", "core/iterator.c", "core/blocks.c",
                "core/inlines.c", "core/scanners.c", "core/utf8.c", "core/buffer.c",
                "core/references.c", "core/map.c",
                "core/houdini_html_u.c", "core/markdown_core_ctype.c",
                "core/linked_list.c", "core/text.c", "extensions/core-extensions.c",
                "extensions/ast.c", "extensions/session.c", "extensions/adopt.c",
                "extensions/footnote.c",
                "extensions/changeset.c", "extensions/table.c", "extensions/strikethrough.c",
                "extensions/autolink.c", "extensions/formula.c", "extensions/directive.c",
                "extensions/ext_scanners.c", "extensions/tasklist.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("core"),
                .headerSearchPath("extensions"),
                .headerSearchPath("include"),
                .headerSearchPath("core/include"),
                .define("MARKDOWN_CORE_STATIC_DEFINE"),
                .define("MARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE"),
            ]
        ),
        .target(
            name: "MarkdownCore",
            dependencies: ["MarkdownCoreC"],
            path: "packages/swift-markdown-core/Sources/MarkdownCore"
        ),
        .testTarget(
            name: "MarkdownCoreTests",
            dependencies: ["MarkdownCore"],
            path: "packages/swift-markdown-core/Tests/MarkdownCoreTests"
        ),
        .testTarget(
            name: "MarkdownCoreConformanceTests",
            dependencies: ["MarkdownCore"],
            path: "packages/swift-markdown-core/Tests/MarkdownCoreConformanceTests",
            plugins: [.plugin(name: "GenerateCanonicalASTResources")]
        ),
        .executableTarget(
            name: "CanonicalASTResourceGenerator",
            path: "packages/swift-markdown-core/Tools/CanonicalASTResourceGenerator"
        ),
        .plugin(
            name: "GenerateCanonicalASTResources",
            capability: .buildTool(),
            dependencies: ["CanonicalASTResourceGenerator"],
            path: "packages/swift-markdown-core/Plugins/GenerateCanonicalASTResources"
        ),
        .executableTarget(
            name: "MarkdownCoreBenchmarks",
            dependencies: ["MarkdownCore"],
            path: "packages/swift-markdown-core/Benchmarks/MarkdownCoreBenchmarks"
        ),
    ],
    cLanguageStandard: .c99
)
