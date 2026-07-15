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

All platform APIs have one synchronous parse entry point: `Document.parse` in
Swift, Kotlin, and ECMAScript, and `markdown_core_document_parse` in C. Parsing
produces a complete AST. The Swift, Kotlin, and ECMAScript bindings copy that
AST into platform values and retain no native parser handle after the parse
returns; the C API exposes an owned document with borrowed node views.

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
.package(url: "https://github.com/nouprax/markdown-core", from: "1.0.2")
```

```swift
import MarkdownCore

let document = try Document.parse(
    "# Hello",
    options: ParseOptions(directives: false)
)
print(document.dump())
```

The Swift AST is an immutable, `Sendable` value tree. The module also provides
exhaustive typed visitors and read-only depth-first walking.

### Kotlin Multiplatform

Use the root Maven coordinate from a Kotlin Multiplatform source set:

```kotlin
kotlin {
    sourceSets {
        commonMain.dependencies {
            implementation("com.nouprax:kotlin-markdown-core:1.0.2")
        }
    }
}
```

```kotlin
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.ParseOptions

val document = Document.parse(
    "# Hello",
    ParseOptions(directives = false),
)
println(document.dump())
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

const document = Document.parse("# Hello", { directives: false });
new Walker().walk(document, (event, node) => {
  console.log(event, node.kind, node.scope);
});
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

Include the read-only facade as `#include <markdown_core.h>`. Pass `NULL` for
parse options to use the defaults, and release every successful parse with
`markdown_core_document_free`. Nodes and string views borrow from their owning
document and must not outlive it. Error objects and allocated dump buffers use
their corresponding `markdown_core_error_free` and `markdown_core_dump_free`
functions.

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
[docs/toolchains.md](docs/toolchains.md). Release maintainers must follow
[docs/releasing.md](docs/releasing.md), including the no-secret release dry run,
protected tag/environment approval, Maven signing, npm OIDC, artifact
attestation, and post-publication verification. Release notes start from
[CHANGELOG.md](CHANGELOG.md).

## License

Markdown Core preserves all applicable upstream copyright and license notices.
See [LICENSE](LICENSE), [COPYING](COPYING), and [UPSTREAM.md](UPSTREAM.md).
