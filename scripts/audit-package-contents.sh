#!/bin/sh
set -eu

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT
export npm_config_cache="${npm_config_cache:-$temp_dir/npm-cache}"

legacy_ms_pattern='MS_''COPILOT|MS_''FORMULA|ms_''copilot|ms-''copilot|ms-''formula'
if grep -R -I -n -E "$legacy_ms_pattern" \
    packages/markdown-core packages/swift-markdown-core \
    packages/kotlin-markdown-core packages/es-markdown-core scripts; then
    echo "MS-specific surface remains in repository source" >&2
    exit 1
fi

renderer_api_pattern='markdown_core_(markdown_to_html|render_)|markdown_core_syntax_extension_set_(commonmark_render|plaintext_render|latex_render|xml_attr|man_render|html_render|html_filter|commonmark_escape)'
if grep -R -I -n -E "$renderer_api_pattern" \
    packages/markdown-core/core packages/markdown-core/extensions \
    packages/markdown-core/include Package.swift Makefile scripts; then
    echo "Renderer API remains in repository source" >&2
    exit 1
fi

for removed_renderer in \
    packages/markdown-core/core/render.c \
    packages/markdown-core/core/render.h \
    packages/markdown-core/core/html.c \
    packages/markdown-core/core/html.h \
    packages/markdown-core/core/xml.c \
    packages/markdown-core/core/commonmark.c \
    packages/markdown-core/core/latex.c \
    packages/markdown-core/core/man.c \
    packages/markdown-core/core/plaintext.c; do
    if [ -e "$removed_renderer" ]; then
        echo "Removed renderer source still exists: $removed_renderer" >&2
        exit 1
    fi
done

cmp LICENSE packages/es-markdown-core/LICENSE
node packages/es-markdown-core/scripts/build.mjs >/dev/null
npm pack ./packages/es-markdown-core --dry-run --json >"$temp_dir/npm-pack.json"
node - "$temp_dir/npm-pack.json" <<'NODE'
import fs from "node:fs";

const report = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const files = report.flatMap((entry) => entry.files.map((file) => file.path));
const unexpected = files.filter(
    (file) =>
        !["LICENSE", "README.md", "package.json", "dist/markdown-core.wasm"].includes(file) &&
        !/^dist\/.+\.(?:js|d\.ts)$/.test(file)
);
for (const required of [
    "LICENSE",
    "README.md",
    "package.json",
    "dist/index.js",
    "dist/index.d.ts",
    "dist/markdown-core.wasm"
]) {
    if (!files.includes(required)) {
        throw new Error(`npm package is missing ${required}`);
    }
}
if (unexpected.length > 0) {
    throw new Error(`Unexpected npm package files: ${unexpected.join(", ")}`);
}
const manifest = JSON.parse(fs.readFileSync("packages/es-markdown-core/package.json", "utf8"));
if (manifest.private !== false) {
    throw new Error("ES package must be publishable");
}
NODE

CLANG_MODULE_CACHE_PATH="$temp_dir/swift-module-cache" \
    swift package --disable-sandbox dump-package >"$temp_dir/swift-package.json"
node - "$temp_dir/swift-package.json" <<'NODE'
import fs from "node:fs";

const manifest = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
if (manifest.name !== "swift-markdown-core") {
    throw new Error(`Unexpected SwiftPM package name: ${manifest.name}`);
}
const product = manifest.products.find((candidate) => candidate.name === "MarkdownCore");
if (!product) {
    throw new Error("SwiftPM MarkdownCore product is missing");
}
const target = manifest.targets.find((candidate) => candidate.name === "MarkdownCore");
if (!target || target.path !== "packages/swift-markdown-core/Sources/MarkdownCore") {
    throw new Error("SwiftPM MarkdownCore target path changed unexpectedly");
}
if (target.resources.length !== 0) {
    throw new Error("SwiftPM public product must not package canonical AST spec data");
}
const cTarget = manifest.targets.find((candidate) => candidate.name === "MarkdownCoreC");
if (!cTarget || cTarget.path !== "packages/markdown-core") {
    throw new Error("SwiftPM internal C target path changed unexpectedly");
}
const allowedPrefixes = ["core/", "extensions/"];
const unexpected = cTarget.sources.filter(
    (source) => !allowedPrefixes.some((prefix) => source.startsWith(prefix))
);
if (unexpected.length > 0) {
    throw new Error(`Unexpected SwiftPM sources: ${unexpected.join(", ")}`);
}
const conformanceTarget = manifest.targets.find(
    (candidate) => candidate.name === "MarkdownCoreConformanceTests"
);
if (
    !conformanceTarget ||
    conformanceTarget.resources.length !== 0 ||
    conformanceTarget.pluginUsages?.length !== 1 ||
    conformanceTarget.pluginUsages[0].plugin?.[0] !== "GenerateCanonicalASTResources"
) {
    throw new Error("Swift conformance test bundle must derive resources through its build-tool plugin");
}
const generatorTarget = manifest.targets.find(
    (candidate) => candidate.name === "CanonicalASTResourceGenerator"
);
if (
    !generatorTarget ||
    generatorTarget.type !== "executable" ||
    generatorTarget.path !== "packages/swift-markdown-core/Tools/CanonicalASTResourceGenerator"
) {
    throw new Error("Swift canonical AST resource generator target is missing or misplaced");
}
const pluginTarget = manifest.targets.find(
    (candidate) => candidate.name === "GenerateCanonicalASTResources"
);
if (
    !pluginTarget ||
    pluginTarget.type !== "plugin" ||
    !("buildTool" in pluginTarget.pluginCapability) ||
    pluginTarget.path !== "packages/swift-markdown-core/Plugins/GenerateCanonicalASTResources"
) {
    throw new Error("Swift canonical AST build-tool plugin target is missing or invalid");
}
NODE

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config is required for installed C consumer validation" >&2
    exit 1
fi

cmake -S . -B "$temp_dir/cmake-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMARKDOWN_CORE_TESTS=OFF \
    -DMARKDOWN_CORE_STATIC=OFF \
    -DMARKDOWN_CORE_SHARED=ON \
    -DCMAKE_INSTALL_PREFIX="$temp_dir/cmake-prefix" >/dev/null
cmake --build "$temp_dir/cmake-build" --parallel 2 >/dev/null
cmake --install "$temp_dir/cmake-build" >/dev/null

cmake -S . -B "$temp_dir/cmake-build-static" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMARKDOWN_CORE_TESTS=OFF \
    -DMARKDOWN_CORE_STATIC=ON \
    -DMARKDOWN_CORE_SHARED=OFF \
    -DCMAKE_INSTALL_PREFIX="$temp_dir/cmake-prefix-static" >/dev/null
cmake --build "$temp_dir/cmake-build-static" --parallel 2 >/dev/null
cmake --install "$temp_dir/cmake-build-static" >/dev/null

cli="$temp_dir/cmake-build/packages/markdown-core/core/markdown-core"
if "$cli" --to html </dev/null >/dev/null 2>&1; then
    echo "CLI still accepts renderer output formats" >&2
    exit 1
fi
if "$cli" --help | grep -E -- '--to|--width|--unsafe|--hardbreaks|--nobreaks'; then
    echo "CLI help still advertises renderer options" >&2
    exit 1
fi

if grep -R -I -n -E "$legacy_ms_pattern" "$temp_dir/cmake-prefix"; then
    echo "MS-specific surface remains in installed C artifacts" >&2
    exit 1
fi

find "$temp_dir/cmake-prefix" \( -type f -o -type l \) | while IFS= read -r artifact; do
    relative_path=${artifact#"$temp_dir/cmake-prefix/"}
    case "$relative_path" in
        bin/markdown-core | \
        include/markdown_core.h | \
        lib/cmake/markdown-core/markdown-core-config.cmake | \
        lib/cmake/markdown-core/markdown-core-config-version.cmake | \
        lib/cmake/markdown-core/markdown-core-targets.cmake | \
        lib/cmake/markdown-core/markdown-core-targets-release.cmake | \
        lib/libmarkdown-core* | \
        lib/pkgconfig/markdown-core.pc)
            ;;
        *)
            echo "Unexpected C install artifact: $relative_path" >&2
            exit 1
            ;;
    esac
done

find "$temp_dir/cmake-prefix-static" \( -type f -o -type l \) | while IFS= read -r artifact; do
    relative_path=${artifact#"$temp_dir/cmake-prefix-static/"}
    case "$relative_path" in
        bin/markdown-core | \
        include/markdown_core.h | \
        lib/cmake/markdown-core/markdown-core-config.cmake | \
        lib/cmake/markdown-core/markdown-core-config-version.cmake | \
        lib/cmake/markdown-core/markdown-core-targets.cmake | \
        lib/cmake/markdown-core/markdown-core-targets-release.cmake | \
        lib/libmarkdown-core.a | \
        lib/pkgconfig/markdown-core.pc)
            ;;
        *)
            echo "Unexpected static C install artifact: $relative_path" >&2
            exit 1
            ;;
    esac
done

for prefix in "$temp_dir/cmake-prefix" "$temp_dir/cmake-prefix-static"; do
    if grep -R -I -n 'markdown-core-extensions' "$prefix"; then
        echo "Installed C metadata exposes the private extensions target" >&2
        exit 1
    fi
done

PKG_CONFIG_PATH="$temp_dir/cmake-prefix/lib/pkgconfig" \
    cc packages/markdown-core/tests/consumers/c/main.c \
    -o "$temp_dir/pkg-config-consumer-shared" \
    $(PKG_CONFIG_PATH="$temp_dir/cmake-prefix/lib/pkgconfig" \
        pkg-config --cflags --libs markdown-core)
DYLD_LIBRARY_PATH="$temp_dir/cmake-prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    LD_LIBRARY_PATH="$temp_dir/cmake-prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$temp_dir/pkg-config-consumer-shared"

PKG_CONFIG_PATH="$temp_dir/cmake-prefix-static/lib/pkgconfig" \
    cc packages/markdown-core/tests/consumers/c/main.c \
    -o "$temp_dir/pkg-config-consumer-static" \
    $(PKG_CONFIG_PATH="$temp_dir/cmake-prefix-static/lib/pkgconfig" \
        pkg-config --static --cflags --libs markdown-core)
"$temp_dir/pkg-config-consumer-static"

cmake -S packages/markdown-core/tests/consumers/cmake \
    -B "$temp_dir/cmake-consumer-shared" \
    -DCMAKE_PREFIX_PATH="$temp_dir/cmake-prefix" >/dev/null
cmake --build "$temp_dir/cmake-consumer-shared" --parallel 2 >/dev/null
DYLD_LIBRARY_PATH="$temp_dir/cmake-prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    LD_LIBRARY_PATH="$temp_dir/cmake-prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$temp_dir/cmake-consumer-shared/markdown-core-installed-consumer"

cmake -S packages/markdown-core/tests/consumers/cmake \
    -B "$temp_dir/cmake-consumer-static" \
    -DCMAKE_PREFIX_PATH="$temp_dir/cmake-prefix-static" >/dev/null
cmake --build "$temp_dir/cmake-consumer-static" --parallel 2 >/dev/null
"$temp_dir/cmake-consumer-static/markdown-core-installed-consumer"

nm_tool=$(command -v llvm-nm || true)
if [ -z "$nm_tool" ] && command -v xcrun >/dev/null 2>&1; then
    nm_tool=$(xcrun -f llvm-nm 2>/dev/null || true)
fi
if [ -z "$nm_tool" ]; then
    nm_tool=$(command -v nm || true)
fi
if [ -z "$nm_tool" ]; then
    echo "No nm implementation is available for the C export audit" >&2
    exit 1
fi
facade_library=$(find "$temp_dir/cmake-prefix/lib" -type f \
    \( -name 'libmarkdown-core.so*' -o -name 'libmarkdown-core.*.dylib' \) | head -n 1)
if [ -z "$facade_library" ]; then
    echo "Installed C facade shared library was not found" >&2
    exit 1
fi
case "$facade_library" in
    *.dylib)
        "$nm_tool" -gU "$facade_library" | awk '{ print $NF }' | sed 's/^_//' ;;
    *)
        "$nm_tool" -D --defined-only "$facade_library" | awk '{ print $NF }' | sed 's/@.*//' ;;
esac | grep '^markdown_core_' | sort -u >"$temp_dir/c-actual-exports.txt"
awk '
    /global:/ { global_symbols = 1; next }
    /local:/ { global_symbols = 0 }
    global_symbols {
        gsub(/[;[:space:]]/, "")
        if (length > 0) print
    }
' packages/markdown-core/core/exports/markdown_core.map \
    | sort -u >"$temp_dir/c-expected-exports.txt"
if ! cmp "$temp_dir/c-expected-exports.txt" "$temp_dir/c-actual-exports.txt"; then
    echo "C facade exports differ from the read-only allowlist" >&2
    diff -u "$temp_dir/c-expected-exports.txt" "$temp_dir/c-actual-exports.txt" >&2 || true
    exit 1
fi

scripts/gradle.sh :packages:kotlin-markdown-core:verifyKotlinNativePackaging >/dev/null
kotlin_jvm_jar=$(find packages/kotlin-markdown-core/build/libs -maxdepth 1 -type f \
    -name '*-jvm-*.jar' ! -name '*-sources.jar' | head -n 1)
if [ -z "$kotlin_jvm_jar" ]; then
    echo "Kotlin JVM publication JAR is missing" >&2
    exit 1
fi
if unzip -Z1 "$kotlin_jvm_jar" | grep -E '(^|/)(canonical-ast|manifest\.json)|\.ast$'; then
    echo "Kotlin JVM publication contains shared conformance spec data" >&2
    exit 1
fi
kotlin_aar="packages/kotlin-markdown-core/android-runtime/build/outputs/aar/android-runtime-release.aar"
unzip -Z1 "$kotlin_aar" | while IFS= read -r artifact; do
    case "$artifact" in
        */ | \
        AndroidManifest.xml | \
        R.txt | \
        classes.jar | \
        META-INF/com/android/build/gradle/aar-metadata.properties | \
        jni/arm64-v8a/libmarkdown_core_kotlin.so | \
        jni/armeabi-v7a/libmarkdown_core_kotlin.so | \
        jni/x86/libmarkdown_core_kotlin.so | \
        jni/x86_64/libmarkdown_core_kotlin.so)
            ;;
        *)
            echo "Unexpected Kotlin Android runtime AAR entry: $artifact" >&2
            exit 1
            ;;
    esac
done

echo "Package content audits passed."
