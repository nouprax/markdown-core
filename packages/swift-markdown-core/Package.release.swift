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
// everything but `core`, `elements`, `include` and `Sources/MarkdownCore`
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
                "core/utf8.c",
                "core/buffer.c",
                "core/references.c",
                "core/map.c",
                "core/houdini_html_u.c",
                "core/markdown_core_ctype.c",
                "core/linked_list.c",
                "elements/code.c",
                "elements/code_block.c",
                "elements/document.c",
                "elements/emphasis.c",
                "elements/html.c",
                "elements/html_block.c",
                "elements/line_break.c",
                "elements/paragraph.c",
                "elements/text.c",
                "elements/thematic_break.c",
                "elements/autolink_scanners.c",
                "elements/code_block_scanners.c",
                "elements/comment_scanners.c",
                "elements/footnote_scanners.c",
                "elements/formula_scanners.c",
                "elements/heading_scanners.c",
                "elements/html_scanners.c",
                "elements/link_scanners.c",
                "elements/table_scanners.c",
                "elements/text_scanners.c",
                "elements/ast.c",
                "elements/attributes.c",
                "elements/autolink.c",
                "elements/block_identifier.c",
                "elements/callout.c",
                "elements/citation.c",
                "elements/comment.c",
                "elements/core-elements.c",
                "elements/cross_link.c",
                "elements/definition_list.c",
                "elements/directive.c",
                "elements/footnote.c",
                "elements/formula.c",
                "elements/heading.c",
                "elements/insertion.c",
                "elements/link.c",
                "elements/list.c",
                "elements/mark.c",
                "elements/media.c",
                "elements/properties.c",
                "elements/span.c",
                "elements/specimen.c",
                "elements/strikethrough.c",
                "elements/subscript.c",
                "elements/superscript.c",
                "elements/table.c",
                "elements/tasklist.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("core"),
                .headerSearchPath("elements"),
                .headerSearchPath("include"),
                .headerSearchPath("core/include"),
                .define("MARKDOWN_CORE_STATIC_DEFINE"),
                .define("MARKDOWN_CORE_ELEMENTS_STATIC_DEFINE"),
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
