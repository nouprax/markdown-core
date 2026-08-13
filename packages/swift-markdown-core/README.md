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

The package exposes parsing, the two document mutations (edit and append),
and read-only AST traversal — not rendering, and not in-place tree mutation.

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

## Edit and Append

There is no session type. A document is the live head of a CHAIN: `edit(_:)`
hands it whole new text, `append(_:)` adds text at the end, and each returns
the next head. Options are fixed for the chain's whole series — changing what
the parser means is a new `Document`, not a mutation.

```swift
var document = try Document("# Title\n\nHel")
document = try document.append("lo")  // any split is legal, mid-word included
document = try document.edit("# Title\n\nHello world")
// The paragraph kept its identity; only its text advanced a revision.
```

The chain contract in one sentence: a mutation advances the chain and
supersedes its receiver — old heads stop mutating, decoded values live
forever. In detail: both mutations follow one rule (same chain, same series,
revision strictly +1 on the chain's own counter); mutating a superseded
document throws a deterministic error and disturbs nothing; and every read on
a superseded document — its values, scopes, `dump()`, `node(_:)`,
`diagnostics` — keeps answering from the state decoded when it was built,
long after the rest of the chain is gone. A failed `edit` supersedes nothing:
the receiver stays the head. A failed `append` poisons the chain — "the chain
is done": only reads and release remain, and recovery is a new chain built
from text the caller still holds. Appending an empty string is still a
mutation: the chain advances over an identical projection.

Streaming is `document = try document.append(chunk)` per message — never
re-send accumulated text. Per-tick work is O(changed), not O(document): a
node the append did not reach keeps its id, revision, and positions, and the
binding reuses its already-decoded value whole — subtree and all — rather
than rebuilding it, so decode work is proportional to what the append
changed plus the open frontier it grew.

Hand the returned document to SwiftUI and stop. The stability a reactive
framework needs is on the TREE: an unchanged node keeps its `id` and its
`revision`, `==` is O(1) over that pair, and `id` goes unmodified into
`ForEach(id:)`. What changed is on the tree too: a node that changed at this
mutation — in its own fields, its child list, or somewhere below — carries
the new document's `revision`, and a node that kept its revision is, subtree
included, exactly what it was.

Threading is the chain's one rule. A mutation is an exclusive operation on
its chain: all access to any document on the chain must happen before the
mutation begins or after it returns, and two mutations must be serialized by
the caller. Between mutations, concurrent reads of any document on the chain
— live head or superseded — are safe from any thread, and decoded values are
pure `Sendable` values, always safe everywhere. `document.node(_:)` answers
this document's value for an id, and `document.diagnostics` lists everything
an editor should underline — which, for Markdown, is one thing: a directive's
`{...}` attribute block that did not parse. Every other "wrong" construct is
a defined outcome of the standard semantics, not a failure.
