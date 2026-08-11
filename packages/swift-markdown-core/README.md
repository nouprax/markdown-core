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

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, and
directives, cross-links (`[[reference]]`), and embeds (`![[reference]]`). The
single `formulas` switch controls `$…$`, `$$…$$`, LaTeX delimiters, and
`formula` fenced blocks. The result is an immutable `Sendable` value tree whose
nodes carry a stable identity (`id`, a `MarkupID` of the owning lineage's salt
plus a raw value) and a change `revision`; equality is O(1) over that pair, and
an unchanged node compares equal across consecutive revisions — safe fast paths
for render caches and reconciliation keys. The package exposes parsing,
editing, and read-only AST traversal, not rendering or mutation.

Nodes do not store absolute positions. Resolve them through the document:
`document.scope(of: node)` returns the node's absolute start/end line and
column.

## Traverse and Inspect

Use `MarkupWalker` for a read-only depth-first traversal; every event carries the
node's resolved absolute scope:

```swift
try MarkupWalker().walk(document) { event, node, scope in
    if event == .entering {
        print(type(of: node), scope.start.line)
    }
}
```

For typed dispatch, conform to `MarkupVisitor` and hand it to
`node.accept(&visitor)`, or traverse the complete document without resolving
scopes via `MarkupWalker().walk(document, visitor: &visitor)`. Directive labels
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
`edit(_:)` hands it new text: it returns the document that text describes plus
the `Delta` between the two. Options are fixed for a document's whole lineage —
changing what the parser means is a new `Document`, not an edit.

```swift
let document = try Document("# Title\n\nHello")
let commit = try document.edit("# Title\n\nHello world")
// The paragraph kept its identity; only its text advanced a revision.
```

`edit(_:)` is `consuming`: the native parse moves to the successor, so the
document it was called on must not be edited again. Its already-extracted
values, scopes, and diagnostics stay valid forever, because they are values.

A `Delta` is one list, in the new document's postorder: every node whose
projection differs appears after all of its own children, and a retired node
appears where it was found — before its former parent's row. Each
row says WHICH parts differ — `value`, `text`, `children`, `descendant` — and a
row with no parts is a node that no longer exists. A renderer reconciling by id
reads the list once, front to back.

```swift
func renderTick(_ document: consuming Document, text: String) throws -> Document {
    let commit = try document.edit(text)
    for diff in commit.delta.diffs {
        guard let node = commit.document.node(diff.markup) else {
            retire(diff.markup)
            continue
        }
        refresh(node, diff.parts)
    }
    return commit.document
}
```

A document is immutable and `Sendable`; concurrent reads of one document are
safe from any thread. `document.node(_:)` answers this document's value for an
id, and `document.diagnostics` lists everything an editor should underline —
which, for Markdown, is one thing: a directive's `{...}` attribute block that
did not parse. Every other "wrong" construct is a defined outcome of the
standard semantics, not a failure.
