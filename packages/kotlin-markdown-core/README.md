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
stripping, tables, strikethrough, autolinks, task lists, formulas (dollar and
LaTeX delimiters included), and directives. The result is an immutable value tree
with source scopes. The package exposes parsing and read-only AST traversal,
not rendering or mutation.

## Stream Markdown

`Session` parses a document that arrives in pieces. Every `feed` returns the
immutable document after those bytes -- a mid-stream projection whose
incomplete trailing line is not yet in it -- and `finish` seals the stream,
returning the same document `Document.parse` produces for the same bytes.
Chunks are raw UTF-8 and may end anywhere, mid-character included:

```kotlin
import com.nouprax.markdown.core.Session

Session().use { session ->
    var updated = session.feed("# Str")
    updated = session.feed("eamed\n")
    println(session.finish().dump())
}
```

Every returned document is a plain value: it stays readable after later feeds
and after the session is closed. After `finish`, further `feed` and `finish`
calls throw `ParseException` with `ParseErrorCode.INVALID_ARGUMENT`; `close`
releases the session and is idempotent.

## Traverse and Inspect

Use `Walker` for a depth-first traversal:

```kotlin
import com.nouprax.markdown.core.WalkEvent
import com.nouprax.markdown.core.Walker

Walker.walk(document) { event, node ->
    if (event == WalkEvent.ENTERING) {
        println(node)
    }
}
```

Every immutable `Markup` exposes `dump()`, which delegates to the public
`TreeDumper` and returns the canonical file-tree diagnostic for that subtree:

```kotlin
import com.nouprax.markdown.core.TreeDumper

val document = Document.parse("# Hello")
println(document.dump())
println(TreeDumper.dump(document.content.first()))
```

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.
