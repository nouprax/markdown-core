# O/I/P oracle drift and review provenance audit

Audit baseline: PR #229 at `b8b6e4df`, 2026-09-12. This audits the requested
Obsidian, insertion and Pandoc feature tracks, including their effects on the
CommonMark, GFM and remark comparisons. It does not expand the feature inventory
or require full Pandoc compatibility.

- [x] Replay all six pinned oracle gates and inspect every active O/I/P delta.
- [x] Retrieve 24 feature/specification PRs, 168 review threads and 221 comments,
      including both pages of #197; trace semantic follow-ups in Git.
- [x] Compare the 34 Pandoc drift cases with native JSON and the complete product
      dump, then run isolated controls for suspicious premises.
- [x] Separate expected model/grammar differences from a historical reversal,
      reader-configuration effects and unresolved semantic choices.
- [x] Add seven independent agreement witnesses and clarify misleading documents.
- [ ] Resolve the empty-caret reversal and the two citation choices below, then
      change their grammar, implementation, fixtures and delta entries together.

## Conclusion and evidence standard

The registered differences cannot all be considered verified product intent.
One historical specification reversal is confirmed: empty superscripts were
allowed, explicitly verified against Pandoc after a false review, then rejected
during a later example rewrite. P6 implemented the rewritten rule and converted
its outstanding oracle gap into a `projection` entry. The same behavior also
occurs inside a second compound delta.

Two citation choices need semantic review. Their current implementation matches
the current module, but the source behavior and the claimed transactional
fallback do not establish the chosen result. This audit found no direct GitHub
review demanding those exact changes; they must not be presented as proven
false-positive review fixes. There is also a reader-configuration effect inside
the escaped-space delta which must be separated from a real fallback difference.

For the other reviewed differences, the evidence supports the declared shared
model, feature ownership, source grammar or inherited-language rule. No additional
review-induced semantic reversal was found in the O or I tracks. This is a
bounded provenance and executable-corpus audit, not proof for all possible input
compositions or a claim that every historical design sentence was individually
approved by the user.

An exact digest proves that a difference is stable. Calling it `projection`,
updating a golden or finding matching normative prose does not prove its intent:
the prose and golden may have been changed together by the same mistaken review.

## A-1: confirmed empty-superscript reversal

The provenance chain is reproducible:

- In [#192's review](https://github.com/nouprax/markdown-core/pull/192#discussion_r3929067146),
  the reviewer asserted that Pandoc required nonempty script bodies.
- The [oracle-backed reply](https://github.com/nouprax/markdown-core/pull/192#discussion_r3929198372)
  rejected that assertion. Commit `7783adfe` retained empty bodies and added a CLI
  witness. The pre-rewrite module explicitly permitted `^^` and an unclaimed `~~`.
- In [fe03f147](https://github.com/nouprax/markdown-core/commit/fe03f147afb94a07735435ed3594a03493086a47),
  “Restate every dialect module with CommonMark-style examples”, `A body may be
  empty` became `A body is non-empty`. The conflicts file newly attributed
  rejection to Pandoc. This diff changes grammar, despite the restatement framing.
- P0 still recorded the empty-script case as a P6 gap at `8d9177fa`. P6's merged
  `0c68bd1d` changed it to `projection`, citing the rewritten module. The product
  now emits `Text("^^")`; the pinned Pandoc reader emits `Superscript([])`.

The incorrect premise demonstrably resurfaced, although the available Git history
does not prove that the author of `fe03f147` acted on that particular earlier
review comment. The error is the unsupported reversal, not an inferred motive.

Recommendation: restore the original empty-caret contract with general delimiter
and ownership coverage. Treat double tildes separately: they also participate in
the deliberately unified strikethrough/subscript rule. Restoring `^^` does not
by itself authorize changing `~~` or longer tilde runs.

Affected drift IDs: `empty-superscript-and-subscript` and the empty-caret portions
of `p6-pairing-and-tilde-runs`. The latter also includes intentional tilde ownership
and ordinary nonempty pairing; one compound waiver must not approve all of them.

## A-2: successful script escaping depends on the oracle reader

For `^a\ b^`, Pandoc 3.11 with
`markdown_strict+superscript+subscript` emits ordinary text. With
`+all_symbols_escapable`, it emits a Superscript containing `a`, NBSP and `b`,
exactly matching the product. This is now an isolated, unwaived corpus agreement.

The existing `p6-escaped-space` input combines successful scripts with an escape
outside a script and a failed script. Those latter cases remain a real chosen
difference: the product decodes escaped ASCII space only in a committed script;
the broader Pandoc reader also decodes it outside or after failed matching.
For example, `^a\ b c^` retains its backslash in the product, while that reader
produces an NBSP in its literal fallback.

Recommendation: explain the compound entry as mixed reader/fallback evidence.
Do not change the product to make successful script escapes literal, and do not
enable all of Pandoc's escape grammar globally in order to remove this delta.

## A-3/A-4: citation choices requiring semantic review

| Input | Current product | Pandoc 3.11 | Assessment |
| --- | --- | --- | --- |
| `@a [p. @b]` | One author-in-text `a`, with an author-in-text `b` nested inside its suffix | Two outer items: author-in-text `a`, then normal `b` with prefix `p.` | `p7-tail-later-author` changes citation grouping and mode. The earlier ordinary bracket-group rule does not prove this author-tail rule. Prefer the source's item structure unless a separate product reason requires nesting. |
| `[@a;]` | Entire spelling is Text | Literal brackets/semicolon around an author-in-text Cite for `a` | `p7-malformed-group` suppresses a valid inner construct after outer-group failure. The module's example prescribes this, while its “failed candidate consumes nothing” rule suggests ordinary fallback. Resolve that contract tension; ordinary fallback is the audit recommendation. |

The [#197 comment about choosing an item's key](https://github.com/nouprax/markdown-core/pull/197#discussion_r3942560375)
concerns `[@a @b]`, where native Pandoc really does nest `b` in `a`'s suffix.
That verified result cannot be generalized to the differently owned `@a [@b]`
tail. The original source contract already called a simple author tail a suffix;
later prose added semicolon-separated items without explicitly deciding the
no-semicolon key case. P7's ledger made the latter choice explicit only when
implementation exposed the difference.

For malformed groups, the pre-rewrite contract released the opener to ordinary
bracket parsing. The blanket Text example appeared in `fe03f147`; the historical
audit's PC-6 explained rejection of the *group*, not suppression of every valid
inner citation. These are real semantic review items, not missing consumer
numbering, citation rendering or Pandoc-only features.

## Review suggestions that did not cause the suspected regression

| Review | Independent check and present behavior |
| --- | --- |
| [#192: restrict GFM anchor characters](https://github.com/nouprax/markdown-core/pull/192#discussion_r3929227847) | The claimed Pandoc whitelist is incorrect for the pinned reader: connector punctuation and combining marks survive. A new isolated case agrees with the existing product. The compound emoji delta remains explained by the explicit no-emoji policy. |
| [#192: use plain heading labels](https://github.com/nouprax/markdown-core/pull/192#discussion_r3929227849) | Under `# *Foo*`, Pandoc leaves `[Foo]` literal and resolves `[*Foo*]`. Both isolated cases agree with the product. Putting the two references next to each other would confound this with Pandoc's whitespace-separated full-reference syntax. |
| [#197: simple-table relative widths](https://github.com/nouprax/markdown-core/pull/197#discussion_r3937428928) | Correctly rejected historically. Simple columns retain default/null widths. Multiline/grid widths are a separate declared source-interior model. |
| [#197: four-equals runs must be literal](https://github.com/nouprax/markdown-core/pull/197#discussion_r3942560374) | The pinned Obsidian parser and product both parse `==d====e==` as two marks. The follow-up clarified pairwise run ownership; it did not adopt the requested blanket rejection. Three run-partition controls were replayed. |
| [#214: gate inline footnotes behind an option](https://github.com/nouprax/markdown-core/pull/214#discussion_r3953508279) | The requested gate conflicts with the already chosen single dialect. The follow-up removed obsolete option assumptions. The shipped feature remains always on. |
| [#225: exclude images from virtual heading definitions](https://github.com/nouprax/markdown-core/pull/225#discussion_r3968799759) | Pandoc resolves shortcut, collapsed and full image references against a heading; the product does too. Three new agreement cases guard this behavior. The module's Link-only wording has been corrected. |
| [#226: script whitespace/context across owned fields](https://github.com/nouprax/markdown-core/pull/226#discussion_r3978323631) | These enforce the existing raw-whitespace and contextual-escape rules across owned inline content. The later shared-stack audit retained the semantics and recorded its 472-input differential; they do not justify the unrelated empty-body rule. |
| [#228: prefixed blank lines in specimen bodies](https://github.com/nouprax/markdown-core/pull/228#discussion_r3985166314) | The fix restores the shared continuation rule after ancestor prefixes. It preserves content ownership instead of adding a new source interpretation. |
| [#229: end captions at a code fence](https://github.com/nouprax/markdown-core/pull/229#discussion_r3995775248) | Replayed with the pinned table-caption/backtick reader: both sides end the caption and emit an independent CodeBlock. This review is correct. |
| [#229: end simple rows at a block marker](https://github.com/nouprax/markdown-core/pull/229#discussion_r3995894450) | Incorrect; the six existing heading/quote/fence cases with and without blanks agree with Pandoc. The code retained correct body ownership and the docs were clarified. |

O5's Unicode-column fix, O7's source-adjacency fixes, O6's final field ownership
fixes and the allocation/lifetime follow-ups were checked against their shared
invariants and changed fixtures. They do not account for an unexplained active
oracle drift. O6's discarded full-YAML implementation belongs to the documented
user-directed scope correction, not a requirement to recover YAML features.
The final O6 `authors: - Ada` follow-up prevents an array from being invented;
the product retains `- Ada` as a text scalar under its own scalar contract,
instead of adopting the review's suggestion to discard it solely because it
is invalid YAML. Balanced unsupported collections keep nested source inside
the owning field during recognition, so a nested `state` cannot steal a later
actual top-level field.

## Complete feature-owned drift inventory

“Supported” below means supported by the audited general product/source rule,
not that the native ASTs agree or that a `projection` label is accurate.

### Obsidian: eight exact differences and eight general projections

| IDs | Disposition |
| --- | --- |
| `task-prefix-supplementary-scalar`, `task-prefix-closing-bracket` | Supported O5 one-authored-Unicode-scalar grammar; the oracle's UTF-16 matcher and excluded bracket differ. |
| `task-prefix-separator-alphabet`, `task-prefix-separator-run` | Supported structural separator alphabet and whole-run consumption. |
| `task-prefix-before-first-block`, `task-prefix-opening-line` | Supported item-creation ownership, before the first block; later paragraphs cannot create a task state. |
| `task-prefix-authored-scalar` | Supported authored-scalar matching before escape decoding. |
| `embedded-dimensions` | Supported O9 typed dimensions; the oracle leaves their spelling in the alias. |
| `comment-removal`, `highlight-content-model` | Supported retained Comment nodes and parsed Mark content; canaries prevent the projection from disguising absent recognition. |
| `scope-model`, `wikilink-label-nullability`, `destination-value-model`, `image-alt-content-model` | Declared source-coordinate/value/content representations; absent versus authored-empty labels stay distinct. |
| `universal-callout-container`, `metadata-fixed-fields-and-recovery` | Explicit product domain and consumer model. The direct Markdown oracle does not implement callout metadata; YAML judges only the selected valid metadata intersection. |

O4, O7, O8 and parts of O9 intentionally rely on product conformance where this
oracle cannot express their feature. Passing its corpus does not independently
validate those parts; their contract and review changes were checked separately.

### Insertion: twelve exact composition differences

| IDs | Disposition |
| --- | --- |
| `nested-mark`, `crossed-mark` | Supported Mark/Insertion interaction in the shared delimiter stack; the insertion oracle has no Mark grammar. |
| `cross-links`, `crossed-cross-link` | Supported opaque cross-link ownership and typed dimensions. |
| `cites` | Supported document-owned footnotes and independent inline containers. |
| `comments`, `comment-block` | Supported retained/opaque comments; the oracle has no percent-comment parser. |
| `formulas`, `formula-block` | Supported formula opacity; the oracle has no formula parser. |
| `autolinks` | Supported GFM bare URLs; the oracle deliberately has linkification disabled. |
| `inline-fields` | Supported callout titles, directive labels and inline-footnote ownership. |
| `failed-candidates` | Supported fallback exposes existing Mark/formula constructs, absent in the insertion oracle. |

All upstream insertion fixtures and all module examples agree. The 13.0.2 to
14.2.0 oracle update preserved the original 54 results and all 12 delta digests;
its three new Unicode controls agree with the existing product. The historical
O2 delimiter-depth limit was already traced to a leftover worklist capacity and
removed during I1; no parser nesting restriction was retained.

### Pandoc: all 34 exact entries

| # | ID | Audit disposition |
| --- | --- | --- |
| 1 | `empty-superscript-and-subscript` | A-1: confirmed historical reversal for empty carets; split from tilde ownership. |
| 2 | `pandoc-reference-attribute-merge` | Supported preservation of inherited then occurrence declarations, including duplicates; consumer chooses duplicate resolution. The review changed a deduplicating merge deliberately to satisfy that ownership rule. |
| 3 | `multiline-table` | Supported declared interior-width ratios; content agrees. |
| 4 | `grid-table-block-cells` | Same width rule; block content agrees. |
| 5 | `grid-table-row-and-column-spans` | Same width rule; sparse spans/content agree. |
| 6 | `example-lists-and-reference` | Supported document-owned Specimens and raw references; numbering stays with consumers. |
| 7 | `p6-escaped-space` | A-2: mixed reader configuration and contextual-fallback difference; not uniform semantic drift. |
| 8 | `p6-whitespace-recovery` | Supported Unicode whitespace boundary. ASCII recovery agrees; the actual differing component is the EM SPACE body. |
| 9 | `p6-entity-space` | Supported decoded TAB preservation; Pandoc normalizes the TAB to space. |
| 10 | `p6-pairing-and-tilde-runs` | Mixed: A-1 empty carets plus declared maximal tilde ownership. Nonempty caret pairing itself agrees. |
| 11 | `p6-opaque-tokens` | Supported code-token ownership; the product does not inspect raw code whitespace as an outer script delimiter event. |
| 12 | `anchor-global-reservation` | Supported document-wide explicit-anchor reservation before generation. |
| 13 | `anchor-permitted-scalars` | Supported no-emoji-alias policy; connector punctuation is retained by both sides. |
| 14 | `anchor-simple-lowercase` | Supported explicit Unicode simple-lowercase rule. |
| 15 | `anchor-whitespace-scalars` | Supported explicit Unicode White_Space mapping. |
| 16 | `anchor-inline-code-reservation` | Same global reservation on another anchor producer. |
| 17 | `implicit-reference-adjacency` | Supported inherited CommonMark tail adjacency. |
| 18 | `p5-heading-span-reservation` | Same global reservation on Span. |
| 19 | `p7-tail-and-nesting` | Supported explicit affix trimming; nested owners are retained. |
| 20 | `p7-tail-later-author` | A-3: review author-tail item grouping/mode independently of ordinary bracket-group nesting. |
| 21 | `p7-malformed-group` | A-4: review literal fallback versus valid inner citation preservation. |
| 22 | `p7-heading-projection` | Follows the declared projection of stored, trimmed affixes and keys. It is a semantic generated-anchor difference, not a representation-only projection. |
| 23 | `p7-unicode-boundary` | Supported explicit word-boundary rule includes underscore. |
| 24 | `p9a-nested-start` | Both reject the non-one nested marker; the actual difference is Pandoc's paragraph split versus inherited lazy continuation. Isolated blank-separated/empty-parent controls agree. |
| 25 | `p9b-resolution` | Supported specimen ownership and consumer numbering. |
| 26 | `p9b-heading` | Same specimen model; heading text projects the stored reference ID. |
| 27 | `p7-unresolved-reference-tail` | Supported ordered bracket alternatives: only a resolving link tail wins. |
| 28 | `p7-explicit-citation-shortcut` | Supported inherited reference-definition grammar and complete-Cite-before-shortcut precedence. |
| 29 | `p9a-start-always-authored` | Deliberate negative reader control without `startnum`; normal selected-feature mapping includes it. No product change is indicated. |
| 30 | `p8-closer-width` | Explicit C-7 ruling: named and nameless directives use the same closer width rule. |
| 31 | `p8-explicit-anchor-reservation` | Same document-wide anchor reservation on a nameless directive. |
| 32 | `p10-nested-and-lazy` | Only authored compactness differs where Pandoc has no Plain/Para flag to recover. |
| 33 | `p10-padding-and-tabs` | Same compactness representation gap for code bodies; body bytes agree. |
| 34 | `p10-empty-bodies` | Same compactness representation gap for empty bodies; body grouping agrees. |

### Inherited oracle effects

The full CommonMark/GFM/remark ledgers also replayed: 22, 28 and 96 exact
differences respectively. Relative to the pre-O1 `1ad40aed` snapshot, the O/I/P
changes have the following explained sources; pre-existing base-engine defects
and earlier M-stage model decisions remain outside the new feature audit.

| Effect | Evidence/owner |
| --- | --- |
| Cross-link brackets inside inherited paragraphs/directives | O1 opaque scanner precedence. |
| Leading metadata envelopes | O6 explicitly selected front matter and member recovery. |
| Image dimension suffixes | O9 typed source facts; oracle alt strings retain the suffix. |
| Task prefixes, block ownership and Unicode/tab columns | O5 grammar and shared cursor invariant. |
| Shared attributes and P2 attachment sites | C-8's one Pandoc attribute grammar; remark owns no competing member language. |
| Single tildes formerly recognized as strikeout | Explicit C-1 subscript ruling. |
| Cite spellings previously treated as text/autolinks | P7 recognition and inherited priority. |
| Non-one nested ordered starts | P9a's adopted nested-list restriction, independently reproduced with Pandoc. |
| Span after a failed directive; nameless containers | P5/P8 ordered fallback and C-7 ownership. |
| Quoted directive lazy continuation | Pre-existing inherited parser behavior, exposed and recorded by P8; not introduced by its review fixes. |
| Caption before an inline directive | P11 caption ownership; remark has no table captions. |

## Verification and reproducibility

The six gate entry points in `package.json` were run directly with Node using
the already installed, pinned tools. Their version checks, native canaries and
exact-digest enforcement were retained. CommonMark, GFM, remark, Obsidian,
insertion and Pandoc all passed, as did the oracle harness unit tests.

The initial Pandoc replay was 92 inputs / 34 differences. Seven new independent
agreement cases make the active corpus 99 inputs / 65 agreements / 34 differences
and zero feature gaps. No waiver or recorded output digest was refreshed to
make a suspicious case pass. Runtime parser behavior is unchanged by this audit.

The empty-caret result can be reproduced after installing the pinned oracle:

```sh
printf '^^\n' | .tools/pandoc/3.11/pandoc --from=markdown_strict+superscript+subscript --to=json
printf '^^\n' | build/cmake/packages/markdown-core/core/markdown-core
git show fe03f147 -- docs/specs/dialect/superscript-and-subscript.md docs/specs/dialect/conflicts.md
```

The permanent corpus contains the image/label/Unicode/escape controls. The
existing corpus holds the exact inputs for every numbered drift. The local
audit capture at `/private/tmp/oip-oracle-audit/` additionally retains all
retrieved reviews, 34 native/projection comparisons, 19 isolated Pandoc probes,
three Obsidian run-partition probes and the gate logs. That capture is diagnostic
evidence; the repository's product fixtures retain their existing ownership.
