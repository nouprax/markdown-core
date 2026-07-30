# Kotlin Markdown Core

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

## Add the Dependency

Use the root coordinate from a Kotlin Multiplatform or Android project:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:2.0.0")
        }
    }
}
```

JVM-only Gradle and Maven consumers can use
`com.nouprax:kotlin-markdown-core-jvm:2.0.0`. Published targets are Android API
21 or later, JVM 17, macOS arm64, and Linux x64.

### Android local and Robolectric tests

On a device or emulator the Android artifact loads its bundled native
library automatically. Local unit tests (including Robolectric) run on the
host JVM, where the Android artifact needs a host build of
`markdown_core_kotlin`. Provide it one of two ways:

- pass `-Dmarkdown.core.hostNativeLibrary=/path/to/libmarkdown_core_kotlin.dylib`
  (or `.so`) to the test JVM, or
- put a host build on the test classpath at
  `com/nouprax/markdown/core/native/<os>-<arch>/<library>`, the same layout
  the JVM artifact uses (for example
  `com/nouprax/markdown/core/native/macos-arm64/libmarkdown_core_kotlin.dylib`).

Without either, the first parse fails with an `IllegalStateException` that
names both remedies.

The Android AAR publishes its own narrowly scoped R8 consumer rule for the
private `JvmNative` linkage boundary. Applications can therefore enable
release shrinking without adding Markdown Core keep rules; the bridge class
and its 13 native method names remain stable while every other implementation
class stays eligible for shrinking, optimization, and obfuscation. If no
application path uses Markdown Core, R8 may remove the bridge as well.

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
stripping, tables, strikethrough, autolinks, task lists, formulas, and
directives, cross-links (`[[reference]]`), and embeds (`![[reference]]`). The
`formulas` option controls formula fences and every supported formula delimiter,
including `$`, `$$`, `\\(...\\)`, and `\\[...\\]`. The result is an immutable
value tree whose nodes carry a stable identity (`id`) and a change `revision`;
equality is O(1) over that pair. Absolute source scopes are resolved through the
snapshot with `document.scope(node)`. The package exposes parsing, incremental
sessions, and read-only AST traversal, not rendering or mutation.

## Traverse and Inspect

Use `MarkupWalker` for a depth-first traversal. The callback overload emits
entering/exiting events with the node's resolved absolute scope:

```kotlin
import com.nouprax.markdown.core.WalkEvent
import com.nouprax.markdown.core.MarkupWalker

MarkupWalker.walk(document) { event, node, scope ->
    if (event == WalkEvent.ENTERING) {
        println("$node at ${scope.start.line}")
    }
}
```

The typed-visitor overload, `MarkupWalker.walk(document, visitor)`, instead
dispatches each node once in preorder without resolving scopes. That
scope-free form remains valid for a retained session snapshot even if it was
superseded before scope materialization.

`Document` exposes `dump()`, which delegates to the public `MarkupDumper` and
returns the canonical file-tree diagnostic for the snapshot:

```kotlin
import com.nouprax.markdown.core.MarkupDumper

val document = Document.parse("# Hello")
println(document.dump())
println(MarkupDumper.dump(document))
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
    check(second.delta.added.isEmpty())
}
```

Streaming consumers keep the two primitives on their natural cadences:
`append` on every socket message (cheap — nothing parses), `commit()` on the
render tick, so messages between ticks conflate into one commit and the parse
rate follows the display, not the socket. `footnote(id)` / `footnotes()` /
`references(id)` answer footnote numbering, resolution, and back-reference
ordinals as queries against the committed revision. Sessions are `AutoCloseable`; snapshots, deltas, and any
scopes materialized while their snapshot was current stay usable after
`close()`.

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.

## Maintaining ABI Baselines

The package intentionally has two complementary ABI gates:

- `api/jvm/kotlin-markdown-core.api` and
  `api/kotlin-markdown-core.klib.api` freeze the public Kotlin/JVM and
  Kotlin/Native APIs. The KLIB baseline covers both `linuxX64` and
  `macosArm64`.
- `jvm-abi.txt` freezes the artifact's Java-source-callable bytecode, including
  JVM descriptors and inheritance. Its class inventory must match the
  documented JVM API dump exactly, while a Java compiler probe proves that
  native-bridge, wire-decoder, and resolver implementation names and members
  cannot be used from ordinary Java source.

Module-internal top-level helpers share the documented `FootnoteQueriesKt`
owner through Kotlin's multifile-class mechanism. Its generated backing parts
are package-private on both JVM and Android; keep the matching `@JvmName` and
`@JvmMultifileClass` file annotations together when moving those helpers.

After an intentional public Kotlin API change, update the metadata and KLIB
baselines from the repository root:

```sh
scripts/gradle.sh :packages:kotlin-markdown-core:updateKotlinAbi
```

After an intentional change to the JVM bytecode surface, update its separate
baseline:

```sh
scripts/gradle.sh :packages:kotlin-markdown-core:verifyJvmAbi -PwriteJvmAbi
```

`kotlinTest` checks both baselines and the JVM and Android Java-visibility
gates. Release staging also runs the ABI checks on Linux and macOS so each
host validates its native target directly.
