#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

clean=false
physical=false
case "${1:-}" in
    "") ;;
    --clean) clean=true ;;
    --physical)
        clean=true
        physical=true
        ;;
    *)
        echo "Usage: $0 [--clean|--physical]" >&2
        exit 2
        ;;
esac

fail() {
    echo "Repository audit failed: $1" >&2
    exit 1
}

if [ "$clean" = true ]; then
    status=$(git status --porcelain --untracked-files=all)
    if [ -n "$status" ]; then
        printf '%s\n' "$status" >&2
        fail "the source snapshot is not a clean checkout"
    fi
fi

tracked_secret_paths=$(git ls-files | awk '
    /(^|\/)\.env($|\.)/ && $0 !~ /\.env\.example$/ { print }
    /\.(jks|keystore|p12|pem)$/ { print }
')
if [ -n "$tracked_secret_paths" ]; then
    printf '%s\n' "$tracked_secret_paths" >&2
    fail "credential-shaped files are tracked"
fi

secret_pattern='-----BEGIN (RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{36,}|npm_[A-Za-z0-9]{36,}|AKIA[0-9A-Z]{16}'
if secret_matches=$(git grep -Il -E -e "$secret_pattern" -- .); then
    printf '%s\n' "$secret_matches" >&2
    fail "high-confidence credential material is tracked"
fi

large_file_report=$(mktemp)
trap 'rm -f "$large_file_report"' EXIT
git ls-files | while IFS= read -r path; do
    [ -f "$path" ] || continue
    size=$(wc -c <"$path" | tr -d ' ')
    if [ "$size" -gt 5242880 ]; then
        printf '%s (%s bytes)\n' "$path" "$size"
    fi
done >"$large_file_report"
large_files=$(cat "$large_file_report")
if [ -n "$large_files" ]; then
    printf '%s\n' "$large_files" >&2
    fail "tracked files exceed the reviewed 5 MiB limit"
fi

git ls-files -s | awk '$1 == "120000" { print $4 }' | while IFS= read -r link; do
    [ -L "$link" ] || fail "tracked symlink is missing: $link"
    target=$(readlink "$link")
    case "$target" in
        /*) fail "tracked symlink is absolute: $link" ;;
    esac
    [ -e "$link" ] || fail "tracked symlink is broken: $link -> $target"
done

git ls-files scripts | while IFS= read -r path; do
    [ -f "$path" ] || continue
    if [ "$(head -c 2 "$path")" = '#!' ]; then
        mode=$(git ls-files -s -- "$path" | awk 'NR == 1 { print $1 }')
        [ "$mode" = "100755" ] || fail "script with a shebang is not executable: $path"
    fi
done

git ls-files | while IFS= read -r path; do
    [ -f "$path" ] && [ -s "$path" ] || continue
    if LC_ALL=C grep -Iq -- . "$path"; then
        last_byte=$(tail -c 1 "$path" | od -An -t u1 | tr -d ' ')
        [ "$last_byte" = "10" ] || fail "text file lacks a final newline: $path"
    fi
done

for required in LICENSE COPYING UPSTREAM.md; do
    [ -f "$required" ] || fail "required attribution file is missing: $required"
done
cmp LICENSE packages/es-markdown-core/LICENSE >/dev/null \
    || fail "the npm package license differs from the repository license"

grep -q '^group = "com.nouprax"$' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "the Kotlin Maven group changed"
grep -q 'artifactId = "kotlin-markdown-core-android-runtime"' \
    packages/kotlin-markdown-core/android-runtime/build.gradle.kts \
    || fail "the Android runtime coordinate changed"
grep -q '"name": "@nouprax/es-markdown-core"' packages/es-markdown-core/package.json \
    || fail "the npm package coordinate changed"
grep -q 'name: "swift-markdown-core"' Package.swift \
    || fail "the Swift package identity changed"

if [ "$physical" = true ]; then
    generated=$(find . -type d \
        \( -name .build -o -name .cxx -o -name .gradle -o -name .idea \
        -o -name .kotlin -o -name .pnpm-store -o -name .swiftpm -o -name .tools \
        -o -name .vscode -o -name build -o -name DerivedData -o -name dist \
        -o -name node_modules -o -name target \) \
        -not -path './.git/*' -prune -print | sort)
    if [ -n "$generated" ]; then
        printf '%s\n' "$generated" >&2
        fail "generated, cache, dependency, or IDE directories remain"
    fi

    empty_dirs=$(find . -type d -empty -not -path './.git/*' -print | sort)
    if [ -n "$empty_dirs" ]; then
        printf '%s\n' "$empty_dirs" >&2
        fail "empty directories remain in the physical checkout"
    fi
fi

if [ "$physical" = true ]; then
    echo "Repository audit passed (physical checkout)"
else
    echo "Repository audit passed"
fi
