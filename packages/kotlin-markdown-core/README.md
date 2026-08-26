# Kotlin Markdown Core

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

## Add the Dependency

Use the root coordinate from a Kotlin Multiplatform or Android project:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:3.0.0")
        }
    }
}
```

JVM-only Gradle and Maven consumers can use
`com.nouprax:kotlin-markdown-core-jvm:3.0.0`. Published targets are Android API
21 or later, JVM 17, macOS arm64, and Linux x64.

## Parse Markdown

The living `Document` is the one entry into the parser: it is fed text -- in
one piece or many -- and yields `Read` values, each the pair of the parse's
two total views: `semantic`, the tree, and `concrete`, the normalized source
its scopes are counted against. The whole-text parse is a one-chunk stream:

```kotlin
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.ParseOptions

val read = Document("# Hello", ParseOptions(directives = false)).seal()

println(read.semantic.content.first()::class.simpleName)
println(read.dump())
```

All parse options default to `true`: smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas (dollar and
LaTeX delimiters included), and directives. `semantic` is an immutable value
tree with source scopes; `concrete` carries the normalized source bytes with
`lines` and `offset(line)`. The package exposes parsing and read-only AST
traversal, not rendering or mutation.

## Stream Markdown

Every `feed` returns the read after those bytes -- a mid-stream projection
whose incomplete trailing line is not yet in it -- and `seal` ends the stream
and releases the native shell, returning the sealed read, identical for the
same bytes however they were fed. Chunks are raw UTF-8 and may end anywhere,
mid-character included:

```kotlin
import com.nouprax.markdown.core.Document

Document().use { document ->
    var updated = document.feed("# Str")
    updated = document.feed("eamed\n")
    println(document.seal().dump())
}
```

Every returned read is a plain value: it stays readable after later feeds and
after the document is closed. Sealing IS closing: after `seal`, every call
throws `IllegalStateException`. `close` releases a stream abandoned before
`seal` and is idempotent, which is what `use` leans on.

## Traverse and Inspect

Use `Walker` for a depth-first traversal:

```kotlin
import com.nouprax.markdown.core.WalkEvent
import com.nouprax.markdown.core.Walker

Walker.walk(read.semantic) { event, node ->
    if (event == WalkEvent.ENTERING) {
        println(node)
    }
}
```

Every immutable `Markup` exposes `dump()`, which delegates to the public
`TreeDumper` and returns the canonical file-tree diagnostic for that subtree:

```kotlin
import com.nouprax.markdown.core.TreeDumper

val read = Document("# Hello").seal()
println(read.dump())
println(TreeDumper.dump(read.semantic.content.first()))
```

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.
