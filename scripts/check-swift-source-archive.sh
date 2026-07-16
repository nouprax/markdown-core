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
mkdir -p \
    "$package/packages/markdown-core" \
    "$package/packages/swift-markdown-core/Sources"
cp packages/swift-markdown-core/Package.release.swift "$package/Package.swift"
cp LICENSE README.md VERSION "$package/"
cp -R packages/markdown-core/core "$package/packages/markdown-core/core"
cp -R packages/markdown-core/extensions "$package/packages/markdown-core/extensions"
cp -R packages/markdown-core/include "$package/packages/markdown-core/include"
cp -R packages/swift-markdown-core/Sources/MarkdownCore \
    "$package/packages/swift-markdown-core/Sources/MarkdownCore"

(cd "$unpacked" && zip -qr "$archive" markdown-core)

for required in \
    Package.swift \
    LICENSE \
    README.md \
    VERSION \
    packages/markdown-core/include/markdown_core.h \
    packages/swift-markdown-core/Sources/MarkdownCore/Document.swift; do
    [ -f "$package/$required" ] || { echo "Swift source archive is missing $required" >&2; exit 1; }
done

if find "$package" -type d \
    \( -iname test -o -iname tests -o -iname benchmarks -o -iname fixtures \
    -o -name .build -o -name build -o -name dist -o -name node_modules \) \
    -print | grep -q .; then
    echo "Swift source archive contains test, benchmark, fixture, build, or dependency content" >&2
    exit 1
fi

if unzip -Z1 "$archive" | grep -E -i \
    '(^|/)(Tests?|Benchmarks?|Fixtures?|Plugins?|Tools?)(/|$)|canonical-ast|\.ast$'; then
    echo "Swift release archive contains non-product source" >&2
    exit 1
fi

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
    \( -iname '*test*' -o -iname '*benchmark*' -o -name 'CanonicalAstCases.swift' \
    -o -name manifest.json -o -name '*.ast' \) -print | grep -q .; then
    echo "product-only Swift consumer built or carried test or benchmark content" >&2
    exit 1
fi

if [ -n "$destination" ]; then
    mkdir -p "$destination"
    cp "$archive" "$destination/markdown-core-source-$(cat "$root/VERSION").zip"
fi

echo "Product-only Swift source archive and external consumer passed."
