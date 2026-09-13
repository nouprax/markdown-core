# Swift ownership compared with swift-markdown

Comparison versions: swiftlang/swift-markdown
`75e3df1d7b664ef3c96595de36c243b98599b3bc` (main as of 2026-09-08),
swift-cmark `7898f1b3e4befeecee56cb4a3bc8eebd2cb63219`, and this repository at
`ccda8bd4`. Experiments used optimized builds on macOS arm64 with Swift 6.3.3.

## Definitions and responsibilities

swift-markdown also separates public node wrappers from internal storage:

- [RawMarkupData](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/RawMarkup.swift#L18)
  uses an enum for node payloads. `RawMarkup` uses `ManagedBuffer` to hold a
  header and strong child references in trailing storage.
- [MarkupChildren](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/MarkupChildren.swift#L16)
  is a public lazy `Sequence`. `makeMarkup` also contains an exhaustive switch
  from kind to public type.
- [_MarkupData](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/MarkupData.swift#L112)
  additionally records the parent and occurrence identity for parent navigation
  and persistent editing. Root identity uses an atomic counter as part of that
  editing and identity model; iterative destruction does not require it.

Collection views, payload enums, and public type wrappers therefore have valid
roles. In this repository, `Fields` stores the actual fields and public types
provide access to them. They do not hold two independent copies that require
synchronization. Internal integer indices require kind validation; public fields
remain statically typed. Treating these structures alone as overengineering, or
equating their internal checks with weaker public type guarantees, overstates
the problem.

`MarkupCollection` keeps access to child relations, count, and subscripts at
O(1), without allocating a child array on each read. `StoredMarkup` allows a flat
array to hold heterogeneous payloads while preventing container views from
reentering records and creating recursive ownership. Names and per-type access
code can be improved; those readability concerns alone do not invalidate the
ownership model.

Collections are stored directly in their owning node's `Fields`: ordinary
relations use `[Int]`, and `Definition.content` uses `[[Int]]`. `MarkupGroups`
returns inner `MarkupCollection` views on demand. Obtaining views and accessing
subscripts are both O(1), with no per-group materialization. The former design
that stored body collections as separate records has been removed; groups no
longer enter the storage enum or construction queue.

## Field queries and node identity

`MarkupStore` owns the records and centralizes typed field queries, node
projection, and collection construction. All 29 container or scoped value types
declare their fields through `@Stored var fields: Fields`; node types no longer
access records directly, match the storage enum, or repeat error checks.
`Stored` is an ordinary generic property wrapper containing the store and a
private index. It introduces no payload copy, cache, additional heap object, or
macro dependency. `$fields` provides the store needed to query relations.

The index identifies a particular occurrence within a store. One store can
contain multiple Paragraph nodes, and different stores can hold different nodes
at the same position, so a type alone cannot identify the correct fields.
Location details are centralized in the reference implementation; semantic node
types do not depend on array indices.

The comparison used the distributed accessors at `72f14045` as its baseline,
with the same optimized C objects and Swift `-O -whole-module-optimization`
builds on arm64 with Apple Swift 6.3.3:

| Diagnostic | Original accessors | Centralized queries |
| --- | ---: | ---: |
| TableCaption / Paragraph / Document stride | 16 bytes each | 16 bytes each |
| 4,000 rich-text paragraphs, one read pass | 2.917 ms | 2.848 ms |
| 1,000 callout/directive/table groups, one read pass | 0.340 ms | 0.338 ms |
| Same rich-text input, parse + release | 9.521 ms | 9.445 ms |

The read experiment accessed the scope of top-level blocks and the direct
inline children of paragraphs, rather than all descendants. The two programs
ran sequentially; each result is the median of five samples. These differences
do not establish a stable speedup, but no read regression from centralizing
queries was observed. In optimized LLVM IR, the Paragraph and TableCaption
scope getters contain neither dynamic casts nor heap allocations. Allocation
call sites in node projection also match the baseline. Typed queries specialize
to the corresponding case checks without an intermediate step that erases and
reboxes `Fields`.

The earlier experiment that stored complete Fields directly in each node
remains rejected: TableCaption grew from 16 to 64 bytes, and projection added
an 80-byte heap object. That finding applies to inline payload storage; it does
not argue against centralizing queries in the store. The current implementation
retains small references and removes the distributed accessors.

## Upstream destruction fix and remaining lifecycle boundary

[PR #276](https://github.com/swiftlang/swift-markdown/pull/276) merged on
2026-06-22. At the compared revision,
[RawMarkup.deinit](https://github.com/swiftlang/swift-markdown/blob/75e3df1d7b664ef3c96595de36c243b98599b3bc/Sources/Markdown/Base/RawMarkup.swift#L164)
retains children on a work stack, deinitializes child references, and continues
dismantling a child's children after establishing unique ownership, then sets
childCount to zero. This algorithm fixes downward recursive destruction of
RawMarkup.

Experiments used the actual upstream library, public APIs, and separate
processes:

| Experiment | 30,000 levels | 65,536 levels |
| --- | --- | --- |
| Repeatedly construct `BlockQuote([node])`, then release the root normally | Passed | Passed |
| Parse repeated `> ` with `Document(parsing:)`, then release the root normally | Passed | Passed |
| After construction, descend with `child(at: 0)`, retain the deepest Text, then release normally | Passed | SIGSEGV |
| Same deepest-Text experiment, printing before `_exit(0)` skips destruction | Not run | Passed |
| Apply `detachedFromParent` immediately at each descent, then release normally | Not run | Passed |

The failing program printed that it had reached level 65,537, that the literal
was `leaf`, and that release was about to begin. Host LLDB showed repeated
`destroyGenericBox` and `_swift_release_dealloc` frames, with `EXC_BAD_ACCESS`
while pushing a stack frame. Together with the strong
`_MarkupData.parent: Markup?` reference and the control that detached at every
step, the evidence points to the ownership chain through public nodes' parent
existentials. RawMarkup's iterative destructor does not cover that upward
chain. The precise failure depth depends on the compiler, thread stack, and
other execution conditions.

In this repository, descending a 65,536-level list to the deepest Paragraph and
discarding the Document completed 131,073 parent-to-child moves and then
released normally. Existing root and independently retained container release
regressions also passed. This compares lifecycle boundaries, not the performance
ranking of the two input shapes.

Reproduction sketch using upstream public APIs:

```swift
@inline(never) func build(_ depth: Int) -> any BlockMarkup {
    var node: any BlockMarkup = Paragraph(Text("leaf"))
    for _ in 0..<depth { node = BlockQuote([node]) }
    return node
}

@inline(never) func exercise(_ depth: Int) {
    var node: any Markup = build(depth)
    while let child = node.child(at: 0) { node = child }
    withExtendedLifetime(node) {
        print("before release")
        fflush(nil)
    }
}

exercise(65_536)
print("released")
```

## Alternatives and decision

| Approach | Benefit | Cost or unmet lifecycle boundary |
| --- | --- | --- |
| Restore recursive `[Markup]` and clean up only when Document ends | Simple public array API | Independently retained subtrees released after Document still trigger recursive ARC destruction. |
| Keep flat storage but return a materialized array from every getter | Preserves the array type | Each access copies O(k) child values and allocates an array; caching adds state and ownership concerns. |
| Adapt upstream ManagedBuffer without the parent/editing mechanisms this library does not need | Retains memory per subtree; trailing storage may improve locality | A viable alternative, but still requires payload and child-relation views. Adds manual initialization/deinitialization, unique-reference checks, and a destruction work stack. Named relations and Sendable boundaries would still need to be established after porting. |
| Current immutable flat Swift storage | No manual destruction, cross-parse state, or locks; Swift checks Sendable conformance; bounded release stack | Container subtrees or child relations retain the entire Swift store, and public child relations become collection views. |

There is currently no evidence that an alternative is both simpler and meets
all constraints, so the decision is to retain flat storage. The basis is this
library's read-only model, which needs neither parent navigation nor editing:
immutable integer edges directly eliminate recursive ownership. If real editor
usage later shows that retaining a small subtree for a long time keeps enough
otherwise-unused document memory alive to become a major cost, reconsider
storage owned per subtree. Custom destruction is not justified by a workload
that has not yet been measured.

The upstream lifecycle comparison itself did not change production code. The
complete programs, build logs, exit statuses, and backtraces were kept in the
locally ignored `build/review/` directory: `upstream-release.swift`,
`upstream-release-results.json`, `upstream-descendant-backtrace.log`,
`upstream-detached-each-step.log`, and `our-descendant-release.log`. The comparison
does not adopt upstream source-range conversions; this library's cmark UTF-8
editor scope contract remains unchanged.
