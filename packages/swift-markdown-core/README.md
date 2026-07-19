# Swift Markdown Core

Swift bindings for the immutable Markdown Core AST, built directly on the C
parser as SwiftPM targets — no wire, no separate runtime.

## Add the Dependency

```swift
dependencies: [
    .package(url: "https://github.com/nouprax/markdown-core", from: "1.0.3")
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

let document = try Document.parse(
    "# Hello",
    options: ParseOptions(directives: false)
)

print((document.children.first as? Heading)?.level ?? 0)
print(document.dump())
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, dollar and
LaTeX formula delimiters, and directives. The result is an immutable `Sendable`
value tree whose nodes carry a stable identity (`id`, a `MarkupID` of the
owning session's `lineage` salt plus a raw value) and a change `revision`;
equality is O(1) over that pair, and an unchanged node compares equal across
consecutive snapshots — safe fast paths for render caches and reconciliation
keys. The package exposes parsing, incremental sessions, and read-only AST
traversal, not rendering or mutation.

Nodes do not store absolute positions. Resolve them through the snapshot:
`document.scope(of: node)` returns the node's absolute start/end line and
column.

## Traverse and Inspect

Use `Walker` for a read-only depth-first traversal; every event carries the
node's resolved absolute scope:

```swift
try Walker().walk(document) { event, node, scope in
    if event == .entering {
        print(type(of: node), scope.start.line)
    }
}
```

For typed dispatch, conform to `MarkupVisitor` and hand it to
`node.accept(&visitor)`. `document.dump()` and
`TreeDumper.dump(document, of: node)` emit the canonical diagnostic tree for
the complete document or a focused subtree (subtree scopes print with the
subtree as origin) — intended for logs, snapshots, and debugging rather than
persistence or data interchange.

## Incremental Sessions

`MarkupSession` owns one Markdown text and its living AST. Queue edits
(`append` is an edit at end-of-text), then `commit()`: the session reparses
incrementally, keeps node identity wherever content is unchanged, and returns
a `Commit` holding the new immutable snapshot plus its `Delta` — the exact ids
that were `added`, `removed`, `changed`, or `bubbled`. After any sequence of
edits and commits the document is semantically identical to a one-shot
`Document.parse` of the same final text.

```swift
let session = try MarkupSession()
try session.append("# Title\n\nHello")
let first = try session.commit()

try session.append(" world")
let second = try session.commit()
// The paragraph kept its identity; only its text advanced a revision.
```

`updates(feeding:)` is async sugar over the streaming hot path, yielding one
`Commit` per token:

```swift
for try await commit in session.updates(feeding: tokens) {
    render(commit.document, commit.delta)
}
```

Calls on one session must be externally synchronized (one writer at a time);
commits are transactional, so a failed commit leaves the session valid at its
previous revision. Snapshots are immutable `Sendable` values that share every
unchanged node with the previous snapshot and stay usable after the session
advances or deinitializes. `session.node(for:)` answers the committed value
for an id, and `session.footnotes()`, `session.footnote(of:)`, and
`session.references(of:)` answer footnote numbering, resolution, and
back-reference ordinals as queries against the committed revision.
