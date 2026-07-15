#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:?usage: stage-npm-release.sh OUTPUT_DIRECTORY}
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
export npm_config_cache="$temporary/npm-cache"

rm -rf "$output"
mkdir -p "$output"
cd "$root"
node packages/es-markdown-core/scripts/build.mjs
node packages/es-markdown-core/tests/packaging.mjs all
npm pack ./packages/es-markdown-core --pack-destination "$output" >/dev/null

archive=$(find "$output" -maxdepth 1 -type f -name '*.tgz' | head -n 1)
[ -n "$archive" ] || { echo "npm release archive was not produced" >&2; exit 1; }
package_version=$(tar -xOf "$archive" package/package.json | node -e \
    'let value=""; process.stdin.on("data", chunk => value += chunk); process.stdin.on("end", () => console.log(JSON.parse(value).version));')
[ "$package_version" = "$(cat VERSION)" ] || { echo "npm archive version drifted" >&2; exit 1; }

echo "Staged $archive"
