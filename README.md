# Markdown Core

Markdown Core is a cross-platform Markdown parser with a shared immutable
abstract syntax tree (AST) for C, Swift, Kotlin, and ECMAScript. One C engine
provides consistent parsing behavior and source locations across all bindings.

The library provides synchronous parsing and read-only AST traversal. It has
one Markdown dialect with no parse options. Applications own rendering and
keep the source text when they need it; the parser does not retain the input.

Read the [Markdown syntax guide](docs/specs/dialect.md) to learn the language,
or the [documentation index](docs/specs/README.md) for the AST and source-location
contracts.

## Usage

### Swift

The Swift package supports iOS 26 and macOS 26 or later. Add the `MarkdownCore`
product to your target:

```swift
.package(url: "https://github.com/nouprax/markdown-core", from: "3.0.0")
```

```swift
import MarkdownCore

let document = try Document.parse("Hello, Markdown.")
print(document.dump())
```

The AST is an immutable, `Sendable` value tree with typed visitors and
stack-safe walking.

### Kotlin Multiplatform

Add the dependency to a Kotlin Multiplatform source set:

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

val document = Document.parse("Hello, Markdown.")
println(document.dump())
```

Supported targets are Android API 21 or later, JVM 17, macOS arm64, and Linux
x64. Native runtime dependencies are included. On JDK 26 or later, launch JVM
applications with `--enable-native-access=ALL-UNNAMED` to avoid the native-access
warning from the JNI loader.

### ECMAScript and TypeScript

```sh
pnpm add @nouprax/es-markdown-core
```

```js
import { Document, TreeDumper } from "@nouprax/es-markdown-core";

const document = Document.parse("Hello, Markdown.");
console.log(TreeDumper.dump(document));
```

The ESM package supports Node.js 20 or later and browsers that can load its
WebAssembly asset. Parsing is synchronous once the import resolves. TypeScript
values are recursively readonly; JavaScript objects are not runtime-frozen.

### C and C++

Link the installed CMake target:

```cmake
find_package(markdown-core CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE markdown-core::markdown-core)
```

Include `<markdown_core.h>` and call `markdown_core_document_parse`. Release
successful parses with `markdown_core_document_free`; node and string views
borrow from that document. Release errors and allocated dumps with
`markdown_core_error_free` and `markdown_core_dump_free`.

Independent parses may run concurrently. Read-only access to a document is
safe while it remains alive. See the [public C header](packages/markdown-core/include/markdown_core.h)
for the complete API and ownership rules.

## Development

Start with the [toolchain and environment guide](docs/toolchains.md), validate
your setup with `scripts/init-environment.sh --check`, then install dependencies:

```sh
pnpm install --frozen-lockfile
```

Build the package you are working on:

```sh
pnpm build:c
pnpm build:swift
scripts/gradle.sh :packages:kotlin-markdown-core:jvmJar
pnpm --dir packages/es-markdown-core build
```

The C CLI at `build/cmake/packages/markdown-core/core/markdown-core` reads files
or standard input and prints a debug AST dump. To install the C library:

```sh
cmake --install build/cmake --prefix /path/to/prefix
```

Run the tests for your host, such as `pnpm test:c-host` and
`pnpm conformance:c-host`. See [testing](docs/architecture/testing.md) for all
platform targets, sanitizers, conformance checks, and benchmarks. `pnpm verify`
runs the repository's formatting, lint, contract, and audit checks.

The four bindings live in `packages/`; shared executable fixtures live in
`specs/`. Reader documentation lives in `docs/specs/`, implementation design in
`docs/architecture/`, and consumer examples in `samples/`.

For releases, follow the [release guide](docs/releasing.md) and
[change log](CHANGELOG.md).

## License

Markdown Core is an independent project derived from cmark, cmark-gfm, and
[DongyuZhao/cmark-gfm](https://github.com/DongyuZhao/cmark-gfm). It preserves the
applicable upstream copyright and license notices. See [LICENSE](LICENSE),
[COPYING](COPYING), and [UPSTREAM.md](UPSTREAM.md) for licensing and lineage.
