# Swift Markdown Core

Swift bindings for the immutable Markdown Core AST, built directly on the C
parser as SwiftPM targets — no wire, no separate runtime.

## Add the Dependency

```swift
dependencies: [
    .package(url: "https://github.com/nouprax/markdown-core", from: "2.0.0")
],
targets: [
    .target(
        name: "App",
        dependencies: [.product(name: "MarkdownCore", package: "markdown-core")]
    )
]
```

Requires Swift 6 tools; published platforms are iOS 18 and macOS 15 or later.

## Parse Markdown

```swift
import MarkdownCore

let document = try Document(
    "# Hello",
    options: ParseOptions(directives: false)
)

print((document.content.first as? Heading)?.level ?? 0)
print(document.dump())
```

All parse options default to `true`: smart punctuation, footnotes, tables,
strikethrough, autolinks, task lists, formulas, directives, cross-links
(`[[reference]]`), and embeds (`![[reference]]`). The
single `formulas` switch controls `$…$`, `$$…$$`, LaTeX delimiters, and
`formula` fenced blocks. The result is an immutable `Sendable` value tree whose
nodes carry a stable identity (`id`, a `MarkupID` of the owning series salt
plus a raw value) and a change `revision`; equality is O(1) over that pair, and
an unchanged node compares equal across consecutive revisions, which is what a
render cache keys on.

Identity says nothing about POSITION. An edit that shifts text moves positions
without changing any node's content, and revisions deliberately do not
report that. A consumer that draws anything positional — gutter numbers,
underlines, a scroll anchor, a source map — must read it from the NEW
document's node even for one it skipped as unchanged.

The package exposes parsing, editing, and read-only AST traversal, not
rendering or mutation.

Every node carries its own `scope` — its absolute start and end line and
column — read in O(1) off the value. It is deliberately not part of `==`:
position is not content, so two nodes differing only in where they sit are
equal, which is what lets an edit above a node leave every reactive
comparison below it untouched.

## Traverse and Inspect

Use `MarkupWalker` for a read-only depth-first traversal; every event carries
the node's absolute scope:

```swift
try MarkupWalker().walk(document) { event, node, scope in
    if event == .entering {
        print(type(of: node), scope.start.line)
    }
}
```

For typed dispatch, conform to `MarkupVisitor` and hand it to
`node.accept(&visitor)`, or traverse the complete document via
`MarkupWalker().walk(document, visitor: &visitor)`. Directive labels
are first-class `DirectiveLabel` nodes:
`Directive.label` and `DirectiveBlock.label` are optional typed edges, while an
explicit empty `[]` is a non-nil label whose `content` is empty.
`document.dump()` and
`MarkupDumper.dump(document, of: node)` emit the canonical diagnostic tree for
the complete document or a focused subtree (subtree scopes print with the
subtree as origin) — intended for logs, snapshots, and debugging rather than
persistence or data interchange.

## Edit

There is no session type. A document is created from text and options, and
`edit(_:)` hands it new text: it returns the document that text describes.
Options are fixed for a document's whole series — changing what the parser
means is a new `Document`, not an edit.

```swift
let document = try Document("# Title\n\nHello")
let edited = try document.edit("# Title\n\nHello world")
// The paragraph kept its identity; only its text advanced a revision.
```

`edit` READS the receiver and takes nothing: the document it was called on
keeps everything it owns, stays usable, and may be edited again. Editing one
document twice gives two lines of descent, told apart by their revisions —
and, like nodes from two separate parses, nodes from two lines are not
comparable. Release a document when you are done with it; its
already-extracted values stay valid forever, because they are values.

Hand the returned document to SwiftUI and stop. The stability a reactive
framework needs is on the TREE: an unchanged node keeps its `id` and its
`revision`, `==` is O(1) over that pair, and `id` goes unmodified into
`ForEach(id:)`. What changed is on the tree too: a node that changed at this
edit — in its own fields, its child list, or somewhere below — carries the
new document's `revision`, and a node that kept its revision is, subtree
included, exactly what it was.

A document is immutable and `Sendable`; concurrent reads of one document are
safe from any thread. `document.node(_:)` answers this document's value for an
id, and `document.diagnostics` lists everything an editor should underline —
which, for Markdown, is one thing: a directive's `{...}` attribute block that
did not parse. Every other "wrong" construct is a defined outcome of the
standard semantics, not a failure.
