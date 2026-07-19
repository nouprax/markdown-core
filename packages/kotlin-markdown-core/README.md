# Kotlin Markdown Core

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

## Add the Dependency

Use the root coordinate from a Kotlin Multiplatform or Android project:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:1.0.3")
        }
    }
}
```

JVM-only Gradle and Maven consumers can use
`com.nouprax:kotlin-markdown-core-jvm:1.0.3`. Published targets are Android API
21 or later, JVM 17, macOS arm64, and Linux x64.

## Parse Markdown

```kotlin
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.ParseOptions

val document = Document.parse(
    "# Hello",
    ParseOptions(directives = false),
)

println(document.content.first()::class.simpleName)
println(document.dump())
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas, dollar and
LaTeX formula delimiters, and directives. The result is an immutable value tree
whose nodes carry a stable identity (`id`) and a change `revision`; equality is
O(1) over that pair. Absolute source scopes are resolved through the snapshot
with `document.scope(node)`. The package exposes parsing, incremental sessions,
and read-only AST traversal, not rendering or mutation.

## Traverse and Inspect

Use `Walker` for a depth-first traversal; every event carries the node's
resolved absolute scope:

```kotlin
import com.nouprax.markdown.core.WalkEvent
import com.nouprax.markdown.core.Walker

Walker.walk(document) { event, node, scope ->
    if (event == WalkEvent.ENTERING) {
        println("$node at ${scope.start.line}")
    }
}
```

`Document` exposes `dump()`, which delegates to the public `TreeDumper` and
returns the canonical file-tree diagnostic for the snapshot:

```kotlin
import com.nouprax.markdown.core.TreeDumper

val document = Document.parse("# Hello")
println(document.dump())
println(TreeDumper.dump(document))
```

## Incremental Sessions

`MarkupSession` owns one Markdown text and its living AST. Queue edits
(`append` is an edit at end-of-text), then `commit()`: the session reparses
only the stale region, keeps node identity wherever content is unchanged, and
returns a `Commit` holding the new immutable snapshot plus its `Delta` — the
exact ids that were `added`, `removed`, `changed`, or `bubbled`. Unchanged
nodes are the same objects across snapshots, so UI diffing is O(delta).

```kotlin
import com.nouprax.markdown.core.MarkupSession

MarkupSession().use { session ->
    session.append("# Title\n\nHello")
    val first = session.commit()
    session.append(" world")
    val second = session.commit()
    check(second.document.content[1].id == first.document.content[1].id)
    check(second.changes.added.isEmpty())
}
```

`updates(input: Flow<String>): Flow<Commit>` streams one commit per token, and
`footnote(id)` / `footnotes()` / `references(id)` answer footnote
numbering, resolution, and back-reference ordinals as queries against the
committed revision. Sessions are `AutoCloseable`; snapshots, deltas, and any
scopes materialized while their snapshot was current stay usable after
`close()`.

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.
