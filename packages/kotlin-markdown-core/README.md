# Kotlin Markdown Core

Kotlin Multiplatform bindings for the immutable Markdown Core AST.

Consumers use the root coordinate `com.nouprax:kotlin-markdown-core:1.0.0` from
Gradle or the JVM target coordinate from Maven, then call
`Document.parse(source)` from `com.nouprax.markdown.core`.

Every immutable `Markup` exposes `dump()`, which delegates to the public
`TreeDumper` and returns the canonical file-tree diagnostic for that subtree:

```kotlin
val document = Document.parse("# Hello")
println(document.dump())
println(TreeDumper.dump(document.content.first()))
```

On JDK 26 and later, JVM applications should launch with
`--enable-native-access=ALL-UNNAMED` so the package-private JNI loader can call
`System.load` without a restricted-native-access warning.

The maintained public model layout mirrors the AST under `model/`, while
traversal lives under `walker/`. Because Kotlin crosses a serialized native
boundary, `wire/` centrally owns the private binary schema and exhaustive
dispatch; model files do not interpret bridge fields. Parsing constructs
immutable public values directly without `Any`, `WireNode`, or a second tree.
The aggregate `List`/`ListItem` types share `List.kt`, and the typed table
Markup nodes share `Table.kt`. `FootnoteDefinition` and `FootnoteReference`
share `Footnote.kt`.
