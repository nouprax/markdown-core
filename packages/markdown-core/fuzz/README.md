# Fuzzing harnesses

Long-running libFuzzer campaigns live here; the deterministic per-commit fuzz
gates that run in CI are the `fuzz`-labelled CTest tests (`fuzz_smoke` and
`fuzz_script_smoke`).

Both harnesses need a clang with the libFuzzer runtime (plain LLVM clang; the
Apple Xcode toolchain does not ship it).

## Document edit-script fuzzer

`fuzz_document_edits` interprets every input as an edit script over a
document (format documented in
`tests/support/edit_replay.h`: two option bytes, then
insert/delete/replace/commit operations) and verifies each commit through the
shared replay harness — the document dump must equal a one-shot parse of the
same text, the identity double-walk must hold (no id resurrection, no revision regression),
and footnote queries must match a fresh document. Verification failures abort,
so the fuzzer preserves the failing script; replay one deterministically with
`fuzz_smoke_runner --script FILE`.

```bash
cmake -S packages/markdown-core -B build-fuzz -DMARKDOWN_CORE_FUZZ_SESSION=ON \
      -DCMAKE_C_COMPILER=$(which clang) -DCMAKE_CXX_COMPILER=$(which clang++) \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-fuzz --target fuzz_document_edits --parallel
mkdir -p build-fuzz/corpus-document
build-fuzz/fuzz/fuzz_document_edits build-fuzz/corpus-document \
    -dict=packages/markdown-core/tests/core/fuzzing_dictionary -max_len=512
```

## Quadratic fuzzer

The quadratic fuzzer generates long sequences of repeated characters, such as
`<?x<?x<?x<?x...`, to detect quadratic complexity performance issues.

To build and run the quadratic fuzzer:

```bash
mkdir build-fuzz
cd build-fuzz
cmake -DMARKDOWN_CORE_FUZZ_QUADRATIC=ON -DCMAKE_C_COMPILER=$(which clang) -DCMAKE_CXX_COMPILER=$(which clang++) -DCMAKE_BUILD_TYPE=Release ..
make
../packages/markdown-core/fuzz/fuzzloop.sh
```
