#!/bin/sh
# Swift macOS coverage producer.
#
# Runs the binding's correctness and conformance test targets together under
# SwiftPM's coverage instrumentation and hands the llvm-cov report to the
# repository's one coverage gate. Both targets run in one invocation because
# they exercise one binding: splitting them would make each look worse than the
# shipped surface is.
#
# Pass --update-ledger through to record an improvement.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# shellcheck source=scripts/lib/discover-llvm.sh
. "$root/scripts/lib/discover-llvm.sh"
markdown_core_discover_llvm

output="build/coverage-swift"
report="$output/coverage.json"
mkdir -p "$output"

CLANG_MODULE_CACHE_PATH=build/swift-module-cache \
    swift test --disable-sandbox --enable-code-coverage

bin_path=$(CLANG_MODULE_CACHE_PATH=build/swift-module-cache swift build --show-bin-path)
profdata="$bin_path/codecov/default.profdata"
if [ ! -f "$profdata" ]; then
    echo "coverage: SwiftPM produced no profile at $profdata." >&2
    exit 1
fi

bundle=$(find "$bin_path" -name '*.xctest' -maxdepth 1 | head -n 1)
if [ -z "$bundle" ]; then
    echo "coverage: found no test bundle under $bin_path." >&2
    exit 1
fi
binary="$bundle/Contents/MacOS/$(basename "$bundle" .xctest)"
[ -f "$binary" ] || binary="$bundle/$(basename "$bundle" .xctest)"
if [ ! -f "$binary" ]; then
    echo "coverage: found no test executable inside $bundle." >&2
    exit 1
fi

# The full report, not -summary-only: a summary drops the per-region detail
# check-coverage.mjs needs to attribute an uncovered branch to a line.
"$LLVM_COV" export "$binary" \
    -instr-profile="$profdata" \
    -ignore-filename-regex='(/Tests/|/\.build/|^/usr/|Xcode\.app|/Plugins/)' \
    >"$report"

exec node scripts/check-coverage.mjs \
    --platform swift-macos \
    --format llvm-cov \
    --input "$report" \
    "$@"
