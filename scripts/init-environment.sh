#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

NODE_VERSION=26.5.0
PNPM_VERSION=11.7.0
JAVA_VERSION=26
XCODE_VERSION=26.6
SWIFT_VERSION=6.3.3
EMSCRIPTEN_VERSION=4.0.23
CLANG_FORMAT_VERSION=22.1.8
CMAKE_FORMAT_VERSION=0.6.13
SWIFTLINT_VERSION=0.65.0
ANDROID_PLATFORM=android-36
ANDROID_CMAKE_VERSION=3.22.1
ANDROID_NDK_VERSION=28.2.13676358
GRADLE_VERSION=9.6.1
MAVEN_VERSION=3.9.16

usage() {
    cat <<'EOF'
Usage: scripts/init-environment.sh --check [component ...]
       scripts/init-environment.sh --install [component ...]

Components: core node java wrappers android android-emulator swift emscripten
            dependencies tools

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
        core | node | java | wrappers | android | android-emulator | swift | emscripten | dependencies | tools) ;;
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
        version_at_least "$actual" 3.20 || fail "CMake 3.20 or later is required; found $actual"
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
    grep -q '^distributionSha256Sum=' gradle/wrapper/gradle-wrapper.properties \
        || fail "Gradle Wrapper checksum is missing"
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
    "$directory/emsdk" install "$EMSCRIPTEN_VERSION"
    "$directory/emsdk" activate "$EMSCRIPTEN_VERSION"
}

install_tools() {
    require_command python3 || return
    clang_directory="$root/.tools/clang-format/$CLANG_FORMAT_VERSION"
    if [ ! -x "$clang_directory/venv/bin/clang-format" ]; then
        python3 -m venv "$clang_directory/venv"
        "$clang_directory/venv/bin/python" -m pip install --disable-pip-version-check --quiet \
            "clang-format==$CLANG_FORMAT_VERSION"
    fi
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
has_component dependencies "$@" && check_dependencies
has_component tools "$@" && check_tools

if [ "$failures" -gt 0 ]; then
    echo "$failures environment requirement(s) failed for: $components" >&2
    exit 1
fi
echo "Environment check passed: $components"
