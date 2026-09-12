// swift-tools-version: 6.3
import PackageDescription

let package = Package(
    name: "swift-markdown-core",
    platforms: [
        .iOS(.v26),
        .macOS(.v26),
    ],
    products: [
        .library(name: "MarkdownCore", targets: ["MarkdownCore"])
    ],
    targets: [
        .target(
            name: "MarkdownCoreC",
            path: "packages/markdown-core",
            sources: [
                "core/markdown_core.c",
                "core/node.c",
                "core/iterator.c",
                "core/blocks.c",
                "core/inlines.c",
                "core/scanners.c",
                "core/utf8.c",
                "core/buffer.c",
                "core/references.c",
                "core/map.c",
                "core/houdini_html_u.c",
                "core/markdown_core_ctype.c",
                "core/linked_list.c",
                "extensions/ast.c",
                "extensions/attributes.c",
                "extensions/autolink.c",
                "extensions/block_identifier.c",
                "extensions/callout.c",
                "extensions/citation.c",
                "extensions/comment.c",
                "extensions/core-extensions.c",
                "extensions/cross_link.c",
                "extensions/definition_list.c",
                "extensions/directive.c",
                "extensions/ext_scanners.c",
                "extensions/footnote.c",
                "extensions/formula.c",
                "extensions/heading.c",
                "extensions/insertion.c",
                "extensions/link.c",
                "extensions/list.c",
                "extensions/mark.c",
                "extensions/media.c",
                "extensions/properties.c",
                "extensions/span.c",
                "extensions/specimen.c",
                "extensions/strikethrough.c",
                "extensions/subscript.c",
                "extensions/superscript.c",
                "extensions/table.c",
                "extensions/tasklist.c",
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
    ],
    cLanguageStandard: .c99
)
