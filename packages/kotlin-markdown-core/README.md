# Kotlin Markdown Core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(embedded, dest, label)`. `dest` is a cross destination with
raw path and optional anchor; the label is null when no separator was authored
and an empty string for `[[Note|]]`. These are leaves with exhaustive visit and
walk callbacks. Resolving files, rendering and transclusion belong to consumers.

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

Every Markup value also exposes `anchor` and `attributes`. Attributes contain
ordered `classes` and ordered `records` (`name`, `value`), with duplicates
preserved. Directives populate these fields through the shared Pandoc braced
attribute grammar; an absent or empty container produces empty attributes.
`Document.metadata` holds the first complete `---` envelope at the start of a
document. Its ordered `content` contains `data(MetadataRecord)` and
`comment(String)` cases: supported YAML properties become data, while comments,
non-YAML text, `...`, and unsupported members retain their source. Valid
properties before and after a failed member survive. Metadata stays outside
Markup children and visitor callbacks. Numbers retain exact decimal strings.
`Image.width` and `Image.height` remain absent until O9.

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
Ordered lists expose their `variant` and `delimiter`.

Task prefixes accept exactly one authored Unicode scalar, such as `- [?]`,
`- [✓]`, or `- [🚀]`, followed by a space, tab, vertical tab, or form feed.
The item preserves that scalar in `marker`; completion is derived. Recognition
is limited to the item's opening line and removes the prefix before deciding
its first block. Empty or multi-scalar markers and a missing separator remain
literal text.

`==highlight==` produces `Mark` with parsed inline `content`, including nested
emphasis, links, and other inline nodes. Matching consumes two equals signs
at a time; unmatched signs remain text. Typed visitors and walking visitors
include the `Mark` case, and its scope covers both delimiters and the body.

`^[inline note]` produces a one-item `Cite` and a document-owned `Footnote`
whose content holds the parsed inline body directly. Referenced definitions and
inline notes share `Document.footnotes` in source order. Generated `inline-N`
ids avoid every authored id; nested calls remain id edges and can be visited
without following semantic cycles.

`%%comment%%` produces `Comment`, the kind an HTML comment already produces,
inline or as a block when both `%%` fences stand on lines of their own under
the same container prefixes. The body is opaque and stored as written, nothing
is stripped, and a consumer that does not want comments drops the nodes.

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

Tables expose `columns`, `head`, `content`, and `foot`. Each `TableColumn` has
`alignment` and nullable `relative`; each `TableCell` has `rowspan`, `colspan`,
and direct inline or block `content`. Rows carry their cells and scope; group
ownership belongs to the table. Pipe tables have unit spans and no authored widths.
