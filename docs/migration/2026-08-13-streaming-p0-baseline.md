# Streaming P0 baseline: today's per-tick cost, measured on the engine streaming starts from

**Date:** 2026-08-13 · **Branch:** streaming-p0, after R1–R9 of
[the streaming plan](../reviews/2026-08-12-streaming-plan.md) · **Workload:**
`benchmark_append_baseline` (`bench_runner --workload append_baseline`)

## What these numbers are

The per-tick cost of consuming a stream **today**: every tick hands the whole
bytes-so-far to `edit`, so a tick costs one full parse plus one whole-tree
diff of the prefix. The workload drives the real `edit()` path with
token-sized, non-line-aligned strides (3–8 bytes) in bursts of five (after
one unrecorded warmup tick) at each doubling checkpoint, per shape from the
plan's list; the burst median is reported. These are the numbers the streaming engine's D6 fallback may never
exceed and its warm path is built to beat. They are measured after the P0
removals (delta emission, ref_size budget, CLEAN_START, the definition
archive), so they describe the exact engine P1 starts from.

Absolute values below are one machine's; the CTest gate asserts only the
MEDIAN of the adjacent-doubling growth ratios (≤ 4.0×), never wall-clock —
an isolated allocator size-class transition moves one interval's ratio
without moving the median, while sustained super-linear growth moves every
interval (the same median form the footnote-renumber complexity gate uses,
for the same reason).

## Machine

Apple M5 Max, 18 cores, 128 GiB, macOS 26.6.1, Apple clang 21.0.0,
Release build, arena pooling on (no sanitizer — the gate skips timing under
sanitizers by construction, since benchmark-label tests are excluded there).

## Per-tick medians (ms), by shape × prefix size

| Shape | 256 KiB | 512 KiB | 1 MiB | 2 MiB | 4 MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| prose | 1.134 | 1.990 | 4.278 | 8.373 | 17.324 |
| nested_list | 5.376 | 13.279 | 31.897 | 69.925 | 138.439 |
| fence (unclosed, growing) | 0.156 | 0.308 | 0.607 | 1.238 | 3.111 |
| footnote_dense (distinct labels) | 3.725 | 9.785 | 22.508 | 56.224 | 144.289 |
| giant_paragraph (no blank line) | 0.836 | 1.745 | 3.852 | 9.665 | 23.438 |
| references_appendix (no blank line) | 3.766 | 9.636 | 31.864 | 90.722 | 193.239 |
| mixed_edit (mid-splice tick over prose) | 0.894 | 1.833 | 3.855 | 7.953 | 16.846 |

Peak RSS over the whole workload: ~656 MiB (`append_baseline
peak_rss_kib=671392`), dominated by the per-tick predecessor+successor pair
at the 4 MiB checkpoints.

## Readings

- **Every tick scales with document size** — the defining property this
  baseline exists to record. A 100 ms-throttled desktop consumer at 256 KiB
  prose pays ~1 ms/tick and notices nothing; at 1 MiB dense (nested_list,
  footnote_dense, references_appendix) a tick is 22–32 ms, which saturates a
  core at 30–45 ticks/s and is already past mobile frame budgets — the
  plan's honest-justification thresholds, reproduced by measurement.
- **mixed_edit ≡ prose today** (same full parse); the skeleton keeps it as a
  separate shape because the two paths diverge completely once streaming
  lands — `edit` stays a full parse by design (D2), while append ticks drop
  to O(frontier).
- **fence is the cheapest full parse**, ~0.6 ms at 1 MiB; the warm path's
  target for this shape is memcpy speed, so it is also where streaming's
  relative win is largest on paper and least *needed* in practice.
- **footnote_dense and references_appendix grow super-linearly within the
  bound** (last-step ratios 2.57× and 2.13×): definition-table pressure at
  ~16 K distinct labels per MiB. These are the shapes the plan's degradation
  ladder names; their per-tick cost stays in the full-parse class under
  streaming too (flip storms, harvest re-consumption), which is why the
  ladder exists.

## How to reproduce

```sh
cmake --preset default -DMARKDOWN_CORE_TESTS=ON
cmake --build --preset default --parallel
ctest --test-dir build/cmake -R benchmark_append_baseline --output-on-failure
```

The workload is deterministic (fixed PRNG seed per shape, synthesized
inputs); differences between machines are scale, not shape.
