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
| mixed_edit (mid-splice tick over prose) | 0.881 | 1.782 | 4.022 | 8.005 | 17.251 |

Peak RSS over the whole workload: ~642 MiB (`append_baseline
peak_rss_kib=657280`), dominated by the per-tick predecessor+successor pair
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

## L1 slice 8: the same shapes on the living tree (2026-08-16)

**Branch:** `living-tree-l1`, after slice 7 (the warm tick) · **Workloads:**
`benchmark_append_baseline` and the new `benchmark_append_amortized` ·
same machine, same build.

The append is now one of two ticks, decided per chunk before anything is
written: WARM when the eligibility predicate lets the head's tree grow in
place (prose shapes: paragraph continuation, blank lines, new paragraphs
and headings), REBUILT — today's full parse plus diff — for every other
shape until the journal is total. Warm shapes are measured as the median
over eight bursts of sixteen ticks each (a warm tick sits below what
`CLOCK_MONOTONIC` resolves on macOS); rebuilt shapes as before.

| Shape | tick class | 256 KiB | 512 KiB | 1 MiB | 2 MiB | 4 MiB | doubling ratio (bound) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| prose | warm, bounded leaf | 0.00063 | 0.00056 | 0.00056 | 0.00069 | 0.00056 | 1.00 (≤ 1.5, flat) |
| giant_paragraph | warm, leaf = document | 0.838 | 1.745 | 3.828 | 8.600 | 20.393 | 2.25 (≤ 4, linear) |
| nested_list | rebuilt | 5.117 | 12.273 | 30.733 | 65.613 | 134.456 | 2.40 (≤ 4) |
| fence (unclosed) | rebuilt | 0.172 | 0.342 | 0.659 | 1.277 | 3.134 | 1.99 (≤ 4) |
| footnote_dense | rebuilt | 4.198 | 8.378 | 20.630 | 51.187 | 127.493 | 2.48 (≤ 4) |
| references_appendix | rebuilt | 3.404 | 8.494 | 21.952 | 56.854 | 152.842 | 2.59 (≤ 4) |

Per-tick medians in ms. Prose: **~0.56 µs per tick at every size**, against
4.278 ms at 1 MiB in the P0 baseline above — the tick no longer sees the
document. The giant paragraph is the honest ladder's prose wall: the open
leaf is the whole document and is re-refined, retired (owning its bytes) and
re-diffed every tick, so it stays linear at roughly the P0 figure. Every
rebuilt shape is within noise of P0, as it must be: that path did not change.

**The amortized bound, executable** (`append_amortized`): stream N bytes from
an empty document in 3–8 byte pieces and time the whole stream, T(N); parse
the same bytes once, P(N); K = T/P is the price of streaming in units of one
parse, and the living-tree plan's §1 says K must be flat across doublings.

| Shape | 32 KiB | 64 KiB | 128 KiB | 256 KiB | K doubling ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| prose (gated ≤ 1.5) | K = 29.7 | 27.6 | 28.7 | 26.1 | 0.93 |
| nested_list (record only, at 8–64 KiB) | K = 835 | 1682 | 3430 | 6406 | ~2.0 |

Prose streams at token granularity for **~27 full parses' worth of work,
whatever the size** — 47,596 ticks over 256 KiB in 21.7 ms against a
0.83 ms one-shot parse. Building the gate found the O(document) term the
plan warned of, in the one place it hid: restamping the root's subtree hash
folded over every top-level block per tick, and K doubled with N until each
spine block was restamped from a carried prefix fold of its settled
children. The rebuilding shape's K doubles per doubling, as a full parse per
tick must, and is printed unbounded until the journal makes it warm.
