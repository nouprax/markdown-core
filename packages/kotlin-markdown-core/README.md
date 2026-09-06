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

val document = Document.parse("# Hello")

println(document.content.first()::class.simpleName)
println(document.dump())
```

`Document.parse` takes no options. It parses the one Markdown Core dialect,
in which every feature is always recognized: footnotes, tables,
strikethrough, autolinks, task lists, formulas, and directives, on the
CommonMark base. Quotation marks, hyphens, and periods are stored as written.
The result is an immutable value tree with source scopes. The package exposes
parsing and typed AST inspection, not rendering or mutation.

## Traverse and Inspect

`Markup.accept(visitor)` dispatches exactly one node to an exhaustive typed
`Visitor`. `Markup.walk(walkingVisitor)` performs a stack-safe depth-first walk
and dispatches `ENTERING` and `EXITING` to an exhaustive `WalkingVisitor` by
node kind. Each node-kind branch chooses its typed fields and content; there is
no public iterator or uniform child projection. A directive label is walked as
the named `label` field, not as directive content.

Every immutable `Markup` exposes `dump()`, which delegates to the public
`TreeDumper` and returns the canonical file-tree dump for that subtree:

```kotlin
import com.nouprax.markdown.core.TreeDumper

val document = Document.parse("# Hello")
println(document.dump())
println(TreeDumper.dump(document.content.first()))
```

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can load
the bundled native library without a restricted-native-access warning.
