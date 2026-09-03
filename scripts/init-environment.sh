#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

NODE_VERSION=26.8.1
PNPM_VERSION=11.25.0
JAVA_VERSION=26
XCODE_VERSION=26.6
SWIFT_VERSION=6.3.3
EMSCRIPTEN_VERSION=6.0.9
# The emsdk repository commit of the 6.0.9 release tag; keep the pair in
# lockstep when bumping the version.
EMSCRIPTEN_COMMIT=5eb0bde7585670252e8ba05e9d361627bffd08b5
# The newest tag upstream cmark-gfm has published, with its immutable commit.
# This pin defines what "parity with upstream" means, so moving it is a
# reviewed change, not a routine bump (see specs/oracles/cmark-gfm/deltas.json).
CMARK_GFM_VERSION=0.29.0.gfm.13
CMARK_GFM_COMMIT=587a12bb54d95ac37241377e6ddc93ea0e45439b
# The newest stable CommonMark reference implementation. CommonMark parity is
# intentionally judged by cmark itself, not by GitHub's dormant fork.
CMARK_VERSION=0.31.2
CMARK_COMMIT=eec0eeba6d31189fd828314576494566d539b1e3
CLANG_FORMAT_VERSION=23.1.0
CMAKE_FORMAT_VERSION=0.6.13
SWIFTLINT_VERSION=0.65.1
ANDROID_PLATFORM=android-36
ANDROID_CMAKE_VERSION=3.22.1
ANDROID_NDK_VERSION=28.2.13676358
GRADLE_VERSION=9.7.1
GRADLE_DISTRIBUTION_SHA256=acd53f1edaf02f1a8ff99879f8a34b302661a057d9b063ae9e35b552f804d20a
GRADLE_WRAPPER_JAR_SHA256=7a9ce74cff467ca1bf60a4fcd9f05185acceda4d0f382434d393e17864262c5d
GRADLE_SIGNING_KEY_FINGERPRINT=F3FF33E96F18AA62DD580F9651FBF517CE6D6B80
MAVEN_VERSION=3.9.16

usage() {
    cat <<'EOF'
Usage: scripts/init-environment.sh --check [component ...]
       scripts/init-environment.sh --install [component ...]

Components: core node java wrappers android android-emulator swift emscripten
            oracle-cmark oracle-cmark-gfm dependencies tools

With no components, the command checks or installs the complete environment
supported by the current host. --check never installs or downloads anything.
--install bootstraps repository-managed tools, JavaScript dependencies,
Android SDK packages, and Emscripten; it never installs Xcode or reads release
credentials.
EOF
}

mode=${1:-}
case "$mode" in
    --check | --install) shift ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if [ "$#" -eq 0 ]; then
    set -- core node java wrappers android android-emulator emscripten dependencies tools
    if [ "$(uname -s)" = Darwin ]; then
        set -- "$@" swift
    fi
fi

for component do
    case "$component" in
        core | node | java | wrappers | android | android-emulator | swift | emscripten | oracle-cmark | oracle-cmark-gfm | dependencies | tools) ;;
        *)
            echo "Unknown environment component: $component" >&2
            usage >&2
            exit 2
            ;;
    esac
done

failures=0
fail() {
    echo "environment check failed: $1" >&2
    failures=$((failures + 1))
}
ok() {
    echo "ok: $1"
}
has_component() {
    wanted=$1
    shift
    for component do
        [ "$component" = "$wanted" ] && return 0
    done
    return 1
}
require_command() {
    command -v "$1" >/dev/null 2>&1 || {
        fail "$1 is not available"
        return 1
    }
}
version_at_least() {
    awk -v actual="$1" -v minimum="$2" 'BEGIN {
        split(actual, a, "."); split(minimum, b, ".");
        for (i = 1; i <= 3; i++) {
            a[i] += 0; b[i] += 0;
            if (a[i] > b[i]) exit 0;
            if (a[i] < b[i]) exit 1;
        }
        exit 0;
    }'
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        shasum -a 256 "$1" | awk '{ print $1 }'
    fi
}

java_home() {
    if [ -n "${JAVA_HOME:-}" ] && java_home_has_version "$JAVA_HOME" "$JAVA_VERSION"; then
        printf '%s\n' "$JAVA_HOME"
        return
    fi
    if [ "$(uname -s)" = Darwin ] && /usr/libexec/java_home -v "$JAVA_VERSION" >/dev/null 2>&1; then
        candidate=$(/usr/libexec/java_home -v "$JAVA_VERSION")
        if java_home_has_version "$candidate" "$JAVA_VERSION"; then
            printf '%s\n' "$candidate"
            return
        fi
    fi
    candidate="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home"
    if java_home_has_version "$candidate" "$JAVA_VERSION"; then
        printf '%s\n' "$candidate"
        return
    fi
    if [ -d "$HOME/.gradle/jdks" ]; then
        candidate=$(find "$HOME/.gradle/jdks" -type f -path '*/bin/java' -print 2>/dev/null \
            | while IFS= read -r java; do
                home=$(dirname "$(dirname "$java")")
                java_home_has_version "$home" "$JAVA_VERSION" && printf '%s\n' "$home"
            done \
            | head -n 1)
        if [ -n "$candidate" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    fi
    candidate="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
    if java_home_has_version "$candidate" "$JAVA_VERSION"; then
        printf '%s\n' "$candidate"
        return
    fi
    if command -v java >/dev/null 2>&1; then
        candidate=$(dirname "$(dirname "$(command -v java)")")
        if java_home_has_version "$candidate" "$JAVA_VERSION"; then
            printf '%s\n' "$candidate"
        fi
    fi
    return 0
}

java_home_has_version() {
    home=$1
    expected=$2
    [ -x "$home/bin/java" ] || return 1
    actual=$("$home/bin/java" -version 2>&1 | sed -n '1s/.*version "\([0-9][0-9]*\).*/\1/p')
    [ "$actual" = "$expected" ]
}

android_home() {
    if [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME" ]; then
        printf '%s\n' "$ANDROID_HOME"
    elif [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "$ANDROID_SDK_ROOT" ]; then
        printf '%s\n' "$ANDROID_SDK_ROOT"
    elif [ -d "$HOME/Library/Android/sdk" ]; then
        printf '%s\n' "$HOME/Library/Android/sdk"
    elif [ -d "$HOME/Android/Sdk" ]; then
        printf '%s\n' "$HOME/Android/Sdk"
    fi
}

sdkmanager_path() {
    sdk=$1
    for candidate in \
        "$sdk/cmdline-tools/latest/bin/sdkmanager" \
        "$sdk/cmdline-tools/bin/sdkmanager" \
        "$sdk/tools/bin/sdkmanager"; do
        [ -x "$candidate" ] && {
            printf '%s\n' "$candidate"
            return 0
        }
    done
    command -v sdkmanager 2>/dev/null || true
}

emcc_path() {
    if [ -n "${EMSDK:-}" ] && [ -x "$EMSDK/upstream/emscripten/emcc" ]; then
        printf '%s\n' "$EMSDK/upstream/emscripten/emcc"
    elif [ -x "$root/.tools/emsdk/$EMSCRIPTEN_VERSION/upstream/emscripten/emcc" ]; then
        printf '%s\n' "$root/.tools/emsdk/$EMSCRIPTEN_VERSION/upstream/emscripten/emcc"
    else
        command -v emcc 2>/dev/null || true
    fi
}

check_core() {
    before=$failures
    require_command git || true
    require_command cc || true
    require_command cmake || true
    require_command pkg-config || true
    require_command zip || true
    require_command unzip || true
    if command -v cmake >/dev/null 2>&1; then
        actual=$(cmake --version | sed -n '1s/.* //p')
        # The floor matches cmakeMinimumRequired in CMakePresets.json: every
        # documented entry point configures through `cmake --preset`.
        version_at_least "$actual" 3.21 || fail "CMake 3.21 or later is required; found $actual"
    fi
    [ "$failures" -ne "$before" ] || ok "C/C++ build tools"
    return 0
}

check_node() {
    require_command node || return 0
    require_command pnpm || return 0
    require_command npx || return 0
    actual_node=$(node --version | sed 's/^v//')
    actual_pnpm=$(pnpm --version)
    [ "$actual_node" = "$NODE_VERSION" ] || fail "Node.js $NODE_VERSION is required; found $actual_node"
    [ "$actual_pnpm" = "$PNPM_VERSION" ] || fail "pnpm $PNPM_VERSION is required; found $actual_pnpm"
    [ "$actual_node" != "$NODE_VERSION" ] || [ "$actual_pnpm" != "$PNPM_VERSION" ] || ok "Node.js and pnpm"
    return 0
}

check_java() {
    home=$(java_home)
    if [ -z "$home" ]; then
        fail "JDK $JAVA_VERSION is not available"
        return
    fi
    actual=$("$home/bin/java" -version 2>&1 | sed -n '1s/.*version "\([0-9][0-9]*\).*/\1/p')
    [ "$actual" = "$JAVA_VERSION" ] || fail "JDK $JAVA_VERSION is required; found ${actual:-unknown} at $home"
    [ "$actual" != "$JAVA_VERSION" ] || ok "JDK $JAVA_VERSION ($home)"
    return 0
}

check_wrappers() {
    before=$failures
    grep -Fq "gradle-$GRADLE_VERSION-bin.zip" gradle/wrapper/gradle-wrapper.properties \
        || fail "Gradle Wrapper does not select $GRADLE_VERSION"
    grep -Fq "apache-maven-$MAVEN_VERSION-bin.zip" .mvn/wrapper/maven-wrapper.properties \
        || fail "Maven Wrapper does not select $MAVEN_VERSION"
    grep -Fqx "distributionSha256Sum=$GRADLE_DISTRIBUTION_SHA256" \
        gradle/wrapper/gradle-wrapper.properties \
        || fail "Gradle Wrapper checksum does not match the reviewed $GRADLE_VERSION distribution"
    [ "$(sha256_file gradle/wrapper/gradle-wrapper.jar)" = "$GRADLE_WRAPPER_JAR_SHA256" ] \
        || fail "Gradle Wrapper JAR does not match the reviewed $GRADLE_VERSION artifact"
    grep -Fq \
        "<trusted-key id=\"$GRADLE_SIGNING_KEY_FINGERPRINT\" group=\"gradle\" name=\"gradle\" version=\"$GRADLE_VERSION\"/>" \
        gradle/verification-metadata.xml \
        || fail "Gradle $GRADLE_VERSION signing key is not trusted at exact module scope"
    grep -q '^distributionSha256Sum=' .mvn/wrapper/maven-wrapper.properties \
        || fail "Maven Wrapper checksum is missing"
    [ "$failures" -ne "$before" ] || ok "Gradle and Maven wrappers"
    return 0
}

check_android() {
    before=$failures
    sdk=$(android_home)
    if [ -z "$sdk" ]; then
        fail "Android SDK is not available"
        return
    fi
    for relative in \
        "platforms/$ANDROID_PLATFORM" \
        "cmake/$ANDROID_CMAKE_VERSION" \
        "ndk/$ANDROID_NDK_VERSION"; do
        [ -d "$sdk/$relative" ] || fail "Android SDK package is missing: $relative"
    done
    [ "$failures" -ne "$before" ] || ok "Android SDK build packages ($sdk)"
    return 0
}

check_android_emulator() {
    before=$failures
    sdk=$(android_home)
    if [ -z "$sdk" ]; then
        fail "Android SDK is not available"
        return
    fi
    [ -x "$sdk/emulator/emulator" ] || fail "Android Emulator is missing"
    case "$(uname -m)" in
        arm64 | aarch64) abi=arm64-v8a ;;
        *) abi=x86_64 ;;
    esac
    for image in google_apis google_apis_ps16k; do
        [ -d "$sdk/system-images/$ANDROID_PLATFORM/$image/$abi" ] \
            || fail "Android system image is missing: $ANDROID_PLATFORM/$image/$abi"
    done
    [ "$failures" -ne "$before" ] || ok "Android Emulator images ($abi)"
    return 0
}

check_swift() {
    if [ "$(uname -s)" != Darwin ]; then
        fail "Swift/Xcode checks are supported only on macOS"
        return
    fi
    require_command xcodebuild || return 0
    require_command swift || return 0
    xcode=$(xcodebuild -version | sed -n '1s/^Xcode //p')
    swift_version=$(swift --version 2>&1 | sed -n 's/.*Swift version \([0-9][0-9.]*\).*/\1/p' | head -n 1)
    [ "$xcode" = "$XCODE_VERSION" ] || fail "Xcode $XCODE_VERSION is required; found ${xcode:-unknown}"
    [ "$swift_version" = "$SWIFT_VERSION" ] || fail "Swift $SWIFT_VERSION is required; found ${swift_version:-unknown}"
    [ "$xcode" != "$XCODE_VERSION" ] || [ "$swift_version" != "$SWIFT_VERSION" ] || ok "Xcode and Swift"
    return 0
}

check_emscripten() {
    emcc=$(emcc_path)
    if [ -z "$emcc" ]; then
        fail "Emscripten $EMSCRIPTEN_VERSION is not available"
        return
    fi
    actual=$("$emcc" --version | sed -n '1s/.* \([0-9][0-9.]*\).*/\1/p')
    [ "$actual" = "$EMSCRIPTEN_VERSION" ] || fail "Emscripten $EMSCRIPTEN_VERSION is required; found ${actual:-unknown}"
    [ "$actual" != "$EMSCRIPTEN_VERSION" ] || ok "Emscripten $EMSCRIPTEN_VERSION"
    return 0
}

cmark_path() {
    printf '%s\n' "$root/.tools/cmark/$CMARK_VERSION/build/src/cmark"
}

cmark_gfm_path() {
    printf '%s\n' "$root/.tools/cmark-gfm/$CMARK_GFM_VERSION/build/src/cmark-gfm"
}

check_oracle_cmark() {
    binary=$(cmark_path)
    if [ ! -x "$binary" ]; then
        fail "cmark oracle $CMARK_VERSION is not built"
        return
    fi
    actual=$(git -C "$root/.tools/cmark/$CMARK_VERSION" rev-parse HEAD 2>/dev/null || true)
    [ "$actual" = "$CMARK_COMMIT" ] || fail "cmark oracle is ${actual:-missing}, expected $CMARK_COMMIT"
    [ "$actual" != "$CMARK_COMMIT" ] || ok "cmark oracle $CMARK_VERSION"
    return 0
}

check_oracle_cmark_gfm() {
    binary=$(cmark_gfm_path)
    if [ ! -x "$binary" ]; then
        fail "cmark-gfm oracle $CMARK_GFM_VERSION is not built"
        return
    fi
    actual=$(git -C "$root/.tools/cmark-gfm/$CMARK_GFM_VERSION" rev-parse HEAD 2>/dev/null || true)
    [ "$actual" = "$CMARK_GFM_COMMIT" ] || fail "cmark-gfm oracle is ${actual:-missing}, expected $CMARK_GFM_COMMIT"
    [ "$actual" != "$CMARK_GFM_COMMIT" ] || ok "cmark-gfm oracle $CMARK_GFM_VERSION"
    return 0
}

check_dependencies() {
    if [ -f node_modules/.modules.yaml ]; then
        ok "frozen JavaScript dependency install"
    else
        fail "JavaScript dependencies are not installed"
    fi
    return 0
}

check_tools() {
    before=$failures
    clang_format=${CLANG_FORMAT:-}
    if [ -z "$clang_format" ] && [ -x "$root/.tools/clang-format/$CLANG_FORMAT_VERSION/venv/bin/clang-format" ]; then
        clang_format="$root/.tools/clang-format/$CLANG_FORMAT_VERSION/venv/bin/clang-format"
    fi
    if [ -z "$clang_format" ]; then
        clang_format=$(command -v clang-format 2>/dev/null || true)
    fi
    if [ -z "$clang_format" ]; then
        fail "clang-format $CLANG_FORMAT_VERSION is not available"
    else
        actual=$("$clang_format" --version | sed -E 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
        [ "$actual" = "$CLANG_FORMAT_VERSION" ] || fail "clang-format $CLANG_FORMAT_VERSION is required; found $actual"
    fi
    [ -x "$root/.tools/cmakelang/$CMAKE_FORMAT_VERSION/venv/bin/cmake-format" ] \
        || fail "repo-managed cmake-format $CMAKE_FORMAT_VERSION is not installed"
    [ -x "$root/.tools/swiftlint/$SWIFTLINT_VERSION/swiftlint" ] \
        || fail "repo-managed SwiftLint $SWIFTLINT_VERSION is not installed"
    [ "$failures" -ne "$before" ] || ok "repository-managed formatter and lint tools"
    return 0
}

install_core() {
    missing=
    for command in git cc cmake pkg-config zip unzip; do
        command -v "$command" >/dev/null 2>&1 || missing="$missing $command"
    done
    [ -z "$missing" ] && return
    case "$(uname -s)" in
        Darwin)
            require_command brew || return
            NONINTERACTIVE=1 brew install cmake pkg-config
            ;;
        Linux)
            if command -v sudo >/dev/null 2>&1; then
                sudo apt-get update
                sudo env DEBIAN_FRONTEND=noninteractive \
                    apt-get install --yes build-essential cmake pkg-config zip unzip git
            else
                fail "missing system tools:$missing (sudo is unavailable)"
            fi
            ;;
        *) fail "automatic system-tool install is unsupported on $(uname -s)" ;;
    esac
}

install_android() {
    sdk=$(android_home)
    [ -n "$sdk" ] || {
        fail "install Android Studio command-line tools first"
        return
    }
    if [ -d "$sdk/platforms/$ANDROID_PLATFORM" ] \
        && [ -d "$sdk/cmake/$ANDROID_CMAKE_VERSION" ] \
        && [ -d "$sdk/ndk/$ANDROID_NDK_VERSION" ]; then
        return
    fi
    manager=$(sdkmanager_path "$sdk")
    [ -n "$manager" ] || {
        fail "sdkmanager is not available under $sdk"
        return
    }
    yes | "$manager" --licenses >/dev/null 2>&1 || true
    "$manager" "platforms;$ANDROID_PLATFORM" "cmake;$ANDROID_CMAKE_VERSION" "ndk;$ANDROID_NDK_VERSION"
}

install_java() {
    if [ -n "$(java_home)" ]; then
        return
    fi
    case "$(uname -s)" in
        Darwin)
            require_command brew || return
            NONINTERACTIVE=1 brew install openjdk
            ;;
        Linux)
            if command -v sudo >/dev/null 2>&1; then
                sudo apt-get update
                sudo env DEBIAN_FRONTEND=noninteractive apt-get install --yes openjdk-26-jdk
            else
                fail "JDK $JAVA_VERSION is missing and sudo is unavailable"
            fi
            ;;
        *) fail "automatic JDK install is unsupported on $(uname -s)" ;;
    esac
}

install_android_emulator() {
    sdk=$(android_home)
    [ -n "$sdk" ] || {
        fail "install Android Studio command-line tools first"
        return
    }
    case "$(uname -m)" in
        arm64 | aarch64) abi=arm64-v8a ;;
        *) abi=x86_64 ;;
    esac
    if [ -x "$sdk/emulator/emulator" ] \
        && [ -d "$sdk/system-images/$ANDROID_PLATFORM/google_apis/$abi" ] \
        && [ -d "$sdk/system-images/$ANDROID_PLATFORM/google_apis_ps16k/$abi" ]; then
        return
    fi
    manager=$(sdkmanager_path "$sdk")
    [ -n "$manager" ] || {
        fail "sdkmanager is not available"
        return
    }
    "$manager" \
        emulator \
        "system-images;$ANDROID_PLATFORM;google_apis;$abi" \
        "system-images;$ANDROID_PLATFORM;google_apis_ps16k;$abi"
}

install_emscripten() {
    directory="$root/.tools/emsdk/$EMSCRIPTEN_VERSION"
    if [ ! -d "$directory/.git" ]; then
        mkdir -p "$(dirname "$directory")"
        git clone --filter=blob:none https://github.com/emscripten-core/emsdk.git "$directory"
    fi
    # Pin the emsdk manager itself to the immutable commit of the release
    # tag matching EMSCRIPTEN_VERSION: a moved tag or new default-branch
    # commit must never change the bytes this installer executes. The
    # fetch runs only when the commit is absent so a re-run stays
    # idempotent without network access.
    git -C "$directory" rev-parse --quiet --verify "$EMSCRIPTEN_COMMIT^{commit}" >/dev/null \
        || git -C "$directory" fetch --filter=blob:none origin "$EMSCRIPTEN_COMMIT"
    git -C "$directory" checkout --quiet "$EMSCRIPTEN_COMMIT"
    actual_commit=$(git -C "$directory" rev-parse HEAD)
    [ "$actual_commit" = "$EMSCRIPTEN_COMMIT" ] || {
        fail "emsdk checkout is $actual_commit, expected $EMSCRIPTEN_COMMIT"
        return
    }
    "$directory/emsdk" install "$EMSCRIPTEN_VERSION"
    "$directory/emsdk" activate "$EMSCRIPTEN_VERSION"
}

# The two upstream parsers have disjoint authority: cmark for CommonMark and
# cmark-gfm for its extension layer. Both pins are immutable because a moved
# tag must never change what the parity gates compare against.
install_oracle_cmark() {
    directory="$root/.tools/cmark/$CMARK_VERSION"
    if [ ! -d "$directory/.git" ]; then
        mkdir -p "$(dirname "$directory")"
        git clone --filter=blob:none https://github.com/commonmark/cmark.git "$directory"
    fi
    git -C "$directory" rev-parse --quiet --verify "$CMARK_COMMIT^{commit}" >/dev/null \
        || git -C "$directory" fetch --filter=blob:none origin "$CMARK_COMMIT"
    git -C "$directory" checkout --quiet "$CMARK_COMMIT"
    actual_commit=$(git -C "$directory" rev-parse HEAD)
    [ "$actual_commit" = "$CMARK_COMMIT" ] || {
        fail "cmark checkout is $actual_commit, expected $CMARK_COMMIT"
        return
    }
    cmake -S "$directory" -B "$directory/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=OFF >/dev/null
    cmake --build "$directory/build" --parallel >/dev/null
    [ -x "$(cmark_path)" ] || fail "cmark build produced no binary"
}

install_oracle_cmark_gfm() {
    directory="$root/.tools/cmark-gfm/$CMARK_GFM_VERSION"
    if [ ! -d "$directory/.git" ]; then
        mkdir -p "$(dirname "$directory")"
        git clone --filter=blob:none https://github.com/github/cmark-gfm.git "$directory"
    fi
    git -C "$directory" rev-parse --quiet --verify "$CMARK_GFM_COMMIT^{commit}" >/dev/null \
        || git -C "$directory" fetch --filter=blob:none origin "$CMARK_GFM_COMMIT"
    git -C "$directory" checkout --quiet "$CMARK_GFM_COMMIT"
    actual_commit=$(git -C "$directory" rev-parse HEAD)
    [ "$actual_commit" = "$CMARK_GFM_COMMIT" ] || {
        fail "cmark-gfm checkout is $actual_commit, expected $CMARK_GFM_COMMIT"
        return
    }
    # Upstream's CMakeLists still declares a pre-3.5 minimum, which current
    # CMake refuses outright; the policy override is what lets a dormant
    # project keep building without patching its tree.
    cmake -S "$directory" -B "$directory/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMARK_TESTS=OFF \
        -DCMARK_SHARED=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
    cmake --build "$directory/build" --parallel >/dev/null
    [ -x "$(cmark_gfm_path)" ] || fail "cmark-gfm build produced no binary"
}

install_tools() {
    require_command python3 || return
    CLANG_FORMAT_INSTALL_DIR="$root/.tools/clang-format/$CLANG_FORMAT_VERSION" \
        scripts/install-clang-format.sh
    CMAKE_FORMAT_TOOL_DIR="$root/.tools/cmakelang/$CMAKE_FORMAT_VERSION" \
        scripts/format-cmake.sh --check
    scripts/install-swiftlint.sh
}

components="$*"
if [ "$mode" = --install ]; then
    has_component core "$@" && install_core
    has_component node "$@" && check_node
    has_component java "$@" && install_java
    has_component wrappers "$@" && check_wrappers
    has_component android "$@" && install_android
    has_component android-emulator "$@" && install_android_emulator
    has_component swift "$@" && check_swift
    has_component emscripten "$@" && install_emscripten
    has_component oracle-cmark "$@" && install_oracle_cmark
    has_component oracle-cmark-gfm "$@" && install_oracle_cmark_gfm
    has_component dependencies "$@" \
        && npx --yes "pnpm@$PNPM_VERSION" install --frozen-lockfile
    has_component tools "$@" && install_tools
    [ "$failures" -eq 0 ] || exit 1
fi

failures=0
has_component core "$@" && check_core
has_component node "$@" && check_node
has_component java "$@" && check_java
has_component wrappers "$@" && check_wrappers
has_component android "$@" && check_android
has_component android-emulator "$@" && check_android_emulator
has_component swift "$@" && check_swift
has_component emscripten "$@" && check_emscripten
has_component oracle-cmark "$@" && check_oracle_cmark
has_component oracle-cmark-gfm "$@" && check_oracle_cmark_gfm
has_component dependencies "$@" && check_dependencies
has_component tools "$@" && check_tools

if [ "$failures" -gt 0 ]; then
    echo "$failures environment requirement(s) failed for: $components" >&2
    exit 1
fi
echo "Environment check passed: $components"
