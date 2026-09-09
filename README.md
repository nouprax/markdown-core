# Markdown Core

Cross links (`[[Note#Heading|Label]]`) and embeds (`![[Image.png|100x145]]`)
produce `CrossLink(dest, label)` and `CrossEmbedded(dest, label, dimensions)`,
respectively. `dest` is a cross destination with
raw path and optional anchor; the label is null when no separator was authored
and an empty string for `[[Note|]]`. These are leaves with exhaustive visit and
walk callbacks. Resolving files, rendering and transclusion belong to consumers.

Markdown Core is a cross-platform Markdown parser that exposes the same
immutable abstract syntax tree (AST) in C, Swift, Kotlin, and ECMAScript. The C
engine and every binding live in this repository, so a release gives each
platform the same parser behavior, node model, source locations, and extension
defaults.

The project provides parsing and AST traversal, not Markdown rendering or AST
mutation. It inherits from the cmark and cmark-gfm projects and from the
independently developed fork at
[DongyuZhao/cmark-gfm](https://github.com/DongyuZhao/cmark-gfm). Parts of the
inherited implementation were rewritten before and after this repository was
created. Markdown Core is an independent project and does not plan to merge its
changes back upstream. See [UPSTREAM.md](UPSTREAM.md) for the exact baseline,
divergence history, and license relationship.

## Usage

All platform APIs have one synchronous parse entry point: `Document.parse` in
Swift, Kotlin, and ECMAScript, and `markdown_core_document_parse` in C.

A parse produces only the AST. Every node carries a **`scope`**: the pair of
`(line, column)` boundaries reported by the cmark-family parser. It is the
source-faithful editor cursor range of that node's own occurrence, not a byte
range or an expanded set of locations that supplied resolved or inherited
semantic values.

The parser does not retain or publish the input, a normalized source copy, a
line index, tokens, trivia, or recovery nodes. Consumers that need the Markdown
text keep their own input. The Swift, Kotlin, and ECMAScript bindings copy the
AST into platform values and retain no native parser handle; the C API exposes
an owned document with borrowed node views.

There are no parse options. Every parse recognizes the one Markdown Core
dialect: footnotes, tables, strikethrough, autolinks, task lists, formulas
(including dollar and LaTeX delimiters), and directives, always on, on the
CommonMark base; quotation marks, hyphens, and periods are stored as written.
`TreeDumper` and `dump()` produce a canonical debug representation for logs,
tests, and debugging; dump text is not a persistence or interchange format.

Task prefixes accept exactly one authored Unicode scalar, such as `- [?]`,
`- [✓]`, or `- [🚀]`, followed by a space, tab, vertical tab, or form feed.
The item preserves that scalar in `marker`; completion is derived. Recognition
is limited to the item's opening line and removes the prefix before deciding
its first block. Empty or multi-scalar markers and a missing separator remain
literal text.

[Block identifiers](docs/specs/dialect/block-identifiers.md) use `text #id#` to populate
`Paragraph.anchor`, or `- [✓] task #id#` to populate `ListItem.anchor`.
A standalone identifier line can attach to an eligible preceding list,
callout, or table under the module's boundary rules. The marker disappears
from visible content, while scopes retain the authored source positions.
Identifiers are declarations; target resolution belongs to consumers.

Within a pipe-table cell, write a cross-link label separator as `\|`, as in
`[[Note\|Label]]` or `![[asset\|100x145]]`. It remains inside that cell.
Inline `$x$` and display `$$` forms use `Formula` and `FormulaBlock` under the
[formula grammar](docs/specs/dialect/formulas.md). Ordinary fenced code remains `CodeBlock`
with its info, language label, and literal body. Code, formula, comment, HTML
token, and cross-reference payloads retain their ownership boundaries; text
between paired inline HTML tags remains eligible for Markdown parsing.

`==highlight==` produces `Mark` with parsed inline `content`, including nested
emphasis, links, and other inline nodes. Matching consumes two equals signs
at a time; unmatched signs remain text. Typed visitors and walking visitors
include the `Mark` case, and its scope covers both delimiters and the body.

`++inserted++` produces `Insertion` with parsed inline `content`. Repeated pairs
nest (`++++text++++`), and an odd leftover plus stays outside the matching
pairs (`+++text+++`). Insertion participates in exhaustive and walking visitors;
its scope includes the delimiters. Escapes and opaque bodies retain literal plus signs.

`^[inline note]` produces a one-item `Cite` and a document-owned `Footnote`
whose content holds the parsed inline body directly. Referenced definitions and
inline notes share `Document.footnotes` in source order. Generated `inline-N`
ids avoid every authored id; nested calls remain id edges and can be visited
without following semantic cycles.

`%%comment%%` produces `Comment`, the kind an HTML comment already produces,
inline or as a block when both `%%` fences stand on lines of their own under
the same container prefixes. The body is opaque and stored as written, nothing
is stripped, and a consumer that does not want comments drops the nodes.

Tables expose `columns: [TableColumn]` and three ordered row groups: `head`,
`content`, and `foot`. Each column carries alignment and an optional relative
width; each cell carries positive `rowspan`/`colspan` and its parsed `content`.
Pipe tables produce one head row, no foot rows, null widths, and unit spans.
Their inline content stays directly in the cell. Walkers visit the three groups
in that order. This replaces `alignments`, `header`, `rows`, and `isHeader`.

### Swift

The root Swift package supports iOS 26 and macOS 26 or later and exports the
`MarkdownCore` product and module:

```swift
.package(url: "https://github.com/nouprax/markdown-core", from: "3.0.0")
```

```swift
import MarkdownCore

let document = try Document.parse("# Hello")
print(document.dump())
```

The Swift AST is an immutable, `Sendable` value tree. The module also provides
exhaustive typed visitors and stack-safe, read-only depth-first walking through
`MarkupWalkingVisitor` and `Markup.walk(with:)`.

### Kotlin Multiplatform

Use the root Maven coordinate from a Kotlin Multiplatform source set:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:3.0.0")
        }
    }
}
```

```kotlin
import com.nouprax.markdown.core.Document

val document = Document.parse("# Hello")
println(document.dump())
```

The published targets are Android (API 21 or later), JVM 17, macOS arm64, and
Linux x64. Android's four-ABI JNI payload is an internal dependency; consumers
do not need a separate C or Prefab package. On JDK 26 or later, JVM applications
should launch with `--enable-native-access=ALL-UNNAMED` to avoid a restricted
native-access warning from the package-private JNI loader.

Kotlin/Native directly cinterops the read-only C facade. JVM and Android use a
separate one-call JNI payload. Neither adapter is shared with the ES/Wasm
linear-memory ABI; the targets share the AST contract, not a binding bridge.

### ECMAScript and TypeScript

Install the ESM package with your package manager:

```sh
pnpm add @nouprax/es-markdown-core
```

```js
import { Document, TreeDumper } from "@nouprax/es-markdown-core";

const document = Document.parse("# Hello");
console.log(document.content[0].kind, document.content[0].scope);
console.log(TreeDumper.dump(document));
```

The package supports Node.js 20 or later and browser environments that can load
its WebAssembly asset. Module import completes WebAssembly initialization, so
parsing is synchronous after the import resolves. The generated TypeScript
surface is recursively readonly; JavaScript objects are not runtime-frozen.
Native pointers, WebAssembly memory, and initialization internals are not
exported.

### C and C++

An installed CMake package exports one complete library target containing the
parser and all supported extensions:

```cmake
find_package(markdown-core CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE markdown-core::markdown-core)
```

Include the read-only facade as `#include <markdown_core.h>`. Parse with
`markdown_core_document_parse`, which takes the source bytes and nothing else,
and release every successful parse with `markdown_core_document_free`. Nodes and string views borrow from their owning
document and must not outlive it. Error objects and allocated dump buffers use
their corresponding `markdown_core_error_free` and `markdown_core_dump_free`
functions.

The library has no process-level initialization or writable parser globals.
Independent parse instances may run concurrently, including their first calls;
read-only document access is also safe. Callers must ensure that a document is
freed only after all access to it has finished. The complete C contract is in
[`markdown_core.h`](packages/markdown-core/include/markdown_core.h).

Every `>` container is a `Callout`. An opening `[!type]` line stores the type
as written in `variant`; optional `+` and `-` set `collapsed` to false and true.
The parsed inline `title` is visited before `content` and is never a content
child. A plain quote has null metadata, and a missing title stays null. Custom
types are preserved; default titles, aliases and styling belong to consumers.

Every Markup value also exposes `anchor` and `attributes`. Attributes contain
ordered `classes` and ordered `records` (`name`, `value`), with duplicates
preserved. Directives populate these fields through the shared Pandoc braced
attribute grammar; an absent or empty container produces empty attributes.
`Document.metadata` holds the first complete `---` envelope at the start of a
document. Metadata directly exposes ten optional fields:
`name`, `title`, `subtitle`, `time`, `date`, `authors`, `keywords`, `abstract`,
`state`, and `comment`. Unknown names, unnamed text, comments, invalid values,
and later duplicates are ignored; valid neighboring fields survive. `authors`
and `keywords` accept a single string, a bracketed array, or a block list.
`abstract` and `comment` accept single-line text and indented multiline text
with `: |`. Metadata stays outside Markup children and visitor callbacks.
Numbers retain exact decimal strings. Missing fields are null; an authored null
is a present scalar value. No field order or individual field scope is stored.
`Media.dimensions: Dimensions?` reads complete `W`, `WxH`, `alt|W` and
`alt|WxH` suffixes on direct and resolved images. Values range from 1 to
2147483647 without leading zeros; malformed suffixes remain parsed alt content.
Numeric-only labels have empty alt content. Embedded cross links use the same
size grammar in `CrossEmbedded.dimensions`, retaining the raw label prefix (empty
for size-only labels). Ordinary cross-link labels and invalid suffixes stay raw.
`Dimensions` is a node-independent value with required `width` and optional
`height`; it has no scope or visitor callbacks.

## Repository layout

- `packages/markdown-core`: C parser, public facade, CLI, extensions, and C tests.
- `packages/swift-markdown-core`: Swift binding, tests, and consumer fixture.
- `packages/kotlin-markdown-core`: Kotlin binding, platform runtimes, tests, and consumer fixtures.
- `packages/es-markdown-core`: ECMAScript/TypeScript package and WebAssembly runtime.
- `specs/canonical-ast`: shared, platform-independent AST conformance fixtures.
- `docs/specs/dialect.md`: the Markdown Core dialect index: the feature set,
  always on with no switches, the locked executable oracle of each feature,
  the cross-feature recognition order, opacity, failure, and limit rules.
- `docs/specs/dialect/`: one normative module per feature (base language,
  formulas, directives, attributes, anchors, links and images, cross links,
  footnotes, citations, comments, marks, strikethrough, superscript and
  subscript, inserted text, bracketed spans, task lists, lists, definition
  lists, callouts, block identifiers, properties, tables), plus
  `conflicts.md`, the register of source-versus-source collisions, settled
  decisions, and deliberate exclusions.
- `samples`: sample consumers and integration examples.
- `scripts`: repository build, formatting, lint, audit, and consumer-check entry points.

## Build

Set up or validate the pinned contributor toolchain with
[`docs/development-environment.md`](docs/development-environment.md). The
non-interactive entry points are `scripts/init-environment.sh --check` and
`scripts/init-environment.sh --install`.

Install the pinned JavaScript development dependencies before using the root
`pnpm` tasks:

```sh
pnpm install --frozen-lockfile
```

Build an individual package with its native toolchain:

```sh
# C library and CLI
pnpm build:c

# Swift package
pnpm build:swift

# Kotlin/JVM artifact and its native payload
scripts/gradle.sh :packages:kotlin-markdown-core:jvmJar

# ECMAScript package and WebAssembly module
pnpm --dir packages/es-markdown-core build
```

The C build can also be driven directly:

```sh
cmake --preset default
cmake --build --preset default --parallel
cmake --install build/cmake --prefix /path/to/prefix
```

Its CLI is written to
`build/cmake/packages/markdown-core/core/markdown-core`; it takes files or
standard input and prints the canonical AST dump, with no language switch.
The oracle gates and position audits run that same CLI: there is one language
and one parser, and nothing in the test tree parses a part of it. The main
CMake options
are `MARKDOWN_CORE_SHARED`, `MARKDOWN_CORE_STATIC`, `MARKDOWN_CORE_TESTS`, and
`MARKDOWN_CORE_WARNINGS_AS_ERRORS`. `MARKDOWN_CORE_BENCHMARKS` is off by
default and exists only for an explicit local measurement build.

## Test

Correctness and public-contract conformance are the test task families. Run
the targets for the platforms available on the current host:

```sh
# C host
pnpm test:c-host
pnpm conformance:c-host

# Swift on macOS
pnpm test:swift-macos
pnpm conformance:swift-macos

# Kotlin/JVM
pnpm test:kotlin-jvm
pnpm conformance:kotlin-jvm

# ECMAScript
pnpm test:es-node
pnpm test:es-browser
pnpm conformance:es-node
```

Kotlin also has explicit Android host, Android emulator, macOS arm64, and Linux
x64 targets following the same `test:<platform>` and
`conformance:<platform>` naming. Swift has separate iOS Simulator targets.
There is intentionally no cross-host aggregate: required CI runs every
supported platform target on an appropriate host, simulator, browser, or
device.

Performance measurement is an explicit C-host experiment and never a CI gate.
When a controlled environment is available, run `pnpm benchmark:c-host`. A
separate PR benchmark reports one fixed parser workload and binary size against
the exact PR base. The read-only PR workflow measures and uploads only the
untrusted head result. A privileged default-branch workflow reuses a trusted
exact-SHA baseline when one exists, or checks out, builds, and publishes that
base itself before validating both JSON inputs and updating the comment. It
never executes code from the PR head. Hosted-runner timing and RSS remain
informational and never determine pass/fail. The binding packages intentionally
expose no short wall-clock/RSS loops masquerading as cross-runtime diagnostics.

Run repository-wide formatting, lint, contract, topology, and public-surface
checks with:

```sh
pnpm verify
```

The C presets also provide AddressSanitizer, UndefinedBehaviorSanitizer, and
ThreadSanitizer builds. For example:

```sh
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset correctness-asan
```

Replace `asan` with `ubsan` or `tsan` and use the matching correctness preset.
Packaging and isolated consumer checks are available through
`pnpm audit:packages` and `pnpm check:kotlin-consumers`; the Swift consumer is
part of `pnpm test:swift-macos`, and the installed C consumer is exercised by
the C test suite.

## Contributing and releasing

Pinned compiler, SDK, runtime, and IDE versions are documented in
[docs/toolchains.md](docs/toolchains.md). Release maintainers must follow
[docs/releasing.md](docs/releasing.md), including the no-secret release dry run,
protected tag/environment approval, Maven signing, npm OIDC, artifact
attestation, and post-publication verification. Release notes start from
[CHANGELOG.md](CHANGELOG.md).

## License

Markdown Core preserves all applicable upstream copyright and license notices.
See [LICENSE](LICENSE), [COPYING](COPYING), and [UPSTREAM.md](UPSTREAM.md).

Attributes attach to inline code (``x`{.code}`), ATX and Setext headings,
fenced code, direct links/media, resolved references and angle autolinks.
Reference definitions can supply an anchor, classes and records. An occurrence's
nonempty anchor wins; its classes and records follow inherited declarations,
including duplicates. Image dimension suffixes and dimension attribute records
remain independent. All returned values use the binding's native collections
and remain usable after parsing finishes.

Parsed headings receive automatic anchors: `# Hello World` declares
`hello-world`, with `-1`, `-2`, and later suffixes for collisions. Explicit
anchors anywhere in the document are reserved first. `[Hello World]`,
`[Hello World][]`, and `[go][Hello World]` resolve to `#hello-world`, including
before the heading; an explicit reference definition takes priority. Labels
use authored heading text, so `# *Title*` is referenced by `[*Title*]`.
Heading attributes stay on the heading, and generated targets add no scope.
