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
and its three native method names remain stable while every other implementation
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

All parse options default to `true`: smart punctuation, footnotes, tables,
strikethrough, autolinks, task lists, formulas, directives, cross-links
(`[[reference]]`), and embeds (`![[reference]]`). The
`formulas` option controls formula fences and every supported formula delimiter,
including `$`, `$$`, `\\(...\\)`, and `\\[...\\]`. The result is an immutable
value tree whose nodes carry a stable identity (`id`) and a change `revision`;
equality is O(1) over that pair. Every node also carries its own `scope` — its
absolute start/end line and column — which is deliberately outside equality,
because position is not content. The package exposes parsing, appending, and
read-only AST traversal, not rendering or in-place mutation.

`Document` owns a native parse and is `AutoCloseable`: `use { }` it, and
`use { }` what `append` hands back too — a mutation leaves the
receiver's parse open for its holder to close, an O(1) release once it is
superseded. A document that is never closed is released when it becomes
unreachable, but that is a backstop, not the contract. Everything it
produced — content, scopes, diagnostics, dump — is a value and stays usable
afterwards.

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

## Append

There is no session type, and there is no whole-text edit. A document is the
live head of a chain, and the one mutation hands it trailing bytes: `append`
returns the next document. The chain contract is one sentence: **a mutation
advances the chain and supersedes its receiver — old heads refuse further
mutation deterministically, and decoded values live forever.** Options are
fixed for a chain's whole life, and text that is not an extension of the
chain's is a new `Document` — replacing the whole text means a fresh parse,
so the API says exactly that.

```kotlin
val document = Document("# Title\n\nHello")
val next = document.append(" world")
next.use {
    // The paragraph the append extended kept its identity, and its settled
    // sibling compares equal down to the last field — equality is the
    // (id, revision) pair, and appending never moves settled content.
    check(it.content[1].id == document.content[1].id)
    check(it.content[0] == document.content[0])
}
document.close()
```

Release each document when you are done with it — closing a superseded one
is an O(1) release, and its already-extracted values stay valid forever,
because they are values.

## Stream

`append` is the streaming hot path: hand every socket message to the chain's
head and render from the document that comes back.

```kotlin
var document = Document("")
for (message in stream) {
    val previous = document
    document = document.append(message)
    previous.close()
    render(document)
}
document.close()
```

Any byte split is legal — mid-word, mid-marker, even between the two
newlines of a block boundary — and an empty chunk still advances the chain,
to an identical tree. The per-tick cost is O(changed), not O(document):
appending never moves settled content, so a subtree the mutation did not
touch crosses the native boundary as a single reuse record and resolves to
the value the predecessor already decoded — the decode work per tick is
proportional to what the appended bytes changed, plus the trailing spine.
A failed append poisons the chain: every further mutation fails, every
document's values and `close` remain, and recovery is a new `Document`.

### What changed is asked of the tree

Hand the new document to Compose and stop. The stability a reactive framework
needs is on the TREE: an unchanged node keeps its `id` and its `revision`,
equality is O(1) over that pair, and `id` goes unmodified into a `key()`.

There is nothing else to read. A node's `revision` is the document revision at
which its own fields, child list, or any descendant last changed, so `(id,
revision)` is the entire update protocol: a node that kept its pair is
unchanged all the way down, and a consumer that wants to know WHERE a change
landed — a highlighter, a display list, an LSP token array — walks the new
tree and prunes there:

```kotlin
fun repaint(previous: Document, next: Document, node: Markup) {
    if (previous.node(node.id)?.revision == node.revision) return // unchanged, subtree included
    highlight(node.scope)
    // recurse into the children for the run that actually changed
}
```

A node the predecessor does not answer for is new; an id the successor no
longer answers for (`next.node(id) == null`) is retired. Creation and
retirement are presence questions, not a list to read.

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
- `verifyJvmImplementationHidden` and `verifyAndroidImplementationHidden` keep
  no baseline of their own: each checks that the classes ordinary Java source
  can actually use match that JVM API dump's inventory exactly, while Java
  compiler probes prove that native-bridge and wire-decoder implementation
  names and members cannot be used from ordinary Java source.

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

`kotlinTest` checks both baselines and the JVM and Android Java-visibility
gates. Release staging also runs the ABI checks on Linux and macOS so each
host validates its native target directly.
