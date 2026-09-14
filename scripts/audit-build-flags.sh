#!/bin/sh
# EVERY PRODUCER OF THE ENGINE BUILDS UNDER ONE RELEASE POLICY. The CMake
# targets take it from MarkdownCoreBuildFlags.cmake; the Swift package, its
# product artifact and the Wasm build state the same policy in their own
# manifests. A producer that drifts from it ships an engine that asserts in
# release or calls across every unit, and nothing else would notice.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

fail() {
    echo "Build flags audit failed: $1" >&2
    exit 1
}

module=packages/markdown-core/cmake/MarkdownCoreBuildFlags.cmake
test -f "$module" || fail "the shared build flags module is missing"
grep -Fq 'option(MARKDOWN_CORE_LTO' "$module" || fail "the module does not own the MARKDOWN_CORE_LTO option"
grep -Fq 'C_VISIBILITY_PRESET hidden' "$module" || fail "the module does not hide symbols"
grep -Fq 'INTERPROCEDURAL_OPTIMIZATION_RELEASE ON' "$module" \
    || fail "the module does not optimize across units in Release"
grep -Fq 'include(MarkdownCoreBuildFlags)' packages/markdown-core/CMakeLists.txt \
    || fail "the C package does not include the shared build flags module"

for pair in \
    'packages/markdown-core/core/CMakeLists.txt:${PROGRAM}' \
    'packages/markdown-core/core/CMakeLists.txt:${STATICLIBRARY}' \
    'packages/markdown-core/elements/CMakeLists.txt:${LIBRARY}' \
    'packages/markdown-core/elements/CMakeLists.txt:${STATICLIBRARY}' \
    'packages/markdown-core/elements/CMakeLists.txt:${PUBLIC_STATIC_LIBRARY}' \
    'packages/markdown-core/elements/CMakeLists.txt:markdown-core-diagnostics' \
    'packages/kotlin-markdown-core/src/native/CMakeLists.txt:markdown_core_kotlin_jni' \
    'packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt:markdown_core_kotlin'; do
    file=${pair%%:*}
    target=${pair#*:}
    grep -Fq "markdown_core_apply_build_flags($target)" "$file" \
        || fail "$file does not apply the shared build flags to $target"
done
grep -Fq "$module" packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt \
    || fail "the Android build does not include the shared build flags module"
if grep -n 'CMAKE_C_VISIBILITY_PRESET\|CMAKE_VISIBILITY_INLINES_HIDDEN\|CMAKE_INTERPROCEDURAL_OPTIMIZATION' \
    packages/markdown-core/CMakeLists.txt packages/markdown-core/core/CMakeLists.txt \
    packages/markdown-core/elements/CMakeLists.txt packages/markdown-core/tests/CMakeLists.txt; then
    fail "a C target sets visibility or interprocedural optimization outside the shared module"
fi

for manifest in Package.swift packages/swift-markdown-core/Package.release.swift; do
    grep -Fq '.define("NDEBUG", .when(configuration: .release))' "$manifest" \
        || fail "$manifest does not define NDEBUG for the release configuration"
done
grep -Fq 'swift build --target MarkdownCore -c release' scripts/build-swift-product-artifact.sh \
    || fail "the Swift product artifact is not a release build"

for flag in '"-O3"' '"-DNDEBUG"' '"-flto"' '"-mbulk-memory"' '"-mnontrapping-fptoint"'; do
    grep -Fq "$flag" packages/es-markdown-core/scripts/build.mjs || fail "the Wasm build does not pass $flag"
done

echo "Build flags audit passed"
