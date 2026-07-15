#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:?usage: stage-c-release.sh OUTPUT_DIRECTORY}
version=$(cat "$root/VERSION")
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
name="markdown-core-c-$version-$os-$arch"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
prefix="$temporary/$name"
build="$temporary/build"

rm -rf "$output"
mkdir -p "$output"
cmake -S "$root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DMARKDOWN_CORE_TESTS=OFF \
    -DMARKDOWN_CORE_STATIC=ON \
    -DMARKDOWN_CORE_SHARED=ON \
    -DMARKDOWN_CORE_WARNINGS_AS_ERRORS=ON
cmake --build "$build" --config Release --parallel 2
cmake --install "$build" --config Release

find "$prefix" \( -type f -o -type l \) | while IFS= read -r artifact; do
    relative=${artifact#"$prefix/"}
    case "$relative" in
        bin/markdown-core | \
        include/markdown_core.h | \
        lib/cmake/markdown-core/markdown-core-config.cmake | \
        lib/cmake/markdown-core/markdown-core-config-version.cmake | \
        lib/cmake/markdown-core/markdown-core-targets.cmake | \
        lib/cmake/markdown-core/markdown-core-targets-release.cmake | \
        lib/libmarkdown-core.a | \
        lib/libmarkdown-core*.dylib | \
        lib/libmarkdown-core.so | \
        lib/libmarkdown-core.so.* | \
        lib/pkgconfig/markdown-core.pc)
            ;;
        *) echo "unexpected C release artifact: $relative" >&2; exit 1 ;;
    esac
done
if grep -R -I -n -E 'canonical-ast|markdown-core-extensions|markdown_core_(markdown_to_html|render_)' "$prefix"; then
    echo "C release artifact exposes test data, private extensions, or renderer API" >&2
    exit 1
fi

cmake -S "$root/packages/markdown-core/tests/consumers/cmake" \
    -B "$temporary/consumer-build" \
    -DCMAKE_PREFIX_PATH="$prefix" >/dev/null
cmake --build "$temporary/consumer-build" --parallel 2 >/dev/null
DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$temporary/consumer-build/markdown-core-installed-consumer"

if command -v pkg-config >/dev/null 2>&1; then
    PKG_CONFIG_PATH="$prefix/lib/pkgconfig" \
        cc "$root/packages/markdown-core/tests/consumers/c/main.c" \
        -o "$temporary/pkg-config-consumer" \
        $(PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config --cflags --libs markdown-core)
    DYLD_LIBRARY_PATH="$prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$temporary/pkg-config-consumer"
fi

tar -czf "$output/$name.tar.gz" -C "$temporary" "$name"
echo "Staged $output/$name.tar.gz"
