# Supported toolchains

This is the normative toolchain contract for Markdown Core. The exact pins live
in `scripts/init-environment.sh`; `scripts/audit-toolchain-versions.sh` checks
that workflows, wrappers, manifests, consumers, and the cloud image agree with
that source of truth. The versions below were reviewed on 2026-09-01.

## Version contract

| Area | Supported version | Contract |
| --- | --- | --- |
| Gradle | 9.6.1 | Wrapper distribution and SHA-256 are committed |
| AGP | 9.3.2 | Stable patch line; compile/target SDK 36, min SDK 21 |
| Kotlin Gradle plugin | 2.4.10 | Stable bug-fix release |
| Kotlin source/API and stdlib | 2.2 / 2.2.21 | Deliberate consumer compatibility floor; independent of the build plugin |
| Gradle daemon JDK / JVM bytecode | 26 / 17 | JDK 26 runs the build; published JVM code remains Java 17 compatible |
| Android NDK / CMake | 28.2.13676358 / 3.22.1 | AGP-tested NDK and reproducible Android native build tool |
| Xcode / Swift | 26.6 / 6.3.3 | Xcode is selected exactly in Apple CI jobs |
| Swift tools / deployment | 6.3 / iOS 26, macOS 26 | Applies to root, release, and consumer manifests |
| Node.js / pnpm | 26.8.1 / 11.25.0 | Exact local, CI, and cloud-image versions |
| Emscripten | 6.0.9 | emsdk release tag and immutable emsdk commit are paired |
| Maven | 3.9.16 | Maven 3 GA line; Maven 4 remains prerelease |
| clang-format / cmake-format | 23.1.0 / 0.6.13 | Repository-managed, hash-locked formatting tools |
| SwiftLint | 0.65.1 | Release assets are selected per host and verified by SHA-256 |

Android API 37 is still a preview SDK, so the production contract remains API
36. Preview SDKs and release candidates are not adopted merely because they
have a higher version number. The Android CMake pin is intentionally the
reproducible SDK package used by the JNI build; the host CMake requirement
remains the lower compatibility floor declared by the CMake project.

Kotlin 2.4.10's published compatibility table does not yet classify Gradle
9.6.1 and AGP 9.3.2 as fully supported. This repository treats that as an
explicit compatibility edge: Gradle model checks, Android/KMP/JVM consumers,
dependency verification, publications, and package audits must all pass. A
future version bump must not weaken those gates to suppress an incompatibility.

## Maintenance policy

Use stable releases. A version change must update every declaration, dependency
lock, and verification checksum in the same review. Do not change the Kotlin
stdlib compatibility floor as a side effect of updating the Kotlin build
plugin, and do not advance an Apple deployment target without updating all
three Swift manifests and the deployment-contract matrix.

Check a prepared machine without downloading anything:

```sh
scripts/init-environment.sh --check
pnpm audit:toolchains
```

When Gradle plugins change, regenerate locks and dependency-verification
metadata through dependency resolution, then review every added component,
checksum, trusted key, and ignored key. Never disable dependency verification
to make an upgrade pass.

Upstream references: [Gradle releases](https://docs.gradle.org/current/release-notes.html),
[AGP releases](https://developer.android.com/build/releases/gradle-plugin),
[Kotlin/Gradle compatibility](https://kotlinlang.org/docs/gradle-configure-project.html),
[Xcode releases](https://developer.apple.com/documentation/xcode-release-notes),
and [Android SDK setup](https://developer.android.com/about/versions/17/setup-sdk).
