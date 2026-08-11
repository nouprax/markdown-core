// swift-tools-version: 6.0
import PackageDescription

// Product-only mirror of the repository root /Package.swift: the two target
// definitions below must stay byte-identical to their root counterparts —
// SwiftPM manifests cannot share target definitions, so edit both files
// together. scripts/check-swift-source-archive.sh ships this file as the
// release archive's Package.swift and builds it against an external
// consumer, which is where any drift surfaces.
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
                "core/markdown_core.c", "core/node.c", "core/concrete_records.c",
                "core/iterator.c", "core/blocks.c", "core/inlines.c",
                "core/delimiter.c", "core/scanners.c", "core/utf8.c",
                "core/buffer.c", "core/references.c", "core/map.c",
                "core/houdini_html_u.c", "core/markdown_core_ctype.c", "core/linked_list.c",
                "extensions/ast.c", "extensions/document.c", "extensions/arena.c",
                "extensions/source.c", "extensions/concrete.c", "extensions/diff.c",
                "extensions/delta.c",
                "extensions/core-extensions.c", "extensions/table.c", "extensions/strikethrough.c",
                "extensions/autolink.c", "extensions/formula.c", "extensions/directive.c",
                "extensions/cross_reference.c", "extensions/ext_scanners.c", "extensions/tasklist.c",
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
    ],
    cLanguageStandard: .c99
)
