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

val document = Document(
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
equality is O(1) over that pair. Every node also carries its own `scope` — its
absolute start/end line and column — which is deliberately outside equality,
because position is not content. The package exposes parsing, editing, and
read-only AST traversal, not rendering or mutation.

`Document` owns a native parse and is `AutoCloseable`: `use { }` it, or hand it
to `edit`, which moves the parse to the successor. A document that is neither
closed nor edited is released when it becomes unreachable, but that is a
backstop, not the contract. Everything it produced — content, scopes,
diagnostics, dump — is a value and stays usable afterwards.

## Traverse and Inspect

Use `MarkupWalker` for a depth-first traversal. The callback overload emits
entering/exiting events with each node's absolute scope:

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
dispatches each node once in preorder without resolving scopes.

`Document` exposes `dump()`, which delegates to the public `MarkupDumper` and
returns the canonical file-tree diagnostic for the snapshot:

```kotlin
import com.nouprax.markdown.core.MarkupDumper

val document = Document("# Hello")
println(document.dump())
println(MarkupDumper.dump(document))
```

## Edit

There is no session type. A document is created from text and options, and
`edit` hands it new text: it returns the document that text describes plus the
`Delta` between the two. Options are fixed for a document's whole series —
changing what the parser means is a new `Document`, not an edit.

```kotlin
val document = Document("# Title\n\nHello")
val commit = document.edit("# Title\n\nHello world")
commit.document.use { next ->
    // The paragraph kept its identity, and its unchanged sibling compares
    // equal down to the last field. Neither is the SAME object — a document
    // is decoded from its own text, not carried over from the caller's last
    // one — and `scope` may still place them somewhere new, because an edit
    // above moves every position below it without changing any content.
    check(next.content[1].id == document.content[1].id)
    check(next.content[0] == document.content[0])
}
```

`edit` SUPERSEDES the receiver: the native parse moves to the successor, so the
document it was called on must not be edited again. Its already-extracted
values stay valid forever, because they are values.

### Most applications never read the delta

Hand `commit.document` to Compose and stop. The stability a reactive framework
needs is on the TREE, not in the delta: an unchanged node keeps its `id` and
its `revision`, equality is O(1) over that pair, and `id` goes unmodified into
a `key()`. Nothing in this package requires a delta to obtain, retain, walk, or
compare a document.

### The delta is for saying WHERE, and for state you keep yourself

`Delta` is one list, in the new document's postorder: every node whose
projection differs appears after all of its own children, and a retired node
appears where it was found — before its former parent's row. Each row says
which parts differ — `value`, `text`, `children`, `descendant` — and a row whose
`parts.isRetired` is a node that no longer exists.

`descendant` alone means "nothing of this node's own changed, something below
it did", which is what lets a highlighter light the run that actually changed
instead of the section containing it:

```kotlin
for (diff in commit.delta.diffs) {
    if (diff.parts.descendant && !diff.parts.value && !diff.parts.text) continue
    val node = commit.document.node(diff.markup) ?: continue
    highlight(node.scope)
}
```

The other reader is a consumer holding state it must edit in place rather than
re-derive — a display list, a text-measurement cache, an LSP token array. It
walks the same list and edits its own structure.

Streaming consumers edit on the render tick rather than on every socket
message, so the parse rate follows the display and not the socket.
`document.diagnostics` lists everything an editor should underline — which, for
Markdown, is one thing: a directive's `{...}` attribute block that did not
parse. Every other "wrong" construct is a defined outcome of the standard
semantics, not a failure.

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
  native-bridge and wire-decoder implementation names and members cannot be
  used from ordinary Java source.

Module-internal top-level helpers share one `MarkdownCoreKt` owner through
Kotlin's multifile-class mechanism, and each is `@JvmSynthetic`, so the facade
carries no Java-callable member at all. Its generated backing parts are
package-private on both JVM and Android; keep the matching `@JvmName` and
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
