# Swift StoredMarkup storage evaluation — 2026-09-13

The metadata-only boxing policy is retained as a balance between memory use
and destruction cost. It reduces the array space required by all-direct
storage while retaining faster release than all-indirect storage. The measured
tradeoffs support this choice; they do not establish a universal optimum.

StoredMarkup groups cases into nodes with owned Markup relations, inline leaf
nodes, and boxed leaf nodes. Metadata occupies the boxed group and remains a
leaf semantically. Each group's cases and the corresponding projections use
the same alphabetical order.

Among the three tested layouts, none wins every metric. Boxing all cases
reduces retained heap use in every tested workload, while metadata-only boxing
releases trees faster. The differences in total time are mostly small and do
not establish a consistent throughput winner. This comparison is not a search
over every possible storage architecture.

## Method

- macOS arm64; Apple Swift 6.3.3. Each variant uses `-O`, whole-module
  optimization, Swift 6, and the same source snapshot. Only the placement of
  `indirect` in `StoredMarkup` differs. Swift modules and benchmark consumers
  are compiled separately, with identical flags; all variants link the same
  Release C object files built by `swift build -c release --target MarkdownCore`.
- All-direct has no indirect cases; metadata-only is the production layout;
  all-indirect marks the entire enum indirect. Record strides are respectively
  288, 136, and 8 bytes. These are array slot sizes, not complete tree footprints.
- Three rounds, rotating the variant order for each workload and shuffling
  workload order between rounds. Each fresh process performs two warmups and
  five measured samples: 15 measured samples per workload and layout.
- Parsing keeps the whole batch alive. Each sample then walks the retained
  documents three times through the public visitor, consuming source positions
  into an observable checksum, and measures final release separately. Node counts,
  entering/exiting event counts, and checksums match across all variants.
- Retained heap is the increase in `malloc_zone_statistics(NULL, ...)`
  `size_in_use` between the pre-parse and retained-document boundaries. It includes
  live boxes, array capacities, strings, relations, and store allocations, but
  excludes input strings loaded before measurement. It measures live allocator
  bytes, not resident pages or total allocation traffic. Heap block counts are
  live counts, not counts of all allocations made during parsing.
- Timings below are medians. Total means parse + one walk + final release,
  computed within each sample before taking the median. It is one specific
  lifecycle, not a weighting suitable for every consumer. No CPU affinity or
  frequency control was applied. The 32k-depth case in particular had substantial
  process-to-process timing variation (roughly 50–72 ms); small timing differences
  should not be treated as stable wins.

## Inputs

| Workload | Batch |
| --- | --- |
| empty | 4,000 empty documents |
| small | 2,000 short paragraphs with emphasis and a link |
| metadata | 4,000 metadata-only documents, all ten fields present |
| canonical | All 36 shared canonical AST fixtures, repeated 12 times |
| wide-4k | Two documents, each with 4,096 plain paragraphs |
| wide-16k | One document with 16,384 plain paragraphs |
| metadata-wide | wide-16k preceded by all ten metadata fields |
| mixed | 2,000 repetitions of a heading, rich paragraph, quote and two list items |
| table | Four-column table with 4,096 body rows, including emphasis and links |
| deep-8k | List nesting depth 8,192 |
| deep-32k | List nesting depth 32,768 |

## Retained heap

MiB includes the complete retained batch, not merely the `records` buffer.

| Workload | Nodes | All direct | Metadata only | All indirect | All-indirect reduction vs current |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 4,000 | 1.405 | 0.917 | 0.795 | 13.3% |
| small | 18,000 | 10.438 | 3.114 | 2.503 | 19.6% |
| metadata | 8,000 | 3.480 | 3.480 | 2.870 | 17.5% |
| canonical | 15,936 | 7.472 | 3.542 | 2.103 | 40.6% |
| wide-4k | 16,386 | 6.469 | 3.469 | 1.907 | 45.0% |
| wide-16k | 32,769 | 12.938 | 6.938 | 3.813 | 45.0% |
| metadata-wide | 32,770 | 12.938 | 6.938 | 3.813 | 45.0% |
| mixed | 56,001 | 25.498 | 13.498 | 6.917 | 48.8% |
| table | 45,067 | 25.422 | 7.422 | 5.673 | 23.6% |
| deep-8k | 16,387 | 6.750 | 3.750 | 2.438 | 35.0% |
| deep-32k | 65,539 | 27.000 | 15.000 | 9.750 | 35.0% |

All-indirect saves space despite having more live allocations. A direct enum
reserves the largest payload size for every array element; most payloads are
smaller than that maximum. Array spare capacity also reserves full-sized slots.
Indirect slots are pointers, and each box contains only its own case's payload.
For wide-16k, the current layout retains 16,388 heap blocks; all-indirect retains
49,157, exactly 32,769 more—one for each node—while still using 45% fewer live
heap bytes. Allocation count alone was therefore insufficient to choose the
layout.

## Total time

Milliseconds for parse + one walk + release of the complete batch.

| Workload | All direct | Metadata only | All indirect |
| --- | ---: | ---: | ---: |
| empty | 4.697 | 4.661 | 4.826 |
| small | 11.159 | 10.791 | 10.808 |
| metadata | 13.510 | 13.823 | 13.515 |
| canonical | 10.552 | 10.150 | 10.053 |
| wide-4k | 7.737 | 7.583 | 7.755 |
| wide-16k | 15.574 | 15.338 | 15.342 |
| metadata-wide | 15.542 | 15.539 | 15.184 |
| mixed | 26.868 | 26.083 | 25.825 |
| table | 18.856 | 18.086 | 18.380 |
| deep-8k | 7.157 | 7.215 | 6.949 |
| deep-32k | 51.520 | 51.500 | 52.203 |

## Phase details

Phase medians in milliseconds. Summing phase medians need not equal the median
total reported above.

| Workload | Layout | Parse | One walk | Release |
| --- | --- | ---: | ---: | ---: |
| empty | all-direct | 4.258 | 0.287 | 0.142 |
| empty | metadata-only | 4.315 | 0.207 | 0.144 |
| empty | all-indirect | 4.208 | 0.417 | 0.193 |
| metadata | all-direct | 12.004 | 1.108 | 0.415 |
| metadata | metadata-only | 12.363 | 1.016 | 0.424 |
| metadata | all-indirect | 11.833 | 1.227 | 0.466 |
| canonical | all-direct | 7.841 | 2.300 | 0.360 |
| canonical | metadata-only | 7.764 | 2.031 | 0.328 |
| canonical | all-indirect | 7.594 | 1.998 | 0.459 |
| wide-16k | all-direct | 10.780 | 4.425 | 0.442 |
| wide-16k | metadata-only | 10.863 | 4.061 | 0.430 |
| wide-16k | all-indirect | 10.702 | 3.932 | 0.729 |
| mixed | all-direct | 17.704 | 8.185 | 0.957 |
| mixed | metadata-only | 17.753 | 7.428 | 0.921 |
| mixed | all-indirect | 17.260 | 7.125 | 1.321 |
| table | all-direct | 12.881 | 5.207 | 0.717 |
| table | metadata-only | 12.809 | 4.657 | 0.679 |
| table | all-indirect | 12.922 | 4.366 | 1.086 |
| deep-32k | all-direct | 44.531 | 5.666 | 1.315 |
| deep-32k | metadata-only | 45.648 | 4.615 | 1.343 |
| deep-32k | all-indirect | 46.164 | 4.257 | 1.958 |

All-indirect release takes about 10–70% longer than the current layout across
these workloads. Empty-document traversal is also slower (0.417 vs 0.207 ms per
4,000-document batch), and metadata-only document traversal takes 1.227 vs
1.016 ms per 4,000-document batch. Several larger trees instead walk faster with
all-indirect. These are meaningful tradeoffs, not evidence for one universal
fastest choice.

## Semantic validation

Every optimized variant independently passed:

- All 36 shared canonical dumps, covering all 43 Markup kinds, byte-for-byte.
- Retention of a subtree after dropping its document at nesting depth 65,536.
- Complete entering/exiting traversal of the retained subtree.
- Normal final release, verified by a weak reference to the store becoming nil.

The timed workloads also assert equal record counts, visitor event counts, and
checksums across layouts. There was no production code change requiring a new
regression test.

## Local reproduction artifacts

The generated drivers, exact input files, optimized binaries, raw samples and
summaries are in the ignored `build/swift-storage-evaluation/` directory. From
the repository root, this evaluation was produced with:

```sh
swift build -c release --target MarkdownCore
python3 build/swift-storage-evaluation/prepare.py
python3 build/swift-storage-evaluation/run.py
python3 build/swift-storage-evaluation/validate.py
```

`prepare.py` makes isolated Swift source variants without modifying production
files. `results.json` retains every sample; `summary.json` contains phase and
heap medians; `validation.log` records the correctness checks. These local
artifacts are not added to the package or its CI benchmark surface.
