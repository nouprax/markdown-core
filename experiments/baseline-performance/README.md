# Baseline performance fixes: same-machine replay

This experiment compares baseline `5eca3bc1` with the working-tree fixes for
issues #243, #248 and #272. It measures public C **parse + free** with one warmup,
seven raw samples and alternating lane order. It records source and library
SHA-256, compiler, platform, medians and throughput. Generation, canonical dumps
and counting allocators are outside timed samples.

Build the baseline in a separate checkout at the pinned revision, using
`cmake --preset benchmark` and `cmake --build --preset benchmark --parallel 8`.
Build the candidate with those same commands in this checkout, then run:

```sh
python3 experiments/baseline-performance/run.py \
    --baseline-library /absolute/path/to/baseline/build/benchmark/packages/markdown-core/elements/libmarkdown-core.3.0.0.dylib \
    --candidate-build build/benchmark \
    --output build/baseline-performance-results.json
```

On Linux supply the corresponding baseline `.so` instead. The Python driver
uses the current [incremental experiment probe](../incremental/probe.c) to
regenerate baseline collision labels, without any downloaded dependencies.
Run timing without concurrent builds or sanitizer tests. Tiny tracked samples
are useful for semantic comparisons but their individual timings are noisy;
these numbers are not universal CI thresholds.

The saved [results.json](results.json) is a local macOS arm64 Release run.
The candidate's production C changes are the radix index, shared failed
separator recognition, retained lookahead indentation, and last-whitespace
boundary scan. The earlier incremental experiment's `results.json` remains the
original pre-fix evidence.

Every non-deep case compares full canonical output with the baseline, including
all four index consumers and the 26 un-repeated tracked samples. Deep dump output
itself has quadratic size; those cases use native semantic/complexity tests
instead of materializing the dump. A timing result does not establish an
asymptotic bound: the API tests check the radix structure and counted parser work.
The tool does not implement the separate, broader phase/binding/CI benchmark
requirements in #270 and #274.
