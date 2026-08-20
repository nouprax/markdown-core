# Phase 16: legacy setup removal and public surface

> Phase 18 adds only runner-free root canonical AST contract data. It does not
> reopen a legacy setup or public production surface: the shared spec is attached
> only to conformance test targets and is excluded from product artifacts.

## Removed setup

Phase 16 removes the tracked root `spm/`, `swift/`, `wrappers/`, and legacy
`android/` Prefab AAR sources. The root `Package.swift` is the current
`swift-markdown-core` manifest, not the inherited SwiftPM wrapper: it exports
only the `MarkdownCore` product and keeps `MarkdownCoreC` as a non-product
implementation target. The only Android native payload is now the internal
`packages/kotlin-markdown-core/android-runtime/` module selected by the public
KMP package.

The three generated-style headers needed by non-CMake source builds now live
under `packages/markdown-core/core/include/`. SwiftPM, Kotlin Android JNI,
and ES/WASM builds include that private directory directly. No forwarding
header exposes the inherited mutable engine or extension APIs.

The obsolete `check_android_prefab_support.sh`, Gradle `:android` registration,
Gradle model expectation, formatter paths, ESLint wrapper override, package
audit branches, and README layout entry were removed or updated. A final
target-tree pass also removed inherited Travis/AppVeyor, NMake/MinGW, Docker,
manpage, XML-renderer, IDE, and obsolete Unicode-generator roots. The tracked
`samples/` index points to package-owned executable consumers without
reintroducing shared root test implementations.

## Frozen public surfaces

- C installs exactly `packages/markdown-core/include/markdown_core.h`. Its
  declarations and `exports/markdown_core.map` must match exactly. The shared
  facade uses a GNU version script or Darwin exported-symbol list so mutation,
  renderer, parser-internal, and extension-registration symbols do not escape.
  The engine is statically absorbed into that sole public shared library;
  there is no separately installed mutable engine DSO. Repository-owned CLI
  and legacy engine correctness tests link the internal static graph and do
  not widen the shared ABI.
- SwiftPM exports exactly the `MarkdownCore` product. Its immutable copied AST,
  parse options, parse errors, typed visitor, read-only walker, public
  `TreeDumper`, and `Markup.dump()` expose no C handle, renderer, mutation, or
  serialization API.
- Kotlin retains `explicitApi()` and exposes the same immutable common API,
  including `WalkEvent.ENTERING`/`EXITING`, `TreeDumper`, and `Markup.dump()`.
  JNI/native wire code and the Android runtime module remain implementation
  details. The public coordinate is `com.nouprax:kotlin-markdown-core`; the
  four-ABI runtime coordinate is internal.
- npm exports only the ESM/type root and the package-relative WASM asset.
  Runtime named exports are `Document`, `ParseError`, `TreeDumper`, `WalkEvent`,
  and `Walker`, plus `visit`; declarations expose immutable AST types and their
  non-enumerable `dump()` diagnostic method without memory, pointer,
  initialization, renderer, mutation, or serialization entry points.

`scripts/audit-public-surface.sh` enforces these source, manifest, coordinate,
and license allowlists. `scripts/audit-package-contents.sh` additionally builds
and installs C, compares actual facade DSO symbols with the C allowlist,
validates the npm tarball, and checks the Kotlin internal Android runtime AAR.

## Attribution

The repository keeps `LICENSE`, `COPYING`, and `UPSTREAM.md`. The npm package
ships an exact copy of `LICENSE`, which both public-surface and package-content
audits verify.

## Validation

Validated on 2026-07-12:

- SwiftPM manifest audit and `swift build` using the private C include path;
- C shared/static configure and build plus all 59 correctness tests;
- source/manifest public-surface audit;
- package-owned Swift, Kotlin, and ES suites and aggregate audits listed in the
  Phase 16 handoff validation.

## Post-phase audit handoff

A full repository audit after this handoff found that the intended single
public C library boundary was not yet consistently represented by installed
pkg-config/CMake metadata, and also identified CI, attribution, toolchain-doc,
Gradle-deprecation, formatter, stale-path, and physical-worktree cleanup gaps.
Those findings do not reopen the public-surface decision: consumers must see
only `libmarkdown-core`, never a separately installed or exported extensions
library. Repository-owned remediation was completed in Phase 17; Phase 18 now
owns shared canonical AST conformance before quality gates in Phase 19 and
release support in Phase 20. Final Git/physical checkout closure is owned by
Phase 21 rather than blocking either workflow phase; see
`2026-07-12-phase-17-repo-audit-remediation.md`.
