#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

fail() {
    echo "toolchain version audit failed: $1" >&2
    exit 1
}

shell_value() {
    awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); exit }' \
        scripts/init-environment.sh
}

toml_value() {
    awk -F' = ' -v key="$1" '$1 == key { gsub(/"/, "", $2); print $2; exit }' \
        gradle/libs.versions.toml
}

expect_equal() {
    label=$1
    expected=$2
    actual=$3
    [ "$actual" = "$expected" ] || fail "$label is '$actual'; expected '$expected'"
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        shasum -a 256 "$1" | awk '{ print $1 }'
    fi
}

workflow_values() {
    action=$1
    setting=$2
    shift 2
    awk -v action="$action" -v setting="$setting" '
        index($0, "uses: " action "@") { waiting = 1; next }
        waiting && index($0, setting ":") {
            value = $0
            sub(".*" setting ":[[:space:]]*", "", value)
            gsub(/\"/, "", value)
            if (value == "|") {
                while ((getline continuation) > 0) {
                    sub(/^[[:space:]]*/, "", continuation)
                    if (continuation !~ /^[0-9.]+$/) break
                    print continuation
                }
            } else {
                print value
            }
            waiting = 0
        }
        waiting && /uses:/ { waiting = 0 }
    ' "$@"
}

expect_workflow_value() {
    label=$1
    expected=$2
    action=$3
    setting=$4
    shift 4
    values=$(workflow_values "$action" "$setting" "$@" | sort -u)
    [ -n "$values" ] || fail "no $label declaration was found"
    expect_equal "$label workflow declarations" "$expected" "$values"
}

node_version=$(shell_value NODE_VERSION)
pnpm_version=$(shell_value PNPM_VERSION)
java_version=$(shell_value JAVA_VERSION)
xcode_version=$(shell_value XCODE_VERSION)
swift_version=$(shell_value SWIFT_VERSION)
emscripten_version=$(shell_value EMSCRIPTEN_VERSION)
swiftlint_version=$(shell_value SWIFTLINT_VERSION)
clang_format_version=$(shell_value CLANG_FORMAT_VERSION)
android_platform=$(shell_value ANDROID_PLATFORM)
android_cmake_version=$(shell_value ANDROID_CMAKE_VERSION)
android_ndk_version=$(shell_value ANDROID_NDK_VERSION)
gradle_version=$(shell_value GRADLE_VERSION)
gradle_distribution_sha256=$(shell_value GRADLE_DISTRIBUTION_SHA256)
gradle_wrapper_jar_sha256=$(shell_value GRADLE_WRAPPER_JAR_SHA256)
gradle_signing_key_fingerprint=$(shell_value GRADLE_SIGNING_KEY_FINGERPRINT)

expect_equal .node-version "$node_version" "$(tr -d '[:space:]' < .node-version)"
package_versions=$(node -e '
    const value = require("./package.json");
    console.log(value.engines.node);
    console.log(value.engines.pnpm);
    console.log(value.packageManager);
')
expect_equal "package.json Node engine" "$node_version" "$(printf '%s\n' "$package_versions" | sed -n '1p')"
expect_equal "package.json pnpm engine" "$pnpm_version" "$(printf '%s\n' "$package_versions" | sed -n '2p')"
expect_equal "package.json packageManager" "pnpm@$pnpm_version" "$(printf '%s\n' "$package_versions" | sed -n '3p')"

expect_equal "cloud image Node" "$node_version" \
    "$(sed -n 's/^ARG NODE_VERSION=//p' .cursor/Dockerfile)"
expect_equal "cloud image pnpm" "$pnpm_version" \
    "$(sed -n 's/^ARG PNPM_VERSION=//p' .cursor/Dockerfile)"
grep -Fq "/emsdk/$emscripten_version/upstream/emscripten" .cursor/Dockerfile || \
    fail "cloud image Emscripten path does not select $emscripten_version"

workflow_files=(
    .github/workflows/ci.yml
    .github/workflows/release.yml
    .github/workflows/release-dry-run.yml
)
expect_workflow_value Node "$node_version" actions/setup-node node-version "${workflow_files[@]}"
expect_workflow_value pnpm "$pnpm_version" pnpm/action-setup version "${workflow_files[@]}"
java_workflow_versions=$(workflow_values actions/setup-java java-version "${workflow_files[@]}" | sort -u)
expect_equal "Java workflow declarations" "17
$java_version" "$java_workflow_versions"
expect_workflow_value Xcode "$xcode_version" maxim-lobanov/setup-xcode xcode-version "${workflow_files[@]}"
expect_workflow_value Emscripten "$emscripten_version" emscripten-core/setup-emsdk version "${workflow_files[@]}"

android_packages="sdkmanager \"platforms;$android_platform\" \"cmake;$android_cmake_version\" \"ndk;$android_ndk_version\""
while IFS= read -r declaration; do
    case "$declaration" in
        *"$android_packages"*) ;;
        *) fail "workflow Android SDK declaration is out of sync: $declaration" ;;
    esac
done < <(grep -h 'sdkmanager "platforms;' "${workflow_files[@]}")

expect_equal "Gradle Wrapper" "$gradle_version" \
    "$(sed -n 's#.*gradle-\([0-9][0-9.]*\)-bin\.zip#\1#p' gradle/wrapper/gradle-wrapper.properties)"
expect_equal "Gradle Wrapper checksum" "$gradle_distribution_sha256" \
    "$(sed -n 's/^distributionSha256Sum=//p' gradle/wrapper/gradle-wrapper.properties)"
expect_equal "Gradle Wrapper JAR checksum" "$gradle_wrapper_jar_sha256" \
    "$(sha256_file gradle/wrapper/gradle-wrapper.jar)"
grep -Fq \
    "<trusted-key id=\"$gradle_signing_key_fingerprint\" group=\"gradle\" name=\"gradle\" version=\"$gradle_version\"/>" \
    gradle/verification-metadata.xml \
    || fail "Gradle dependency-verification signing key is out of sync"
gradle_signing_subkey=$(printf '%s\n' "$gradle_signing_key_fingerprint" | sed 's/.*\(................\)$/\1/')
grep -Fqx "sub    $gradle_signing_subkey" gradle/verification-keyring.keys \
    || fail "Gradle dependency-verification keyring is missing $gradle_signing_key_fingerprint"
expect_equal "Gradle daemon JDK" "$java_version" \
    "$(sed -n 's/^toolchainVersion=//p' gradle/gradle-daemon-jvm.properties)"

agp_version=$(toml_value agp)
kotlin_version=$(toml_value kotlin)
for declaration in \
    "packages/kotlin-markdown-core/consumers/android/build.gradle.kts|version \"$agp_version\"" \
    "packages/kotlin-markdown-core/consumers/kmp/build.gradle.kts|version \"$kotlin_version\"" \
    "packages/kotlin-markdown-core/consumers/jvm-gradle/build.gradle.kts|version \"$kotlin_version\""; do
    file=${declaration%%|*}
    value=${declaration#*|}
    grep -Fq "$value" "$file" || fail "$file does not match the version catalog"
done

for file in \
    Package.swift \
    packages/swift-markdown-core/Package.release.swift \
    packages/swift-markdown-core/Tests/Consumer/Package.swift; do
    grep -Fq '// swift-tools-version: 6.3' "$file" || fail "$file does not require Swift tools 6.3"
    grep -Fq '.iOS(.v26)' "$file" || fail "$file does not require iOS 26"
    grep -Fq '.macOS(.v26)' "$file" || fail "$file does not require macOS 26"
done
grep -Fq 'platform: iOS' .github/workflows/ci.yml || fail "CI does not validate iOS deployment"
grep -Fq 'platform: macOS' .github/workflows/ci.yml || fail "CI does not validate macOS deployment"
[ "$(grep -c 'version: "26.0"' .github/workflows/ci.yml)" -eq 2 ] || \
    fail "CI must validate exactly the iOS 26.0 and macOS 26.0 deployment floors"

expect_equal "SwiftLint installer" "$swiftlint_version" \
    "$(sed -n 's/^VERSION="\([^"]*\)"/\1/p' scripts/install-swiftlint.sh)"
expect_equal "SwiftLint runner" "$swiftlint_version" \
    "$(sed -n 's/^VERSION="\([^"]*\)"/\1/p' scripts/lint-swift.sh)"
expect_equal "clang-format installer" "$clang_format_version" \
    "$(sed -n 's/^VERSION="\([^"]*\)"/\1/p' scripts/install-clang-format.sh)"
expect_equal "clang-format runner" "$clang_format_version" \
    "$(sed -n 's/^EXPECTED_VERSION="\([^"]*\)"/\1/p' scripts/format-c.sh)"

printf 'Toolchain declarations are consistent (Swift %s, iOS/macOS 26).\n' "$swift_version"
