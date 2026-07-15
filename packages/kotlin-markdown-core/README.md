# Kotlin Markdown Core

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

## Add the Dependency

Use the root coordinate from a Kotlin Multiplatform or Android project:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:1.0.2")
        }
    }
}
```

JVM-only Gradle and Maven consumers can use
`com.nouprax:kotlin-markdown-core-jvm:1.0.2`. Published targets are Android API
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
with source scopes. The package exposes parsing and read-only AST traversal,
not rendering or mutation.

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
