# Phase 15: package-owned tests and contracts

> Phase 18 supersedes only canonical golden ownership: the sole runner-free
> cross-product corpus now lives at `specs/canonical-ast/`. All correctness,
> consumer, compile-contract, packaging, and runner ownership below remains
> package-local, and root `tests/` remains forbidden.

## Ownership result

Phase 15 removes the root `tests/` tree. Every test implementation, fixture,
consumer, compile contract, and packaging check now has one package owner:

| Previous root responsibility | Package owner and current location |
| --- | --- |
| canonical Markdown/`.ast` goldens and coverage manifest | shared product contract, `specs/canonical-ast/` (Phase 18) |
| corpus policy and future vendored corpora | C, `packages/markdown-core/tests/corpora/` |
| C/C++ API, facade, pathological, fuzz, robustness, conformance, and consumer suites | C, `packages/markdown-core/tests/` and its sole CMake/CTest graph |
| Swift API, schema mapping, Unicode, errors, ownership, `Sendable`, and robustness suites | Swift, `packages/swift-markdown-core/Tests/MarkdownCoreTests/` |
| SwiftPM consumer package | Swift, `packages/swift-markdown-core/Tests/Consumer/`; its test target directly depends on the public product |
| Kotlin common/JVM/Android/Native tests and packaging | Kotlin, `packages/kotlin-markdown-core/src/` and package Gradle tasks |
| Kotlin KMP/JVM/Android consumers | Kotlin, `packages/kotlin-markdown-core/consumers/` |
| Kotlin compile contracts | Kotlin, `packages/kotlin-markdown-core/contracts/` |
| Kotlin four-ABI JNI AAR | Kotlin internal runtime, `packages/kotlin-markdown-core/android-runtime/` |
| ES Node/browser/types/schema/ownership/robustness/package/consumer suites | ES, `packages/es-markdown-core/tests/` |

The Android runtime keeps a separate Gradle module only because the Android-KMP
plugin does not support `externalNativeBuild`. Its project path is
`:packages:kotlin-markdown-core:android-runtime`, its internal Maven artifact is
`kotlin-markdown-core-android-runtime`, and its AAR is `android-runtime-release.aar`.
Consumers continue to declare only `com.nouprax:kotlin-markdown-core` (or the
documented JVM target coordinate).

## Binding conformance contract

The C package remains the parser/facade semantic source and exclusively owns
the complete canonical goldens. Swift `ConformanceSuite`, Kotlin/JVM `AstTest`,
and ES `--suite conformance` use package-local focused Markdown strings. Each
binding independently proves that the public C facade schema maps through its
public API:

- all 28 `Markup` node kinds, including `TableRow` and `TableCell`;
- behavior-bearing fields, enums, booleans, child order, and nullable values;
- UTF-8 strings and start/end scope values;
- binding-specific error and ownership behavior.

No binding test imports C test data, calls or reads the C dump, invokes a C test
runner, or compares another binding's output. Each binding owns a public
Visitor/Walker `TreeDumper` and a package-local focused snapshot of the
canonical grammar. Conformance uses independent native targets and is excluded
from ordinary correctness discovery. Large/deep/repeated inputs use
correctness `robustness` cases; benchmark alone owns performance timing and
baselines. No public `stress` task exists.

## Layout guard

`scripts/audit-test-topology.sh` now fails when:

- a root `tests/` path or retired top-level Android runtime module exists;
- canonical parser goldens appear anywhere outside the C package;
- a binding test references C test data or canonical dump helpers;
- the package-owned Swift consumer, Kotlin consumers/contracts/runtime, or ES
  test layout is absent.
- the Swift consumer introduces a dummy executable/`main.swift` instead of
  testing the public `MarkdownCore` product directly.

The existing audit continues to enforce package-native routing, CTest discovery,
suite labels, corpus policy, offline execution, and non-empty Swift discovery.

## Validation

Validated on 2026-07-12:

- C facade and dump conformance through the relocated goldens;
- Swift full correctness and relocated SwiftPM consumer package;
- Kotlin/JVM focused conformance and nested Android runtime model/build;
- Kotlin KMP/JVM Gradle and Android consumer builds from an isolated local Maven
  repository; the real-Maven compatibility smoke is owned by Phase 19 and will
  use a repo-owned Maven Wrapper instead of global `mvn`;
- ES Node, browser, types, independent conformance, robustness, consumer, and packaging checks;
- canonical coverage, layout/test-topology, package-content, formatting, lint,
  contract, and Gradle model checks.

Root scripts expose only explicit `test:<platform>`, `conformance:<platform>`,
and `benchmark:<platform>` tasks; no cross-host aggregate, fixture, case,
normalization, or cross-language harness logic exists.
Each task family subdivides exactly once by real execution platform. Suite/case
discovery stays in named package-native targets; root scripts expose no intermediate layer,
language aggregate, suite matrix or `:full` alias. Phase 7 is closed; actual
required-CI execution of the Linux x64 and repo-managed Android emulator
correctness/conformance targets is deferred to Phase 19 CI acceptance.

Moving the C Markdown inputs through the patch workflow normalized redundant
trailing blank lines in `blocks.md` and `scopes.md`. The reviewed root document
end scopes changed from `18:0` to `17:17` and from `5:0` to `4:0`; node kinds,
children, fields, and all other scope expectations are unchanged.
