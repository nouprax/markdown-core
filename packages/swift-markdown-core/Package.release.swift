// swift-tools-version: 6.3
import PackageDescription

// THE RELEASE MANIFEST: the same two targets the root `Package.swift`
// declares, and NOTHING ELSE. `scripts/check-swift-source-archive.sh` copies
// this file in as the archive's `Package.swift`, and
// `scripts/audit-ci-policy.sh` refuses it if it names a test target, the
// benchmarks, the conformance suite, a plugin or a tool -- so the product
// build cannot reach any of them even by accident.
//
// It is a SECOND FILE rather than a conditional in the root manifest because
// SwiftPM evaluates a manifest with no arguments of its own: there is nowhere
// to put "and not the tests" that a `swift build` of the archive would see.
//
// The paths are the archive's layout, which is this repository's layout with
// everything but `core`, `extensions`, `include` and `Sources/MarkdownCore`
// removed -- so they are the root manifest's paths unchanged, and the two
// files can be diffed.
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
                "extensions/code.c",
                "extensions/code_block.c",
                "extensions/document.c",
                "extensions/emphasis.c",
                "extensions/html.c",
                "extensions/html_block.c",
                "extensions/line_break.c",
                "extensions/paragraph.c",
                "extensions/text.c",
                "extensions/thematic_break.c",
                "extensions/autolink_scanners.c",
                "extensions/code_block_scanners.c",
                "extensions/comment_scanners.c",
                "extensions/footnote_scanners.c",
                "extensions/formula_scanners.c",
                "extensions/heading_scanners.c",
                "extensions/html_scanners.c",
                "extensions/link_scanners.c",
                "extensions/table_scanners.c",
                "extensions/text_scanners.c",
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
    ],
    cLanguageStandard: .c99
)
