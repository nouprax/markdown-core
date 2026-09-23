#!/bin/sh
# Every gate in one run, with the number each one prints.
#
# NOT a CI entry point and not in package.json: it is the loop a step is worked
# in. The presets have their own configure and build and this script runs
# NEITHER -- build first, or a sanitizer preset with no build reports GREEN
# having run nothing (see docs/RECONSTRUCTION.md).
#
#   cmake --preset default && cmake --build --preset default --parallel
#   cmake --preset asan    && cmake --build --preset asan    --parallel
#   cmake --preset ubsan   && cmake --build --preset ubsan   --parallel
#   cmake --preset tsan    && cmake --build --preset tsan    --parallel
#   sh scripts/tooling/check-local-gates.sh
#
# The binding suites are not here either -- they need their own toolchains:
#   pnpm run test:es-node && pnpm run conformance:es-node
#   pnpm run test:swift-macos && pnpm run conformance:swift-macos
#   scripts/tooling/run-gradle.sh :packages:kotlin-markdown-core:jvmTest
#   pnpm -w run lint
set -u
echo "correctness       : $(ctest --preset correctness -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "correctness-asan  : $(ctest --preset correctness-asan -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "correctness-ubsan : $(ctest --preset correctness-ubsan -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "correctness-tsan  : $(ctest --preset correctness-tsan -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "conformance       : $(ctest --preset conformance -j 8 2>&1 | grep -oE '[0-9]+% tests passed out of [0-9]+')"
echo "canonical-ast     : $(node scripts/conformance/check-canonical-ast.mjs 2>&1 | tail -1)"
# Both of these are section 4.8 gates and NEITHER was in this script until Step
# 9b.1, which is how a script that claims to run every gate came to run two
# fewer than the acceptance checklist names.
echo "ast-projections   : $(node scripts/audit/check-ast-projections.mjs 2>&1 | tail -1)"
echo "source-lists      : $(node scripts/audit/check-source-lists.mjs 2>&1 | tail -1)"
echo "public-surface    : $(bash scripts/audit/check-public-surface.sh 2>&1 | tail -1)"
echo "package-contents  : $(bash scripts/audit/check-package-contents.sh 2>&1 | tail -1)"
echo "special-chars     : $(node scripts/audit/check-element-special-chars.mjs 2>&1 | head -1)"
echo "attach-order      : $(node scripts/audit/check-element-attach-order.mjs 2>&1 | tail -1)"
echo "fuzz-commonmark   : $(node scripts/correctness/fuzz-parity.mjs --oracle commonmark --iterations 300 2>&1 | grep -E 'fuzz-parity \[commonmark\]:')"
echo "fuzz-gfm          : $(node scripts/correctness/fuzz-parity.mjs --oracle gfm --iterations 300 2>&1 | grep -E 'fuzz-parity \[gfm\]:')"
echo "fuzz-remark       : $(node scripts/correctness/fuzz-parity.mjs --oracle remark --iterations 300 2>&1 | grep -E 'fuzz-parity \[remark\]:')"
echo "commonmark-parity : $(node scripts/correctness/check-upstream-parity.mjs --oracle commonmark 2>&1 | grep -E 'upstream parity:|divergences:')"
echo "gfm-parity        : $(node scripts/correctness/check-upstream-parity.mjs --oracle gfm 2>&1 | grep -E 'upstream parity:|divergences:')"
echo "mdast-parity      : $(node scripts/correctness/check-mdast-parity.mjs 2>&1 | grep -E 'mdast parity:|backlog:')"
echo "inline-sourcepos  : $(node scripts/audit/check-inline-sourcepos.mjs 2>&1 | tail -1)"
echo "scope-containment : $(node scripts/audit/check-scope-containment.mjs 2>&1 | tail -1)"
echo "position-places   : $(node scripts/audit/check-position-places.mjs 2>&1 | tail -1)"
echo "reference-order   : $(node scripts/audit/check-reference-order-independence.mjs 2>&1 | tail -1)"
echo "test-topology     : $(bash scripts/audit/check-test-topology.sh 2>&1 | tail -1)"
# THE REPOSITORY AUDIT WAS IN NEITHER THIS SCRIPT NOR SECTION 0'S LIST, and it
# is the only gate that reads a tracked file's MODE. Six scripts this branch
# added carry a shebang without the executable bit, which nothing else can see
# and which fails `Health Check - Repository` in CI one script at a time. Run
# WITHOUT --clean here: the --clean form additionally demands a clean checkout,
# which is never true in the middle of a step.
echo "repository        : $(bash scripts/audit/check-repository.sh 2>&1 | tail -1)"
# WHAT JAVA SEES OF THE KOTLIN BINDING, which `checkKotlinAbi` cannot answer:
# its dump is the Kotlin API and every leaked name is absent from it by
# construction. Needs a JDK and the compiled JVM classes.
echo "kotlin-jvm-surface: $(bash scripts/audit/check-kotlin-jvm-surface.sh 2>&1 | tail -1)"
sh scripts/tooling/format-c.sh --check >/dev/null 2>&1; echo "format-c          : exit $?"
sh scripts/tooling/format-cmake.sh --check >/dev/null 2>&1; echo "format-cmake      : exit $?"
printf "lint-c            : "; scripts/tooling/lint-c.sh >/dev/null 2>&1 && echo "exit 0" || echo "FAILED"
