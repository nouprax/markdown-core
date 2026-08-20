# Phase 1 Structure and Tooling Validation

This document records the completion evidence for the repository-only
restructure and tooling work in Phase 1. It does not introduce parser behavior
changes.

## Implementation history

Phase 1 was intentionally split into reviewable commits:

- `d988a23`: Git-aware source, extension, test, benchmark, fuzz, and tooling
  moves into the monorepo package boundaries.
- `7881c82`: root orchestration, pinned format/lint tools, Gradle wrapper and
  verification metadata, package allowlists, CI checks, and toolchain records.
- `1aa7ba8`: standalone clang-format migration of inherited handwritten C/C++
  sources to four-space style.
- `7347e56`: checklist completion marker after validation.

Fixture moves retained byte-identical contents. The only non-formatting C
changes in the tooling commit were compiler-hygiene changes such as explicit
`void` parameter lists and signed/unsigned comparison casts; parser behavior
was not intentionally changed.

## Completion audit corrections

A retrospective audit found two gaps and corrected both:

1. `scripts/gradle-model-smoke.sh` previously ran Gradle CLI discovery tasks
   but did not request a model through the Gradle Tooling API. It now compiles a
   small isolated Tooling API consumer, loads `GradleProject`, verifies the
   `markdown-core` root, and verifies the `:android` child project. It uses the
   pinned Gradle 9.6.1 distribution and standard local/CI JDK and Android SDK
   discovery.
2. `man/CMakeLists.txt` was excluded from cmake-format despite being
   handwritten. The exclusion was removed, the file was formatted, and
   `format:cmake:check` now covers it.

The model check produces:

```text
Loaded Gradle Tooling API model for markdown-core
```

It configures the project without release credentials, signing material,
prebuilt native libraries, `.idea`, or `.iml` state.

## Validation

The corrected Phase 1 infrastructure is covered by:

```sh
pnpm run format:check
pnpm run lint
pnpm run test:gradle-model
pnpm run audit:packages
pnpm run verify
```

The full verification passed on macOS arm64. It included C warnings-as-errors,
Swift formatting/lint/build, Kotlin formatting, ESLint/Prettier, a real Gradle
Tooling API model load, C/Swift tests, Gradle model checks, and package-content
audits.

Node.js remains pinned to `26.5.0` in `.node-version`, `package.json`, CI, and
`docs/toolchains.md`; pnpm remains pinned to `11.7.0`.
