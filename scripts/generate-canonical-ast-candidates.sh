#!/bin/sh
# Generate review-only canonical AST candidates through the public C CLI.
# This command never changes the accepted files under specs/canonical-ast.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
spec_dir="$root/specs/canonical-ast"
candidate_dir="$root/build/canonical-ast-candidates"
cli="$root/build/cmake/packages/markdown-core/core/markdown-core"

cd "$root"
node scripts/check-canonical-ast-fixtures.mjs
cmake --preset default
cmake --build --preset default --parallel --target markdown-core
cmake -E remove_directory "$candidate_dir"
cmake -E make_directory "$candidate_dir"

node --input-type=module -e '
    import fs from "node:fs";
    const manifest = JSON.parse(fs.readFileSync("specs/canonical-ast/manifest.json", "utf8"));
    for (const testCase of manifest.cases) {
        if (Object.values(testCase.parseOptions).some((value) => value !== true)) {
            throw new Error(`${testCase.name}: the C candidate command needs explicit non-default option support`);
        }
        process.stdout.write(`${testCase.input}\t${testCase.expected}\n`);
    }
' | while IFS="	" read -r input expected; do
    "$cli" "$spec_dir/$input" >"$candidate_dir/$expected"
    diff -u "$spec_dir/$expected" "$candidate_dir/$expected" || {
        status=$?
        [ "$status" -eq 1 ] || exit "$status"
    }
done

echo "Canonical AST candidates are in $candidate_dir"
