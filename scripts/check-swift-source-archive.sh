#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
destination=${1:-}
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
archive="$temporary/markdown-core.zip"
unpacked="$temporary/unpacked"
package="$unpacked/markdown-core"
consumer="$temporary/consumer"

cd "$root"
swift package archive-source --output "$archive"
mkdir -p "$unpacked"
unzip -q "$archive" -d "$unpacked"

for required in \
    Package.swift \
    specs/canonical-ast/manifest.json \
    packages/swift-markdown-core/Tests/MarkdownCoreConformanceTests/ConformanceSuite.swift \
    packages/swift-markdown-core/Plugins/GenerateCanonicalASTResources/plugin.swift \
    packages/swift-markdown-core/Tools/CanonicalASTResourceGenerator/CanonicalASTResourceGenerator.swift; do
    [ -f "$package/$required" ] || { echo "Swift source archive is missing $required" >&2; exit 1; }
done

if find "$package" -type d \( -name .build -o -name build -o -name dist -o -name node_modules \) -print | grep -q .; then
    echo "Swift source archive contains derived build or dependency output" >&2
    exit 1
fi

node "$package/scripts/check-canonical-ast-fixtures.mjs"
CLANG_MODULE_CACHE_PATH="$temporary/archive-module-cache" \
    swift test --disable-sandbox --package-path "$package" \
    --filter '^MarkdownCoreConformanceTests\.'
CLANG_MODULE_CACHE_PATH="$temporary/product-module-cache" \
    swift build --disable-sandbox --package-path "$package" --target MarkdownCore

mkdir -p "$consumer/Sources/Consumer"
printf '%s\n' \
    '// swift-tools-version: 6.0' \
    'import PackageDescription' \
    '' \
    'let package = Package(' \
    '    name: "ReleaseConsumer",' \
    '    platforms: [.macOS(.v15)],' \
    '    dependencies: [.package(path: "../unpacked/markdown-core")],' \
    '    targets: [' \
    '        .executableTarget(' \
    '            name: "Consumer",' \
    '            dependencies: [.product(name: "MarkdownCore", package: "markdown-core")]' \
    '        )' \
    '    ]' \
    ')' >"$consumer/Package.swift"
printf '%s\n' \
    'import MarkdownCore' \
    '' \
    'let document = try Document.parse("## archived consumer")' \
    'guard (document.children.first as? Heading)?.level == 2 else { fatalError("parse failed") }' \
    'print(document.dump())' >"$consumer/Sources/Consumer/main.swift"

CLANG_MODULE_CACHE_PATH="$temporary/consumer-module-cache" \
    swift run --disable-sandbox --package-path "$consumer" Consumer >/dev/null
if find "$consumer/.build" -type f \
    \( -name 'CanonicalAstCases.swift' -o -name manifest.json -o -name '*.ast' \) -print | grep -q .; then
    echo "product-only Swift consumer built or carried conformance fixtures" >&2
    exit 1
fi

if [ -n "$destination" ]; then
    mkdir -p "$destination"
    cp "$archive" "$destination/markdown-core-source-$(cat "$root/VERSION").zip"
fi

echo "Swift source archive, build-tool plugin, conformance, and product-only consumer passed."
