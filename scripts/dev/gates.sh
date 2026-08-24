#!/bin/sh
# Every gate in one run, with the number each one prints.
#
# NOT a CI entry point and not in package.json: it is the loop a step is worked
# in. The presets have their own configure and build and this script runs
# NEITHER -- build first, or a sanitizer preset with no build reports GREEN
# having run nothing (see docs/RECONSTRUCTION.md section 0).
#
#   cmake --preset default && cmake --build --preset default --parallel
#   cmake --preset asan    && cmake --build --preset asan    --parallel
#   cmake --preset ubsan   && cmake --build --preset ubsan   --parallel
#   sh scripts/dev/gates.sh
#
# The binding suites are not here either -- they need their own toolchains:
#   pnpm run test:es-node && pnpm run conformance:es-node
#   pnpm run test:swift-macos && pnpm run conformance:swift-macos
#   scripts/gradle.sh :packages:kotlin-markdown-core:jvmTest
#   pnpm -w run lint
set -u
echo "correctness       : $(ctest --preset correctness -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "correctness-asan  : $(ctest --preset correctness-asan -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "correctness-ubsan : $(ctest --preset correctness-ubsan -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "conformance       : $(ctest --preset conformance -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "canonical-ast     : $(node scripts/check-canonical-ast-fixtures.mjs 2>&1 | tail -1)"
# Both of these are section 4.8 gates and NEITHER was in this script until Step
# 9b.1, which is how a script that claims to run every gate came to run two
# fewer than the acceptance checklist names.
echo "ast-projections   : $(node scripts/audit-ast-projections.mjs 2>&1 | tail -1)"
echo "source-lists      : $(node scripts/audit-source-lists.mjs 2>&1 | tail -1)"
echo "public-surface    : $(bash scripts/audit-public-surface.sh 2>&1 | tail -1)"
echo "special-chars     : $(node scripts/audit-extension-special-chars.mjs 2>&1 | head -1)"
echo "attach-order      : $(node scripts/audit-extension-attach-order.mjs 2>&1 | tail -1)"
echo "plan-graph        : $(node scripts/check-plan-graph.mjs 2>&1 | tail -1)"
echo "fuzz-upstream     : $(node scripts/fuzz-parity.mjs --iterations 300 2>&1 | grep -E 'fuzz-parity \[upstream\]:')"
# THE MDAST FUZZ ORACLE WAS IN SECTION 0'S LIST AND NOT IN THIS SCRIPT, which is
# the same hole the comment above records for two other gates: a script that
# claims to run every gate ran one fewer than section 0 names. It turned green
# at 9b.2 and has been unrun by this loop ever since.
echo "fuzz-mdast        : $(node scripts/fuzz-parity.mjs --oracle mdast --iterations 300 2>&1 | grep -E 'fuzz-parity \[mdast\]:')"
echo "upstream-parity   : $(node scripts/check-upstream-parity.mjs 2>&1 | grep -E 'upstream parity:|divergences:')"
echo "mdast-parity      : $(node scripts/check-mdast-parity.mjs 2>&1 | grep -E 'mdast parity:|backlog:')"
echo "scope-sanity      : $(node scripts/audit-scope-sanity.mjs 2>&1 | tail -1)"
echo "inline-sourcepos  : $(node scripts/audit-inline-sourcepos.mjs 2>&1 | tail -1)"
echo "scope-containment : $(node scripts/audit-scope-containment.mjs 2>&1 | tail -1)"
echo "position-places   : $(node scripts/audit-position-places.mjs 2>&1 | tail -1)"
echo "diagnostics       : $(node scripts/audit-diagnostics.mjs 2>&1 | tail -1)"
echo "reference-order   : $(node scripts/audit-reference-order-independence.mjs 2>&1 | tail -1)"
echo "test-topology     : $(bash scripts/audit-test-topology.sh 2>&1 | tail -1)"
sh scripts/format-c.sh --check >/dev/null 2>&1; echo "format-c          : exit $?"
sh scripts/format-cmake.sh --check >/dev/null 2>&1; echo "format-cmake      : exit $?"
printf "lint-c            : "; scripts/lint-c.sh >/dev/null 2>&1 && echo "exit 0" || echo "FAILED"
