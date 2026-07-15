# Phase 12: Kotlin Multiplatform binding

Phase 12 introduces `com.nouprax:kotlin-markdown-core:1.0.0`, whose public API
lives in `com.nouprax.markdown.core` and `commonMain`. The binding exposes only
the immutable AST, `ParseOptions`, `Document.parse`, typed `Visitor<Result>`,
the read-only `Walker` with `WalkEvent.ENTERING`/`EXITING` callbacks, and public
`TreeDumper`/`Markup.dump()` subtree diagnostics.
`Visitor<Result>` requires a method for every concrete `Markup`; it has no
default implementation or catch-all method.

> Phase 18 replaces the package-local expected tree literal with build-generated
> common-test data derived from `specs/canonical-ast/manifest.json`. JVM, Android
> host/device, macOS ARM64, and Linux x64 compile the same generated cases; no
> publication or tracked platform copy contains the spec.

## Toolchain policy

The repository uses the latest stable compatible toolchain available on
2026-07-12: Kotlin/KMP 2.4.0, AGP 9.2.1, Gradle 9.6.1, JDK 26, Android
compile/target SDK 37, and AGP's tested default NDK 28.2.13676358. Dependency
resolution rejects prerelease Kotlin components; this prevents the Kotlin ABI
validation compatibility range from silently selecting 2.4.20 Beta artifacts.

AGP 9.2 embeds Kotlin 2.2 for Android consumers. The library is therefore built
with the Kotlin 2.4.0 compiler while its language/API metadata and explicit
stdlib dependency remain at the 2.2 compatibility baseline (stdlib 2.2.21).
This preserves Android root-coordinate consumption without downgrading the
build toolchain.

## Targets and native payloads

The declared KMP targets are Android, JVM, macOS arm64, and Linux x64.
Kotlin/Native uses cinterop against a statically linked copy of the same C
facade. JVM uses a package-private JNI bridge and selects the native resource by
OS/architecture. The CI matrix builds and runs the macOS arm64 and Linux x64
payloads on their native hosts; the controlled release job must aggregate those
host outputs before the single JVM coordinate is deployed.

Android uses a separate runtime AAR,
`com.nouprax:kotlin-markdown-core-android-runtime:1.0.0`, so the legacy C/Prefab
AAR remains independent. The runtime AAR contains
`libmarkdown_core_kotlin.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and
`x86_64`. Only the JNI entry point is exported from the private runtime shared
library.

The bridge passes source text as standard UTF-8 bytes. It walks typed C
accessors and writes the private, length-prefixed, kind-specific `MKC2` binary
representation; it does not use JNI modified UTF-8, JSON, or the C AST dump.
The common reader and exhaustive decoder own the complete private protocol:
they consume a kind-specific payload and directly construct the corresponding
immutable public model in one pass. Wire interpretation is intentionally not
distributed through public model files. There is no generic `WireNode`,
`Any`-typed intermediate tree, universal field-slot record, or second AST
allocation, so no native document ownership escapes `Document.parse`.

Public model sources are split by AST concept under `model/`; `Code` and
`CodeBlock`, `Formula` and `FormulaBlock`, `HTML` and `HTMLBlock`, and
`Directive` and `DirectiveBlock` remain separate files. The aggregate
`List`/`ListItem` types share `List.kt`, the two footnote roles share
`Footnote.kt`, and the tightly coupled `Table`/`TableRow`/`TableCell` Markup
types share `Table.kt`. `Visitor` and `Walker` live under
`walker/`, while `wire/` centrally owns primitive reading, kind enumeration,
immutable-list support, protocol validation, and direct model construction.
This differs deliberately from Swift's concept-local C accessor initializers
because Kotlin crosses a serialized KMP/native boundary.

JDK 26 consumers pass `--enable-native-access=ALL-UNNAMED`; this authorizes the
unnamed JVM module containing the private JNI loader and avoids the JDK native
access warning.

## Publications

`publishKotlinToMavenLocal` creates the publications that are buildable on the
current host:

- `com.nouprax:kotlin-markdown-core:1.0.0` (KMP root metadata)
- `com.nouprax:kotlin-markdown-core-jvm:1.0.0` (JAR)
- `com.nouprax:kotlin-markdown-core-android:1.0.0` (KMP Android AAR)
- `com.nouprax:kotlin-markdown-core-android-runtime:1.0.0` (internal four-ABI JNI runtime)
- `com.nouprax:kotlin-markdown-core-macosarm64:1.0.0` on macOS
- `com.nouprax:kotlin-markdown-core-linuxx64:1.0.0` on Linux

KMP root and target publications retain Gradle Module Metadata, POM metadata,
sources, and documentation artifacts. Native publications additionally carry
the KLIB and cinterop KLIB. A release is incomplete unless the controlled
deployment contains both host-specific Native publications and the aggregated
JVM payload; that coordinated publication gate remains part of Phase 20.

## Verification

Each `test:kotlin-*` platform task delegates directly to its Gradle/KMP
correctness task; each `conformance:kotlin-*` task delegates to the paired
named conformance task or Android instrumentation selection. Correctness runs
API, option, Unicode, failure, ownership, and robustness checks. Conformance
independently walks the Kotlin AST and verifies the public schema mapping.
Its package-local focused tree snapshot is produced by the public `TreeDumper`
using an exhaustive `Visitor<DumpRecord>` plus `Walker` enter/exit events.
Every `Markup.dump()` delegates to it; the implementation lives in
`commonMain` and does not read the C dump or C-owned goldens.

Independent consumers cover KMP Gradle root-coordinate selection, the JVM
target coordinate through Gradle Module Metadata, and Android root-coordinate/AAR
selection. The JVM consumer runs `Document.parse`, proving the native payload
loads. Because the supported consumer contract also includes real Maven projects,
Phase 19 adds a repo-owned Maven Wrapper smoke for Maven's own effective model,
dependency resolution, and lifecycle. CI builds the Android consumer APK; the product's separate
managed-emulator test validates the packaged Android JNI libraries on-device.

`pnpm benchmark:kotlin-jvm` is a separate deterministic warmup/repeat JVM/JNI
parse and immutable-copy target. It is never part of correctness and runs only
in the scheduled/manual benchmark workflow.
