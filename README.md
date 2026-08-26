# Markdown Core

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

In Swift, Kotlin, and ECMAScript the living **`Document`** is the one entry
into the parser: a `Document` is fed text — in one piece or many — and yields
**`Read`** values. Every `feed` returns the read after those bytes: an
immutable value the caller owns outright — a mid-stream projection whose
incomplete trailing line is not yet in it and whose open constructs are
projected as they stand. **`seal`** ends the stream and releases the native
shell, returning the sealed read — identical for the same bytes however they
were fed. The whole-text parse is one line, `Document(markdown).seal()`: a
one-chunk stream. Every read stays readable after every later feed and after
the document itself is gone. The streaming model is specified in
[docs/STREAMING.md](docs/STREAMING.md).

A `Read` is the parse under its two total views, and the pair is closed over
its own coordinate system. **`semantic`** (the root type `Semantic`, an
ordinary `Markup` node) is the tree with policy applied; every node carries a
**`scope`** — the pair of `(line, column)` **boundaries** the element
occupies, not a byte range, and no substring is taken with it. **`concrete`**
is what those numbers are counted against: the **normalized source** — UTF-8
as fed, every NUL replaced by the three bytes of U+FFFD, every line ending a
single `\n` and every line having one — with its line index (`lines`,
`offset(line)`), because an input containing a NUL has a buffer whose columns
no longer agree with the parser's.

In C the two views are siblings on one handle — a `markdown_core_document`
lends out the root node and the source — which is exactly the shape `Read`
copies out; the bindings retain no native parser handle inside a read. The C
facade keeps its own names and both of its entries:
`markdown_core_document_parse` for a complete input, and the session
(`markdown_core_session_new`, `_feed`, `_finish`, `_free`) for a stream.

The default parse options enable smart punctuation, footnotes, HTML comment
stripping, tables, strikethrough, autolinks, task lists, formulas (including
dollar and LaTeX delimiters), and directives. Each option can be disabled per
parse. `TreeDumper` and `dump()` produce a canonical diagnostic representation
for logs, tests, and debugging; dump text is not a persistence or interchange
format.

### Swift

The root Swift package supports iOS 18 and macOS 15 or later and exports the
`MarkdownCore` product and module:

```swift
.package(url: "https://github.com/nouprax/markdown-core", from: "3.0.0")
```

```swift
import MarkdownCore

let read = try Document(
    markdown: "# Hello",
    options: ParseOptions(directives: false)
).seal()
print(read.dump())
print(read.concrete.lines)

let streaming = try Document()
let updated = try streaming.feed(chunk: "# Str")
let grown = try streaming.feed(chunk: "eamed\n") // then .seal()
```

The Swift AST is an immutable, `Sendable` value tree. The module also provides
exhaustive typed visitors and read-only depth-first walking.

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
import com.nouprax.markdown.core.ParseOptions
import com.nouprax.markdown.core.Document

val read = Document("# Hello", ParseOptions(directives = false)).seal()
println(read.dump())
println(read.concrete.lines)

Document().use { document ->
    val updated = document.feed("# Str")
    val grown = document.feed("eamed\n") // then .seal()
}
```

The published targets are Android (API 21 or later), JVM 17, macOS arm64, and
Linux x64. Android's four-ABI JNI payload is an internal dependency; consumers
do not need a separate C or Prefab package. On JDK 26 or later, JVM applications
should launch with `--enable-native-access=ALL-UNNAMED` to avoid a restricted
native-access warning from the package-private JNI loader.

### ECMAScript and TypeScript

Install the ESM package with your package manager:

```sh
pnpm add @nouprax/es-markdown-core
```

```js
import { Document, TreeDumper, Walker } from "@nouprax/es-markdown-core";

const read = new Document("# Hello", { directives: false }).seal();
new Walker().walk(read.semantic, (event, node) => {
  console.log(event, node.kind, node.scope);
});
console.log(TreeDumper.dump(read.semantic));
console.log(read.concrete.lines);

using streaming = new Document(); // dispose() also works
const updated = streaming.feed("# Streamed\n");
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

Include the read-only facade as `#include <markdown_core.h>`. Pass `NULL` for
parse options to use the defaults, and release every successful parse with
`markdown_core_document_free`. Nodes and string views borrow from their owning
document and must not outlive it. Error objects and allocated dump buffers use
their corresponding `markdown_core_error_free` and `markdown_core_dump_free`
functions.

A streaming parse opens a session with `markdown_core_session_new`, feeds
chunks with `markdown_core_session_feed`, and seals the stream with
`markdown_core_session_finish`. `feed` and `finish` each return an owned
document released with `markdown_core_document_free`;
`markdown_core_session_free` releases the session itself. The C API also reports the parse's ordered
diagnostic list — the places where a construct the author wrote did not become
one and neither the tree nor the concrete view can say so — through
`markdown_core_document_diagnostic_count` and
`markdown_core_document_diagnostic_at`.

The library initializes itself on the first parse. Concurrent parsing and
read-only access are safe; callers must ensure that a document is freed only
after all access to that document has finished. The complete C contract is in
[`markdown_core.h`](packages/markdown-core/include/markdown_core.h).

## Repository layout

- `packages/markdown-core`: C parser, public facade, CLI, extensions, and C tests.
- `packages/swift-markdown-core`: Swift binding, tests, consumer fixture, and benchmarks.
- `packages/kotlin-markdown-core`: Kotlin binding, platform runtimes, tests, and consumer fixtures.
- `packages/es-markdown-core`: ECMAScript/TypeScript package and WebAssembly runtime.
- `specs/canonical-ast`: shared, platform-independent AST conformance fixtures.
- `samples`: sample consumers and integration examples.
- `scripts`: repository build, formatting, lint, audit, and consumer-check entry points.

## Build

Set up or validate the pinned contributor toolchain with
[`docs/deprecated/development-environment.md`](docs/deprecated/development-environment.md)
(archived with the engine reset, but still accurate for the toolchain). The
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
`build/cmake/packages/markdown-core/core/markdown-core`. The main CMake options
are `MARKDOWN_CORE_SHARED`, `MARKDOWN_CORE_STATIC`, `MARKDOWN_CORE_TESTS`, and
`MARKDOWN_CORE_WARNINGS_AS_ERRORS`.

## Test

Correctness, public-contract conformance, and benchmarks are separate task
families. Run the targets for the platforms available on the current host:

```sh
# C host
pnpm test:c-host
pnpm conformance:c-host
pnpm benchmark:c-host

# Swift on macOS
pnpm test:swift-macos
pnpm conformance:swift-macos
pnpm benchmark:swift-macos

# Kotlin/JVM
pnpm test:kotlin-jvm
pnpm conformance:kotlin-jvm
pnpm benchmark:kotlin-jvm

# ECMAScript
pnpm test:es-node
pnpm test:es-browser
pnpm conformance:es-node
pnpm benchmark:es-node
```

Kotlin also has explicit Android host, Android emulator, macOS arm64, and Linux
x64 targets following the same `test:<platform>` and
`conformance:<platform>` naming. Swift has separate iOS Simulator targets.
There is intentionally no cross-host aggregate: required CI runs every
supported platform target on an appropriate host, simulator, browser, or
device.

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
[docs/deprecated/toolchains.md](docs/deprecated/toolchains.md). The release
process itself is defined by
[`.github/workflows/release.yml`](.github/workflows/release.yml), which reads
release notes from `docs/deprecated/releases/<VERSION>.md` and provides a
`workflow_dispatch` recovery path. The archived runbook
[docs/deprecated/releasing.md](docs/deprecated/releasing.md) records the
surrounding practice — the no-secret release dry run, protected
tag/environment approval, Maven signing, npm OIDC, artifact attestation, and
post-publication verification — but has partially diverged from that workflow;
where the two disagree, the workflow is right. Release notes start from
[CHANGELOG.md](CHANGELOG.md).

## License

Markdown Core preserves all applicable upstream copyright and license notices.
See [LICENSE](LICENSE), [COPYING](COPYING), and [UPSTREAM.md](UPSTREAM.md).
