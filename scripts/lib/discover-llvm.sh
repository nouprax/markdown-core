# Shared llvm-profdata/llvm-cov discovery for the source-based coverage
# producers.
#
# The pair must match the clang that produced the instrumented build: a
# mismatch fails with an opaque "unsupported instrumentation profile format
# version" rather than a missing-tool error, so this resolves the toolchain
# explicitly instead of taking whatever happens to be first on PATH.
#
# Sourced by callers; sets LLVM_PROFDATA and LLVM_COV.

markdown_core_discover_llvm() {
    if [ -n "${MARKDOWN_CORE_LLVM_BIN:-}" ]; then
        LLVM_PROFDATA="$MARKDOWN_CORE_LLVM_BIN/llvm-profdata"
        LLVM_COV="$MARKDOWN_CORE_LLVM_BIN/llvm-cov"
    elif command -v xcrun >/dev/null 2>&1 && xcrun --find llvm-profdata >/dev/null 2>&1; then
        LLVM_PROFDATA=$(xcrun --find llvm-profdata)
        LLVM_COV=$(xcrun --find llvm-cov)
    else
        LLVM_PROFDATA=""
        LLVM_COV=""
        for suffix in "" -21 -20 -19 -18 -17; do
            if command -v "llvm-profdata$suffix" >/dev/null 2>&1 &&
                command -v "llvm-cov$suffix" >/dev/null 2>&1; then
                LLVM_PROFDATA=$(command -v "llvm-profdata$suffix")
                LLVM_COV=$(command -v "llvm-cov$suffix")
                break
            fi
        done
    fi

    if [ -z "${LLVM_PROFDATA:-}" ] || [ ! -x "$LLVM_PROFDATA" ] ||
        [ -z "${LLVM_COV:-}" ] || [ ! -x "$LLVM_COV" ]; then
        echo "coverage: no matching llvm-profdata/llvm-cov pair found." >&2
        echo "Install the LLVM tools for the compiler in use, or set" >&2
        echo "MARKDOWN_CORE_LLVM_BIN to the directory that holds them." >&2
        return 1
    fi

    export LLVM_PROFDATA LLVM_COV
}
