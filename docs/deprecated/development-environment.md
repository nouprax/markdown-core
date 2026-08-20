# Development environment

This is the single environment entry point for contributors and release
maintainers. Repository commands remain native to CMake, SwiftPM, Gradle, and
pnpm; the bootstrap script checks or installs prerequisites without replacing
those build systems.

```sh
scripts/init-environment.sh --check
scripts/init-environment.sh --install
```

`--check` is read-only and never downloads, installs, accepts licenses, or reads
credentials. `--install` is non-interactive and idempotent. It bootstraps only
repository-managed tools, JavaScript dependencies, declared Android SDK
packages, and the pinned Emscripten SDK. It may use Homebrew or `apt-get` for
missing basic build tools and JDK 26, but it never installs Xcode, Android
command line tools, Gradle, or Maven. Install those platform-owned
prerequisites first.

Both modes accept components, which is how CI checks only the tools prepared by
each job's official setup actions:

```sh
scripts/init-environment.sh --check core node java wrappers
scripts/init-environment.sh --check android android-emulator
scripts/init-environment.sh --check swift
scripts/init-environment.sh --check emscripten
```

## Required on every development host

| Dependency | Required version | Source and verification |
| --- | --- | --- |
| Git | current supported release | system package; `git --version` |
| C/C++ compiler | C11/C++17-capable Clang or GCC | Xcode Command Line Tools or system package; `cc --version` |
| CMake | 3.20 or later | system package; `cmake --version` |
| pkg-config | current supported release | system package; `pkg-config --version` |
| Node.js | 26.5.0 | `.node-version` and `package.json`; `node --version` |
| pnpm | 11.7.0 | `packageManager` in `package.json`; `pnpm --version` |
| JDK | 26 | host package plus Gradle daemon criteria; `scripts/init-environment.sh --check java` |
| zip/unzip | system version | release and consumer packaging |

Ninja is supported but optional; committed CMake presets do not require it.
Node and pnpm must already be the exact versions above before `--install`,
because the repository does not select or mutate a developer's version manager.

Gradle 9.6.1 and Maven 3.9.16 are repository-owned wrappers with committed
distribution checksums. Do not install global `gradle` or `mvn`; use
`scripts/gradle.sh`, `gradlew`, and `mvnw`.

## Platform-specific dependencies

### macOS, Swift, and Apple platforms

- Xcode 26.6 supplies Swift 6.3.3 and the iOS/macOS SDKs.
- Supported IDE validation uses Android Studio 2026.1.2 or IntelliJ IDEA 2026.1.
- The repository never installs Xcode. Select it with `xcode-select` before
  running `scripts/init-environment.sh --check swift`.
- SwiftPM supports iOS 18/26 and macOS 15/26 deployment validation. The iOS test
  tasks use an `iPhone 17 Pro` simulator on the latest installed runtime.

### Kotlin, Android, and JVM

Android Studio uses the committed JDK 26 daemon criteria for Gradle sync. The
Android SDK is located through `ANDROID_HOME`, `ANDROID_SDK_ROOT`, or the
standard macOS/Linux location. The fixed packages are:

- platform and target SDK 36, minimum API 21;
- Android CMake 3.22.1;
- NDK 28.2.13676358;
- Android Emulator;
- API 36 `google_apis` and `google_apis_ps16k` system images for the host ABI.

`scripts/init-environment.sh --install android android-emulator` uses the SDK's
own `sdkmanager`. It does not create an Android Studio AVD; Gradle Managed
Devices own the repository's 4 KB and 16 KB test devices.

### ECMAScript and WebAssembly

Emscripten 4.0.23 is required. `--install emscripten` clones the official emsdk
into `.tools/emsdk/4.0.23`, installs that release, and activates it locally.
Source its generated `emsdk_env.sh` when invoking `emcc` directly; repository
checks discover the pinned local installation automatically.

## Repository-managed and optional tools

`--install tools` installs ignored, reproducible copies under `.tools/`:

- clang-format 22.1.8;
- cmake-format 0.6.13 with PyYAML 6.0.3;
- SwiftLint 0.65.0 from a checksum-verified upstream archive.

Prettier 3.9.5, ESLint 10.0.1, TypeScript 6.0.3, and typescript-eslint 8.63.0
come from the frozen pnpm lockfile. `--install dependencies` runs only
`npx --yes pnpm@11.7.0 install --frozen-lockfile`, so the pinned package manager
runs on the selected Node.js 26.5.0 runtime even when a host has another pnpm
shim earlier in its tool cache.

## Clean bootstrap and validation

For the release-quality path, begin with a clean checkout and no ignored
outputs:

```sh
git clean -fdX
scripts/audit-repository.sh --physical
scripts/init-environment.sh --install
scripts/init-environment.sh --check
pnpm verify
pnpm check:kotlin-consumers
pnpm release:dry-run
```

The install step never reads Maven Central, npm, GitHub release, signing, or PGP
credentials. Publishing remains isolated to the protected `release`
environment and tag-driven release workflow described in
[`releasing.md`](releasing.md).
