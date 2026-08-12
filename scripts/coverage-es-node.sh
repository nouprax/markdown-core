#!/bin/sh
# ES Node coverage producer.
#
# Runs the in-process correctness and conformance suites under Node's built-in
# coverage and hands the LCOV tracefile to the repository's one coverage gate.
#
# The `consumer`, `types`, and `packaging` suites are absent on purpose: each
# installs a packed tarball and runs it in a child process the parent cannot
# instrument. They verify packaging and declaration resolution rather than
# binding logic, so nothing they exercise is reachable only through them.
#
# Pass --update-ledger through to record an improvement.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

package="packages/es-markdown-core"
output="$root/build/coverage-es"
report="$output/coverage.lcov"

mkdir -p "$output"

node "$package/scripts/build.mjs"
node "$package/scripts/bundle-conformance-fixtures.mjs"

cd "$root/$package"
node --test \
    --experimental-test-coverage \
    --test-coverage-exclude='tests/**' \
    --test-coverage-exclude='scripts/**' \
    --test-reporter=spec --test-reporter-destination=stdout \
    --test-reporter=lcov --test-reporter-destination="$report" \
    tests/node.test.mjs tests/edit.test.mjs tests/conformance.test.mjs

cd "$root"
exec node scripts/check-coverage.mjs \
    --platform es-node \
    --format lcov \
    --input "build/coverage-es/coverage.lcov" \
    --base "$package" \
    "$@"
