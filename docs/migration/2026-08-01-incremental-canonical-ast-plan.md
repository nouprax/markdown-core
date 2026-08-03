# Incremental canonical AST: delivery plan

Status: planning (2026-08-01; milestones resynced 2026-08-03). The design is
frozen in
[`../specs/incremental-canonical-ast.md`](../specs/incremental-canonical-ast.md)
and is not restated here; this document records how that contract gets built,
in what order, what each step must prove before the next starts, and how the
unit-test coverage gate landed on 2026-08-01 burns down alongside it.

The contract moved three times after this plan was first written — the
reference-model unification, the unified-CST ownership model, and the review
fixes on top of it. Every milestone below has been re-read against the current
text rather than left to be reconciled during implementation, because two of
the changes invalidate acceptance criteria this plan previously stated: M4's
worked example proved nothing, and M3's structure would not have met its own
bound. Where a milestone's definition changed, the reason is stated in it.

The implementation language is the existing C core. Every binding is a
consumer of the C surface, so the C header is the pivot on which all four
adopt.

## The constraint that shapes the whole plan

Section 1 of the contract forbids shipping aliases, compatibility projections,
or parallel old/new update surfaces, and requires `canonical-ast.md`, the
session contract, public headers, bindings, fixtures, and examples to adopt
atomically. A package cannot advertise the capability while still exposing the
superseded four-array delta.

That is a constraint on what is *released*, not on how the work is staged. So
the plan builds the new model as internal subsystems with their own unit
tests, keeps the current public surface working the whole time, and flips the
public API exactly once, in M7. Nothing before M7 changes a public type, and
nothing after M7 leaves a compatibility path behind.

The practical consequence: M1–M6 are individually reviewable and individually
mergeable, and the repository stays green throughout. M7 is by nature one
large change, which is why every subsystem it depends on must already be at
100% coverage before it starts.

## Distance from today

The current implementation is the contract's *superseded* column, not a
partial version of its target column. Nothing in the table below is a rename.

| Contract | Today | Nature of the work |
| --- | --- | --- |
| §0 one unified CST, region-relative concrete records, `Document.concrete` | No concrete layer at all: tokens are consumed, not retained | New substrate, threaded through the existing passes |
| §2 `Commit{document, delta}`; §4.2 self-contained immutable `Document` | Session-borrowed view, invalid after the next commit | Storage-layer rewrite |
| §4 `MarkupRevision{self, subtree}` | One scalar `revision` with the subtree meaning | New per-node local stamp |
| §5.1–5.2 `DocumentVersion`/`MarkupID` as `(domain, ordinal)` | `lineage` and `node_id` as bare `uint64_t` | Type restructure, positive-only |
| §5.2 anchored continuity: positional witnesses outside the edit, content LCS inside | Best-effort adoption by kind and position | Rule is now pinned; the matcher must become a pure function of (old children, new children, normalized edit) |
| §6.1 `CanonicalText` + `TextMap` | Bare string views, no source correspondence | New subsystem |
| §6.2 persistent child sequence | Plain linked list | Persistent sequence |
| §6.3 parser answers owned by the document | Live session footnote index | Ownership move + inverted index |
| §7.1 `Source{profile, content}`, `PERMISSIVE_BYTES` | Session-held text, UTF-8 only | New |
| §7.2 `SourceExtent`, `O(log n)` `Document.scope`, five coordinate profiles | Precomputed line/column plus a whole-document scope table | Order-maintenance aggregate tree |
| §9 `Delta{before, after, diffs, edits}`, postorder, six-flag parts | Four disjoint arrays plus a separate ordered-entry API | Rewrite, and delete the old surface |
| §11.1 the ownership region as a defined unit | Stale-range reparse with no named boundary | Classification exists; regions must become the unit concrete offsets are relative to |
| §14 acceptance gates (about 60) | Partial: the equivalence runner | Large test build-out |

## Milestones

Each milestone states what it delivers, what proves it, and which coverage
ledger entries it is expected to clear. A milestone is done when its gates pass
**and** its files are off the ledger — new code is at 100% by the gate's first
rule, so only pre-existing files need clearing.

### The rule every milestone is held to

**For each rule a milestone adds, name the gate that fails when it is
violated.** If no gate fails, the gate is missing, and adding it is part of
the milestone rather than follow-up.

This is stated because it is the failure mode this work has actually produced,
three times across two review rounds, always in the same shape: the prose
asserted something stronger than the check verified, so the check passed and
the wrong text shipped. The contract said "exactly one `CanonicalText` per
kind" while the check tested "at most one"; it classified every kind into an
ownership region and the check confirmed the partition was complete but never
that a class matched the inventory's own content category; it introduced the
asymmetric `STRICT_UTF8` boundary with no gate on either side of it. Each was
caught by review rather than by a test, which is the expensive way.

The corollary for implementation: a gate that cannot fail is not evidence.
When a gate is added, run it once against the pre-fix input and confirm it
fails there.

### M0 — Quality gate (delivered 2026-08-01)

The unit-test coverage gate is in place across all four platforms:
`specs/coverage/policy.json`, `scripts/check-coverage.mjs`, four producers, a
`Coverage - Ready` CI aggregate, and the contract text in
`../specs/test-architecture.md#51-单元测试覆盖率门禁`.

Baseline recorded from the macOS toolchain:

The gate's purpose is narrow and it decides everything else about its shape:
**prove that this rewrite does not change `source -> canonical AST` output.**
It is not a code-quality score, and it must never be used to record current
behaviour as acceptable.

So the C producer runs only the suites that *assert parse output* — `spec`,
`extensions`, `regression`, `conformance`, `equivalence`, `pathological`. A
covered branch is one whose behaviour a golden assertion would catch changing;
an uncovered branch is behaviour M7 can alter with nothing failing. Suites that
execute the parser without asserting its output (`api`, `facade`, `consumer`,
`fuzz`, `packaging`) are excluded: they raise the number and protect nothing.

That is why the C figure below is far lower than a conventional coverage
report would show. It is the honest one.

| Platform | Measured files | Lines | Functions | Branches | Files short of 100% |
| --- | --- | --- | --- | --- | --- |
| `c-host` (behaviour-pinning) | 42 | 65.03% | 91.26% | 48.83% | 40 |
| `kotlin-jvm` (full suite) | 40 | 85.84% | 79.72% | 70.22% | 37 |
| `es-node` (full suite) | 15 | 92.78% | 93.68% | 82.50% | 11 |
| `swift-macos` (full suite) | 36 | 93.21% | 88.80% | not produced | 12 |

**Slightly over half of the parser's branch behaviour is unpinned.** The
largest unprotected surfaces are `core/scanners.c` (18.49% pinned, 6,190
unpinned branches), `core/case_fold_switch.inc` (50.61%),
`extensions/directive.c` (54.13%), and `core/utf8.c` (62.26%).

Generated sources are measured, not exempted. An earlier revision of this gate
exempted the re2c scanners and the case-fold table on the grounds that they are
generated; that was wrong for this purpose. Nobody reviews generated code, so
its behaviour needs pinning more than hand-written code, not less — and
`scanners.c` alone is a third of the entire unpinned surface.

The three binding platforms still run their full suites rather than a
pinning selection, so their numbers are not comparable with `c-host`. The
`source -> AST` truth lives in the C core and the bindings re-expose it;
aligning them is follow-up work.

One property the gate had to be built around: the C `facade` label is excluded
from the coverage graph. Those are the concurrency suites, and which code paths
a thread takes there is the scheduler's decision — two identical runs differ by
a few branches across a dozen files, which would make a required gate flake.
They remain required correctness gates under the ordinary presets; what the
exclusion loses is attribution, not testing.

The branch numbers above are the corrected ones. `llvm-cov`'s per-file summary
charges every branch of a function to the file that function *starts* in, so
code the preprocessor splices into a function body is recorded against its
host. That is not a rounding error here: it charged `core/utf8.c`, a 301-line
source, with the 2,804 branches of the 4,327-line generated case-fold table it
includes — 1,361 uncovered branches on a file no test of that file can reach,
while the table itself reported 0/0 and looked fully covered.

The producers therefore export the full report rather than `-summary-only`,
and `scripts/lib/coverage.mjs` rebuilds branch counters from the
function-level records, charging each branch to the file its own `fileId`
names. The gate refuses a `-summary-only` report outright, because that report
is not merely less detailed — it is wrong in a direction that flatters the
result. With attribution corrected, `core/utf8.c` carries 187 uncovered
branches rather than 1,548, `case_fold_switch.inc` is measured as itself and
exempted beside the re2c scanners, and C branch coverage reads 78.75% instead
of 72.40%. Two headers of inline helpers (`core/houdini.h`,
`extensions/ext_scanners.h`) appear as measured files for the first time.

Two follow-ups this milestone deliberately left open, recorded here rather
than in the ledger because they are infrastructure, not test gaps:

- The measured-file floors were recorded on macOS. The first Linux-hosted
  `kotlin-jvm` and `es-node` runs must confirm them; a genuine platform
  difference is a deliberate one-number edit, not a reason to remove the floor.
- `swift-macos` gates on lines and functions only, because llvm-cov emits no
  branch regions for Swift. That is declared in `unsupportedMetrics`, and the
  gate fails if branch data ever appears, so the declaration cannot outlive the
  toolchain limitation that justifies it.

Adding Kover for the Kotlin platform was attempted first and abandoned: it
refuses to configure against the AGP Kotlin-multiplatform Android target this
module declares, because it looks for the legacy `android` extension that
target does not create. Gradle's built-in JaCoCo plugin scoped to the JVM
target replaces it. That change added four trusted keys to
`gradle/verification-metadata.xml` — `org.jacoco`, `org.ow2`, `com.fasterxml`,
and a group-wide `org.jetbrains` entry that widens what was previously a
name-and-version-pinned trust. The widening is worth a second reviewer's
attention on the landing PR.

### M1 — Incremental-path coverage

This milestone was first written as "fault injection and the branch-coverage
floor", on the assumption that most of the C branch debt was allocation-failure
handling no current test could reach. Measuring it disproved that, and the
milestone is re-scoped rather than kept:

- Fault injection already exists. `tests/runners/fallback_runner.c` carries an
  n-th-allocation sweep over the parser path (`regression_fallback_oom_sweep`)
  and three over the session path (`regression_fallback_session_oom_sweep`,
  `_delta`, `_pooled`), all of them in the coverage graph. They already assert
  what §12 asks of a failed commit: the session stays at its previous revision
  with its previous view, and a retry converges on the control result,
  footnote index included. What §16.5 still wants is the *binding* half, which
  belongs to M8 where the binding consumers are built.
- The remaining debt is ordinary functional debt. In `extensions/incremental.c`
  the uncovered code is the definition-index splice — `line_compare` and
  `def_index_compare` are never called at all, and the prefix/suffix entry
  paths are unreached — not the allocation guards around it.

So M1 is: reach the incremental reparse paths the current corpus never drives.
Definition-index splicing, sentinel lines, and tab expansion on the incremental
path were the named targets.

Delivered: `equivalence_definition_cluster_edits`, a scripted replay over a
population of distinct link reference definitions — insertion between adjacent
definitions, definition-only paragraphs that vanish from the tree, a
tab-indented continuation, and a cluster collapse. Every assertion is the
replay harness's fresh-parse equivalence, so the case survives M7 intact.

**Its coverage yield was two branches, and the reason is the finding this
milestone actually produced.** Three paths in `extensions/incremental.c` are
not reachable from any input the suite can construct:

| Path | Condition | Observed |
| --- | --- | --- |
| `line_compare` | ≥2 distinct vanished-clean sentinel lines | never called |
| `def_index_compare`, the `filled > 1` sort | ≥2 entries in the parse's reference map | `filled > 1` never true in 25.5k calls |
| the `renumber` prefix/suffix copy | vacated order span narrower than the staged definition count | never true in 30.9k evaluations |

Instrumented probes put the cause under the first two: `parser->refmap` holds
no entries at any point `commit_full` observes it — 0 across all 15 full
reparses of `link_ref_edits`, whose document carries several definitions that
resolve correctly. Across the whole suite it never holds more than one, and the
few non-empty cases come from `complexity_runner`'s `session_def_spread` and
`session_ref_retarget`, which the coverage preset excludes for timing
flakiness. Running those two under instrumentation directly does not cover the
three paths either, so the `complexity` exclusion is not the explanation.

With the index empty, `session->def_count` is 0, `splice_hi == def_count`
forces `suffix_min_order` to `UINT64_MAX`, and the span comparison that gates
`renumber` can never fail. The three paths fall together for one reason.

That is a design question, not a test-writing one, and it has two possible
answers: either the definition index is meant to be populated and is not — in
which case the definition-splice optimization has been silently inert and the
`renumber` machinery guards a case that cannot arise — or it is a superseded
design whose remaining code should be deleted. Both are the maintainer's call.
M7 replaces this file, so the cost of deciding late is bounded; the cost of
*not* deciding is that these lines sit on the ledger forever as debt no test
can pay.

No test was written to force these paths. Reaching them would mean driving
internal state the public API does not expose, which is the kind of test M7
deletes and the kind this plan forbids.

Gates: §13's chunk-equivalence obligations.
Clears: two branches of the `extensions/incremental.c` entry. The rest of that
entry is blocked on the decision above.

One caveat this milestone must respect: `extensions/incremental.c` is code M7
substantially replaces, and the plan's own rule is not to write tests against
code that M7 deletes. What justifies testing here is that these are
*equivalence* tests — they assert incremental output equals a fresh parse,
which is a §13 obligation the new implementation inherits unchanged. Tests
written against the internal splice structure would not survive M7 and must
not be written.

### M2 — Source substrate

`Source{profile, content}` with `STRICT_UTF8` and `PERMISSIVE_BYTES`, the
persistent byte storage behind it, and the `SourceEdit`/`Span` primitive in
stored-byte coordinates.

**`STRICT_UTF8` is not "valid UTF-8 only", and this is the milestone's main
trap.** It admits exactly one deviation: a truncated final code point — a
well-formed prefix at end-of-source that a continuation byte would complete.
Edits are byte-addressed, so a streamed chunk may split a multi-byte
character; rejecting that intermediate commit would make streaming legal only
under `PERMISSIVE_BYTES` and turn §8.2's "streaming is ordinary editing" into
a profile-conditional rule, which is the special-casing that section exists to
forbid. The boundary is asymmetric and all four cases are separate behaviour:

| Input, `STRICT_UTF8` | Result |
| --- | --- |
| truncated final code point | accepted; that tail decodes to U+FFFD |
| complete invalid sequence | rejected; neither source nor AST published |
| truncated code point with further bytes after it | rejected — the exception is positional |
| all three under `PERMISSIVE_BYTES` | accepted |

A document ending in a truncated tail is legal as a **final** document, not
only as an intermediate one: §8.2 forbids a finalize operation, a session may
close at any commit, and a caller may parse two bytes and stop. Do not build a
pending or awaiting-continuation state; there is none.

Gates: §14.3.1, §14.3.4, §14.3.5, §14.3.6, §14.3.7, and the multi-byte
boundary clauses of §14.8.2–3. §14.3.6 and §14.3.7 were added on 2026-08-03
because the boundary above had no gate at all: §14.3.1 exercises only *legal*
edits and §14.8 compares only final outputs, so an implementation rejecting
every incomplete chunk and one accepting every invalid sequence both passed.

Requirements that are easy to lose: repeated tail appends must not copy the
prefix; a tiny retained slice must respect the declared amplification bound;
and `Span` is built from `Offset`, never `EncodedOffset`, so a projected
coordinate cannot be fed back in as an edit.

### M3 — Extents and coordinates

`SourceExtent` as identity, the order-maintenance aggregate sequence carrying
subtree byte sums, and `Document.scope(extent, profile)` across all five
coordinate profiles.

**One document-wide sequence of leaf source-bearing units, not one sequence
per container.** §7.2 was tightened on 2026-08-03 to say so, because the
per-container shape resolves a node by summing a prefix at every level of its
spine — `O(depth · log width)`, which is `O(log n)` only when depth happens to
be. Markdown bounds no nesting depth: a measured 1000-deep `BlockQuote` chain
took 1001 descents where a flat 10000-block document took 9. A container
addresses a range of the one sequence.

This is also where §11.1's **ownership region** becomes real, because the
region is the unit concrete offsets are relative to (§0). Regions nest, and a
region's offsets are its own, so an edit inside a paragraph rewrites that
paragraph's concrete records and leaves every enclosing `ListItem`, `List`,
and `BlockQuote` marker record untouched at any depth. That property is what
keeps the CST out of every size-dependent bound; without it M3 and M2 both
compile but the complexity gates in M9 fail.

Gates: §14.3.2, §14.3.3, §14.5.1. The load-bearing property is §7.3: a prefix
insertion must not rewrite later nodes or extents, and must emit no diff entry
at all.

### M4 — Canonical text

`CanonicalText{value, map}` and `TextMap` as ascending non-overlapping
`SpanPair`s, including the cases where the two spaces diverge — entities,
escapes, smart punctuation — and the equal-length-but-non-corresponding cases
§6.1 calls out.

**A `SpanPair`'s source span is relative to the owning node's extent, not to
the document.** Absolute offsets would make a prefix insertion differ on every
later text field — one diff entry per suffix node, which §7.3 and §14.5.1
forbid. A consumer that wants the bytes resolves the node's extent once
through `Document.scope` and adds the pair's offsets to it.

**At most one field per kind is a `CanonicalText`.** Seven of the 34 kinds
carry one (`CodeBlock`, `HTMLBlock`, `FormulaBlock`, `Text`, `Code`, `HTML`,
`Formula`); the other 27 carry none and therefore never carry `TEXT` or
`TEXT_MAP` at all. Every other string field — `Link.destination`,
`Link.title`, `Image.source`, `CodeBlock.info`, every `label`, `name`,
`attributes`, `reference` — is a plain scalar of decoded characters with no
map, even though CommonMark resolves escapes and entities inside several of
them. Naming their source spans is the sub-node extent §7.2 defers.

Gates: the `TEXT` versus `TEXT_MAP` distinction of §9.1, proven on
`&amp;` → `&#x26;` rewriting with unchanged canonical text. **The example was
`&#38;` until 2026-08-03, and it proved nothing**: both spellings are five
source bytes producing one canonical byte, so the two maps are byte-identical
and §9.1 correctly reports no part at all. The working example needs the two
spans to differ in length, which `&#x26;` (six bytes) does. A test written
against the old example would have asserted a false proposition and passed by
accident only if `TEXT_MAP` were computed wrongly.

### M5 — Persistent tree, identity, and revisions

The persistent child sequence, structural sharing across adjacent documents,
`MarkupID`/`DocumentVersion` as domain-qualified pairs, and the
`MarkupRevision{self, subtree}` pair with the §5.4 aggregate rules.

**Prerequisite, and this is the milestone that owns it.** §14.7 requires the
existing representative, large-document, extension, and adversarial benchmarks
to be pinned *before the C-layer AST is extended*, and M5 is where that
happens. Pin them as their own change, merged first, or the "no size-dependent
term moved" comparison M9 has to make has no baseline to make it against.

**The continuity rule is now fixed, not left to the implementation.** §5.2
previously said only "a language-specific continuity proof"; it now states the
matcher: nothing outside a reparsed region is matched at all, identity never
crosses a parent or a kind, children the edit does not overlap are matched by
ordinal from the near end against stable old witnesses, and only the children
the edit overlaps are matched by content LCS with the leftmost tie winning.
The result must be a pure function of the old child sequence, the new child
sequence, and the normalized edit span — no dependence on hash order, arena
addresses, or traversal order.

The positional step is not decoration. Given `[A, A, B]` and an insertion at
the front producing `[A, A, A, B]`, content carries nothing that distinguishes
the inserted `A` from the survivors, so a content-only match — including an
order-preserving LCS — hands the first survivor's identity to the new node and
reports the survivor created. That is the §5.2 failure 14.2.3 tests for, and
the information needed to avoid it is in the edit span, not in the tree.

**`List.tight` needs an aggregate here.** It is folded over the item sequence,
so a grandchild edit that flips tightness emits `VALUE` on the `List` — the
one documented exception to §14.5.11's `DESCENDANT`-only shape — and "some
item is loose" must ride the persistent item sequence as a monoid. Refolding
it per commit makes an `O(1)` edit cost `O(W)` and breaks the bound for every
list in the document.

Gates: §14.2 in full, §14.4.3, §14.4.5. This is where the self-contained
document of §4.2 becomes real: no `materialize()` step, and retained documents
readable after session close.

### M6 — Parser answers

Parser answers moved into the document as immutable data behind the §4.1
`Document` queries, with the parser-owned inverted index that makes an
`ANSWERS` change cost the affected nodes rather than the reference population,
and explicit negative resolutions.

**Publication step 0 belongs to this milestone.** §6.3's order gained a step
before CST construction on 2026-08-03, because an undefined `[^x]` is `Text`
and an undefined `[x]` is prose: whether a run of bytes is one `Text` or a
reference node depends on a label the document may define anywhere, so
building the CST at step 1 needed step 3's answer. Step 0 updates the two
definition label sets — footnote labels and link-reference labels, which
normalize alike but share no key space — from the reparsed region, and names
the further regions whose brackets flip through a **mention** index. Mention,
not resolution: a bracket that was prose until now has no occurrence record,
so an index keyed by resolution cannot find it. Both steps must be
proportional to the reparsed region and the flipped labels, never to the
document.

**Two answer-record rules that reviews have already caught once.** `number`
and `referenceCount` describe the *label*, not the node asked: a shadowed
definition reports the same pair its winner does, and both are zero only when
the label itself has no references — reading them as node-scoped would report
zero beside a non-zero `winner`, the one combination the record cannot mean.
`Document.references` is the node-scoped accessor and is empty for a shadowed
definition. And relation indexes are eager for any document a session can
commit from; only a one-shot document may defer them, because step 4 compares
old and new answers at commit time.

**One reference model, decided 2026-08-02.** A link reference and a footnote
reference are the same concept — a label that resolves against a definition —
and today the tree states them two different ways. That is not inherited: it
is cmark's link model sitting next to a footnote model this repository
redesigned, and each of the three axes was split in the opposite direction.

|                    | definition        | resolved reference        | unresolved reference |
| ------------------ | ----------------- | ------------------------- | -------------------- |
| cmark-gfm, links   | no node           | `Link` with the payload   | text                 |
| cmark-gfm, footnotes | moved to tail, dropped when unreferenced | `FootnoteReference` | text |
| mdast, links       | `definition` node | `linkReference`, label only | text               |
| mdast, footnotes   | `footnoteDefinition` node | `footnoteReference`, label only | text        |
| here, links        | no node           | `Link` with the payload   | text                 |
| here, footnotes    | node at source position | label only          | node kept            |

cmark is self-consistent and so is mdast; only this repository is split. The
target is the third self-consistent model, the one §6.3 already describes: the
definition is a node at its source position, the reference carries its label
and nothing else, and resolution — including a negative one — is an answer.

This is not a milestone bolted on beside M6; it is what M6 requires. M6's own
cost goal — an `ANSWERS` change costs the affected nodes rather than the
reference population — is unreachable for link references while `Link` carries
the payload, because a destination inlined into the tree makes every
`[foo]: /new` edit reparse every unit containing `[foo]`, through
`reference_payloads_equal` and `collect_dependents`. Footnotes already meet the
bound for exactly the reason links do not: nothing from the definition enters
the reference node. Note also that a `Link` built from a reference carries a
destination that appears nowhere in the span it covers, which the §6.3
source-faithful clause does not permit.

The change is two node kinds and one deletion:

- `Definition` — a block node where a reference definition was written. Today
  the definition is consumed into the reference map and leaves nothing, so the
  source has text with no node.
- `LinkReference` / the image reference form — carries the label and reference
  form (full, collapsed, shortcut) and no payload. It is produced whether or
  not a definition exists, as `FootnoteReference` already is.
- The payload fields leave `Link`/`Image` for references. The inline forms
  `[a](/u)` and `![a](/u)` keep theirs: those are written in the source and
  are not resolution results.

Two findings constrain what "unify" can mean, and both were established by
running the parser rather than by reading it.

**The third axis has a grammatical floor, and footnotes come down to it.**
`[foo]` is a reference only because `foo` is defined; `[bar]` with no
definition is prose, not an unresolved reference. A link reference therefore
cannot be produced unconditionally the way `[^x]` can — `[^x]` carries a marker
that means nothing else, and a bare bracket does not. Links have no choice on
this axis.

Footnotes do, and the decision of 2026-08-02 is to give it up: an unresolved
footnote reference degrades to literal text, like an unresolved link reference.
Keeping the node would leave one rule that holds for footnotes and not for
links, which every renderer would then carry as a special case — and a renderer
cannot avoid the link rule, because the grammar imposes it. One rule that
cannot be removed plus one that can is not a choice between two models; it is a
choice about whether consumers get one rule or two.

The cost is stated plainly and accepted: a footnote definition edit gains the
definedness-flip reparse that link references already pay, so adding or
deleting `[^x]: body` reparses the units mentioning `[^x]` instead of flipping
their answers. It is bounded by the same `collect_dependents` machinery, not a
new class of work, and the *payload* dependency — the expensive one, and the
only one that fires on every keystroke inside a definition body — disappears
for both kinds at slice 3.

What it settles elsewhere is most of the reference-model divergence:
`unresolved-footnote-reference` leaves `specs/mdast-parity/deltas.json`
entirely, `reference-link-model` closes on all three of its axes rather than
one, and `footnote-resolution-model` in the upstream policy shrinks to the
placement and retention half, where remark backs this repository. Both external
authorities already agree with the degraded form; the disagreement that
remains is only where a definition lives. (Landed 2026-08-02 — see slice 5.)

`CrossLink` and `Embed` stay outside this rule and keep their nodes when
nothing resolves them. They are not references to a definition in the document
— there is no such definition to look for — so an unresolved one is not a
negative resolution but a target outside the document, which is the consumer's
to answer.

**The order of the two node changes decides whether anything is lost.** Taking
the payload out of a reference first would empty the canonical dump of every
destination, and `scripts/check-upstream-parity.mjs` — the only thing checking
that this repository resolves a reference to the same URL cmark does — would
have nothing left to compare. That reading suggested the dump had to start
printing resolutions.

It does not, and the reason is the order. `Definition` lands first, so the
destination never leaves the tree: it stops being copied into every reference
and is stated once, at the definition, where it was written. A renderer, and
the parity normalizer, resolve a label against the document's definitions —
which is exactly what `scripts/lib/mdast-oracle.mjs` already does to remark's
tree. No dump mechanism, no reference map retained past the parse, and the
one-shot CLI stays self-contained. Reversing these two slices would trade a
real verification for a modelling improvement; in this order nothing is traded.

Slice order.

1. `Definition` block node, where the definition was written. It carries this
   change's one genuinely new problem: definitions are harvested from a
   paragraph's accumulated content buffer at finalize, and mapping a byte
   offset in that buffer back to a source column needs the per-line
   stripped-prefix width, which nothing records today. Nothing else can land
   first without emptying the tree of destinations.
2. `LinkReference` / `ImageReference` nodes, payload out of `Link`/`Image` on
   the reference path only, the golden sweep, and both parity projections.
3. Session reference index and the node-keyed query, mirroring
   `extensions/footnote.c`. It indexes reference and definition *nodes*, so it
   follows the two slices that create them rather than preceding them — the
   earlier note here calling it the unblocking first step was wrong twice over:
   it is neither first nor a precondition someone else supplies.
4. Incremental: `reference_payloads_equal` stops driving the dirty set, so a
   payload edit emits `ANSWERS` and reparses nothing. This is where the cost
   result actually lands.
5. **Done, 2026-08-02.** Unresolved footnote references degrade to text.
   `unresolved-footnote-reference` left the mdast policy — the two now agree
   outright, 83/83 — and `applyUpstreamFootnoteModel` shrank to dropping
   unreferenced definitions, with upstream parity still at 855/855, which is
   what proves the degraded text matches cmark's byte for byte.

   Smaller in the parser than expected, larger in the session than expected.
   The lookup and the text fallback were indeed already there. What was not is
   that footnote definedness had to become a *document*-scoped fact available
   during an inline parse: a paragraph reparsed on its own must still see a
   definition a hundred lines below, or the incremental tree stops equalling
   the one-shot tree. So footnote definitions moved into a session-persistent
   map with the same registration, retraction, and definedness-flip
   reconciliation that reference definitions already had.

   A second map, not a discriminated column of the reference map. Sharing one
   would put `[x]:` and `[^x]:` in a single label bucket, where one winner
   stands for two independent definedness answers and one kind's flip can hide
   behind the other's presence — an under-invalidation, and a wrong tree. The
   mechanism is shared by parameterization instead: `markdown_core_definition_table`
   bundles a map with its line-ordered index, the session holds one per kind,
   and `collect_new_definitions` / `collect_stale_definitions` /
   `definition_sequences_equal` / `reconcile_prepare` / `reconcile_apply` run
   once per table. One mechanism, two instances. The only shared state is the
   commit's dirty-label set, which pools both tables' flips because a unit's
   dependencies are recorded by label with no table attached; the label
   namespaces overlap there, which over-approximates the dependents and never
   under-approximates.

   Registering only, without retracting, is the failure mode to know about: it
   was reached first and it hangs `ctest` rather than failing it. A persistent
   table that only grows is two bugs at once — a deleted `[^x]: body` stays
   defined, and every commit re-appends the same labels until the live chain
   is unbounded.

   **Coverage ledger.** One entry grew, under the policy's third rule (an
   intended behaviour change moved which code the pinning corpus reaches):
   `extensions/footnote.c`, lines 24 → 26 and branches 51 → 56. The cause is
   the length guard in `markdown_core_session_footnote_label_sites` and the
   `slot == SIZE_MAX` handling it feeds through the numbering pass. A
   `FootnoteReference` node now exists only where `markdown_core_map_lookup`
   found a definition, and that lookup already rejects a label outside
   `[1, MAX_LINK_LABEL_LENGTH]` — so no source input can produce a reference
   whose label fails to intern, and no corpus input can pin those branches
   again. They are not dead: `markdown_core_session_footnote_label` still
   returns the sentinel for a label that normalizes to nothing, which the
   definition scanner prevents but the function does not assume. Whether to
   fold that guarantee into the type is a separate decision, not one to take
   blind here.

   Two binding entries grew by one branch each, for the same reason and not
   the corpus one: a new node kind forces a new arm in an exhaustive
   dispatch, and the arm is unreachable by construction.
   `wire/WireDecoder.kt` gains the `else -> error(...)` of `referenceForm()`,
   which the bridge cannot trigger because it only ever encodes 0, 1, or 2 —
   the same shape the file's two boolean decoders already contribute.
   `session/relink.ts` gains `case "referenceDefinition"`, which joins nine
   sibling leaf cases that are all unreachable for one structural reason: a
   leaf has no children, so it never appears in a commit's `bubbled` list.
   Both arms exist to keep the dispatch exhaustive for the type checker, and
   neither has a test seam short of hand-assembling a wire payload or
   defeating exhaustiveness with a `default`. The Kotlin model file the same
   change adds, `model/Reference.kt`, needs no entry at all: a new equality
   and form test brings it to 100%, where its thirty sibling model files each
   still carry their unpinned `equals`/`hashCode` pair.

   Every other entry moved down: `extensions/incremental.c` by 36 lines, one
   function, and 20 branches (the coordination chain is now written once and
   run per table, so the second instance costs no new surface),
   `core/inlines.c` by 2 and 2, `extensions/lookups.c` and
   `extensions/session.c` by one branch each. `core/blocks.c`,
   `core/references.c`, and `extensions/reference.c` stayed at their recorded
   allowance rather than growing, which took shape decisions rather than
   tests: the two definition kinds share one entry constructor, the parser
   asks "are both maps here" once, and a watcher attaches its sink to both
   maps or to neither, so the parser attributes a footnote lookup without a
   second null test.
6. The four bindings, at M7 with every other public type.

**These slices are not independently landable, which a first attempt on
2026-08-02 established by trying.** The parser half of slice 1 — the node kind,
the union member, the per-line marks, and the node emitted at its source
position — is small and compiles. It cannot reach a green tree on its own,
because a node kind is not a core-local fact: `markdown_core_node_kind` in the
facade is a second enum with its own numbering, the dump and the equality
projection both switch on it exhaustively, the wire format numbers it, and each
of the four bindings decodes that number. A document with a reference
definition then reaches every one of those surfaces at once. The attempt was
reverted rather than left half-applied.

So the binding work is not a final slice. Every slice that adds a node kind
carries its own facade, wire, and binding change, and the slices above name the
parser work inside a change that is wider each time. The right unit of landing
is one node kind end to end — `Definition` first, `LinkReference` /
`ImageReference` second — each with its goldens, both parity projections, and
all four bindings in the same change. That also settles where this sits
relative to M7: a node kind changes a public type, so either these land at M7
or M7's "the only milestone that changes a public type" stops being true.

What it costs elsewhere: `notDeltas.link-reference-definitions` in
`specs/upstream-parity/deltas.json` stops being a non-difference and becomes a
registered delta with a projection, exactly like `footnote-resolution-model`;
and the `reference-link-model` shape delta in `specs/mdast-parity/deltas.json`
closes on the definition and label axes, leaving only the unresolved-reference
axis, where mdast degrades to text and this contract answers instead. (That
last axis closed too, at slice 5; the delta is retired.)

One contract clause narrows and has to be amended with this milestone rather
than left to be discovered. §6.3 lists "positive and negative reference
resolutions" among the parser's records, and this milestone's own summary above
says "explicit negative resolutions". Once an unresolved reference of either
kind is literal text, there is no node left to carry a negative answer, so on
the reference side the negative case stops existing rather than becoming an
answer. What survives is the definition side — a `Definition` or
`FootnoteDefinition` that nothing references — and `CrossLink`/`Embed`, whose
targets live outside the document. The clause must be rewritten to say that,
or it will read as a requirement nothing can satisfy.

Gates: §14.5.7. Clears: most of the `extensions/footnote.c`,
`extensions/lookups.c`, and `extensions/cross_reference.c` ledger entries.

### M7 — The atomic flip

One change: the new public C header of §15, the delta engine producing the
single postorder `diffs` list with six-flag parts, deletion of the four-array
delta and the separate ordered-entry API, and all four bindings moved over
together with their fixtures and examples.

Gates: §14.1 in full, §14.5 in full. This is the only milestone that cannot be
split.

It was also described here as the only milestone that changes a public type.
That stopped being true on 2026-08-02: unifying the reference model added three
node kinds to `markdown_core_node_kind` and its four binding projections. The
claim was worth stating and is worth correcting rather than quietly dropping —
a node kind is a public type wherever it is projected, so any change that adds
one carries its bindings in the same landing, and M7 is where the remaining
public types move together, not the only place any of them ever do.

### M8 — Consumer and framework gates

The two complete applications §14.1.8 requires — one that hands
`Commit.document` to a reactive framework and never touches `Delta`, one that
maintains its own state entirely from `diffs` — plus the §14.4 framework
gates and the §14.6 application gates over randomized edit traces.

Gates: §14.4, §14.6, §14.8.

### M9 — Complexity and rollout

The §14.7 telemetry, the §11 bounds enforced separately per §16.6, and the
§16 rollout checklist.

The measurement §14.7 now requires is a **paired** one: every term reported
for the new engine is reported a second time for an AST-only baseline that
captures no syntax-only record, on the same trace. §11.1 permits the CST to
move exactly one of them — the records created inside a reparsed region — and
nothing else. Regions reparsed, persistent nodes copied, extent-resolution
descents, and `|diffs|` must be identical with the CST and without it. That
comparison is against the benchmarks M5 pinned; if M5 shipped without pinning
them, this milestone has nothing to compare to and the gate is unenforceable.

## How the unpinned surface shrinks

The unpinned surface is **not** paid down by writing tests against the current
implementation. A test that executes the parser without asserting its output
raises the number and protects nothing, and a test written against today's
internals gets deleted by M7 along with the internals. Both are worse than
leaving the number where it is, because both make the gate lie.

It shrinks by **adding corpus inputs whose canonical dump is frozen**. Every
input added to `tests/fixtures/` or `specs/canonical-ast/` pins whatever
behaviour it reaches, permanently and independently of which implementation
produces it. That is the only work that survives the rewrite.

Priority follows the measurement, not the milestone order: `core/scanners.c` is
a third of the entire unpinned surface, and it is pure `source -> AST`
behaviour that this contract does not touch, so pinning it is both the largest
win and the safest.

- **Before M7 lands, the unpinned branch surface must be materially smaller
  than 51%.** How much smaller is a judgement call, but shipping the rewrite
  with half the parser's behaviour unprotected would defeat the purpose of
  having built this gate.
- **M2–M6 arrive at 100% by construction** — new files carry no `unpinned`
  entry, so the gate's first rule applies to them from their first commit.
- **M7 removes entries by removing files.** `extensions/delta.c` and the
  superseded parts of `extensions/session.c` and `extensions/incremental.c` go
  with the old surface.

### External oracles and what they have found

Two gates now check Markdown Core's semantics against authorities outside the
repository, and a third fuzzes against them. Everything else in the repository
compares this parser with itself — including the spec fixtures, whose expected
blocks this parser generated.

| Gate | Authority | State |
| --- | --- | --- |
| `check:upstream-parity` | cmark-gfm 0.29.0.gfm.13 | 681/681, three registered deltas |
| `check:mdast-parity` | unified / remark | 61/61, four shape deltas and eleven registered divergences |
| `fuzz:parity --oracle upstream` | as above, over generated inputs | green at 400 iterations, wired into CI |
| `fuzz:parity --oracle mdast` | as above | **not wired**: reports divergences of mixed root cause that are not yet triaged |

Defects these found, all of which the hand-written corpus had missed and two of
which its goldens had recorded as intended behaviour:

1. inline math did not strip code-span padding (`$$ x $$`);
2. a directive name could begin with `-` or `_`, and the fixture's prose stated
   that as a rule;
3. `#id` and `.class` attribute shorthand was unimplemented, and the canonical
   AST contract stated it was deliberately unsupported;
4. a malformed attribute block discarded the whole directive instead of leaving
   the name standing;
5. the `class` separator was written on the wrong condition;
6. a nameless shorthand marker (`{#}`) was skipped instead of invalidating the
   block; and
7. `merge_class_attributes` tested `strbuf.ptr` for allocation failure, which
   that type never reports through — a lost allocation would have silently
   truncated the joined value.

Fuzzing then found two more, both now fixed:

8. an unterminated directive label (`:badge[b`) discarded the directive. The
   obvious fix — scanning ahead for the closer — is quadratic on
   `:x[:x[:x[...]]]` and regressed a required complexity gate by 11x. The
   delimiter engine already pairs in linear time, so the fallback is expressed
   in what it leaves behind instead: the directive is emitted before the label
   opens and the marker is the bracket alone, so an unpaired opener discards
   only the bracket. No lookahead, `O(1)` per opener.
9. an inline directive could start inside a run of colons, so `x ::a` parsed as
   text plus a directive where the reference implementation keeps the run
   whole.

10. a leaf or container directive could not interrupt a paragraph. The cause
    was not in the directive extension: `open_new_blocks` treated any non-null
    return from an extension's block-start hook as "a block was opened", and
    several extensions return the container they were given on every
    non-matching path. With a paragraph open, the table extension always
    returned that paragraph, so no extension registered after it could ever
    interrupt one. The loop now ends only on a *different* node, which is what
    "opened a block" actually means. Upstream cmark-gfm carries the same
    pattern; it never shows there because upstream has no extension that needs
    to interrupt a paragraph.

That fix moved which code the pinning corpus reaches: the old redundant loop
iteration was incidentally exercising several defensive guards in
`extensions/table.c`, which the `api` suite still covers but the pinning suites
no longer do. The guards are reachable — one of them through a public accessor
that accepts any node — so this is not dead code; the ledger entry grew because
the corpus reaches less, not because protection eroded. `specs/coverage/policy.json`
gained a rule for exactly this case, since the "may only shrink" rule as first
written had no way to express it.

11. an attribute block with an unterminated quote scanned past the line and
    took a later `}` as its close, absorbing the text between. The rule it was
    missing is that a quoted value must be followed by whitespace or the
    closing brace, so `{a="x"b=1}` is an invalid block rather than two
    attributes.

One reported divergence turned out not to be ours. A run consisting only of the
spaces a break strips leaves an empty text node, which remark does not produce;
removing it looked right until `check:upstream-parity` failed — cmark-gfm emits
that node, and the goldens record it. Suppressing it would have moved this
parser away from the authority for the base language in order to agree with the
authority for the extensions. It is registered as an mdast shape delta instead,
and the gate failing is what classified it.

12. the attribute-name grammar was this repository's own invention — "any
    printable byte except a few delimiters" — where the reference implementation
    applies a Unicode rule: a name may not begin with whitespace or punctuation
    except `-` and `_`, and admits `.` and `:` from the second character on.
    The consequence was not just accepting a few extra names: blocks that
    should have been invalid parsed as valid and swallowed the text after them
    across lines, which is the family the fuzzer kept reporting. The scan is
    now by code point rather than by byte, because the rules are Unicode
    predicates. Verified name for name against the reference, non-ASCII
    included.

That fix and the table reordering both grew a ledger entry by a line or two of
new code whose bounds guards the pinning corpus cannot reach — a decode loop
that cannot advance by a non-positive width, an attach that cannot fail. The
container-fence fix moved a line and a branch of the generated `ext_scanners.c`
out of reach for the same reason: what the corpus reaches changed, not what is
protected. All are recorded under the rule for intended behaviour changes.

**Settled: registration order is priority.** The table-breaking case above was
fixed by attaching `table` last rather than by adding a priority field. The
repository's philosophy is that attachment order *is* priority, and the rule
this expresses is the one the core cascade already applies to paragraphs: a
construct that matches any line can only be the fallback. `try_opening_table_row`
matches any non-blank line inside an open table, so it belongs last.

Doing so also retired an earlier fix. `open_new_blocks` had been changed to
treat "returned the container it was given" as "did not open a block", to stop
the table extension short-circuiting every extension after it. That was
tolerating a contract violation rather than fixing it — and it was wrong on its
own terms, because `try_opening_table_header` retypes the paragraph node in
place and so returns the same pointer *on success* too, which the check read as
a non-match. The contract is now held where it belongs: the header opener
returns null on every path that did not open a block, and the core is back to
"non-null means claimed".

13. a container directive's closing fence did not end it. The fence was
    consumed and the container reported that the line *matched*, so the
    paragraph inside stayed open and the next line joined it lazily from
    outside the container. The first fix for this banned lazy continuation
    across any extension-owned container, which was a provenance test standing
    in for a semantic one and was rejected on review. The real distinction is
    the one CommonMark already draws: a block quote stops matching because its
    prefix is absent, and a paragraph may continue out of it; a fence *ends*,
    and nothing continues past it. `last_block_matches` now answers with three
    outcomes rather than two — continues, does not match, ends here — and the
    engine finalizes an ended block on the spot and stops processing the line,
    exactly as `parse_code_block_prefix` already did for a closing code fence.
    Two consequences had to be handled with it: the finalize walks from the
    innermost open block outwards, because a container can hold an open
    paragraph where a code block cannot; and a block that ends on its own
    terminator ends at that line, which `finalize` had expressed as a list of
    block types and now also reads from a flag naming the property itself.

    Three goldens moved, each toward a more accurate position: a container's
    end is now its own fence rather than an enclosing one, and a paragraph's
    end is its last line of text rather than the fence that closed the block
    around it. One of them is in `specs/canonical-ast/`, the corpus all four
    bindings share.

**Settled: the fuzzer is in CI.** Both oracles now fuzz on every required run.
The `mdast` oracle was held out until it was clean, and clean means twelve
independent seeds at 300 iterations each rather than the one seed CI pins —
wiring it at a seed that happened to pass would have been choosing the
evidence.

**Settled: a table breaks for an extension's block.** The GFM specification is
explicit — "The table is broken at the first empty line, or beginning of
another block-level structure" — so this was a defect rather than a matter of
taste. Built-in structures already broke a table; an extension's did not,
because the table's own *row* opener is an extension block start, was attached
first, and accepts any line at all. Attaching `table` last fixes it, and states
the rule where this repository keeps such rules: attachment order is priority,
and a construct that matches everything can only be the fallback.

**Settled: registration order is priority.** The table-breaking case above was
fixed by attaching `table` last rather than by adding a priority field. The
repository's philosophy is that attachment order *is* priority, and the rule
this expresses is the one the core cascade already applies to paragraphs: a
construct that matches any line can only be the fallback. `try_opening_table_row`
matches any non-blank line inside an open table, so it belongs last.

Doing so also retired an earlier fix. `open_new_blocks` had been changed to
treat "returned the container it was given" as "did not open a block", to stop
the table extension short-circuiting every extension after it. That was
tolerating a contract violation rather than fixing it — and it was wrong on its
own terms, because `try_opening_table_header` retypes the paragraph node in
place and so returns the same pointer *on success* too, which the check read as
a non-match. The contract is now held where it belongs: the header opener
returns null on every path that did not open a block, and the core is back to
"non-null means claimed".

13. a container directive's closing fence did not end it. The fence was
    consumed and the container reported that the line *matched*, so the
    paragraph inside stayed open and the next line joined it lazily from
    outside the container. The first fix for this banned lazy continuation
    across any extension-owned container, which was a provenance test standing
    in for a semantic one and was rejected on review. The real distinction is
    the one CommonMark already draws: a block quote stops matching because its
    prefix is absent, and a paragraph may continue out of it; a fence *ends*,
    and nothing continues past it. `last_block_matches` now answers with three
    outcomes rather than two — continues, does not match, ends here — and the
    engine finalizes an ended block on the spot and stops processing the line,
    exactly as `parse_code_block_prefix` already did for a closing code fence.
    Two consequences had to be handled with it: the finalize walks from the
    innermost open block outwards, because a container can hold an open
    paragraph where a code block cannot; and a block that ends on its own
    terminator ends at that line, which `finalize` had expressed as a list of
    block types and now also reads from a flag naming the property itself.

    Three goldens moved, each toward a more accurate position: a container's
    end is now its own fence rather than an enclosing one, and a paragraph's
    end is its last line of text rather than the fence that closed the block
    around it. One of them is in `specs/canonical-ast/`, the corpus all four
    bindings share.

**Settled: the fuzzer is in CI.** Both oracles now fuzz on every required run.
The `mdast` oracle was held out until it was clean, and clean means twelve
independent seeds at 300 iterations each rather than the one seed CI pins —
wiring it at a seed that happened to pass would have been choosing the
evidence.

**Open: a table does not break for an extension's block.** The GFM
specification settles what these look like — it is not a matter of taste:

> The table is broken at the first empty line, or beginning of another
> block-level structure

A directive block is another block-level structure, so remark is right and this
is a defect. Built-in structures already break a table here (`# heading` and
`> quote` both do), because the core cascade tries them before it reaches any
extension. An extension's block does not, and the reason is structural rather
than incidental: the table's *row* opener is itself an extension block start,
it is registered first, and it accepts any line at all. It therefore claims the
line before the directive extension is offered it.

The rule that follows is the one the core cascade already applies to
paragraphs: a construct that accepts any line has to be the lowest-priority
block start. Expressing that needs the block-start dispatch to know which hook
is a fallback, which the extension API has no way to say today. Reordering the
registration list would move the symptom without stating the rule, and would
leave the next extension that opens a block in the same position.

Two smaller cases have the same shape and should be settled with it:
`:::::name[...]` after a delimiter row, and emphasis after an unterminated
attribute block.

Until this is settled the `mdast` fuzzer stays out of CI: it is clean on some
seeds and not others, and wiring it at a seed that happens to pass would be
choosing the evidence.

With the fixes above and the fuzz pool scoped to where remark is the authority,
the `mdast` fuzzer is at 199/200. It stays out of CI until that last case is
settled — wiring it at a seed that happens to pass would be choosing the
evidence.

### The M7 acceptance mechanism

The gate above tells us how much behaviour is pinned. What proves no regression
is the pinned corpus itself, run against both implementations:

1. before M7 starts, dump every corpus input with the current implementation
   and freeze the result;
2. after M7, dump the same inputs with the new implementation; and
3. require byte-identical output, excluding only the tracking identities and
   revisions §13 allows to differ.

Any diff is either an intended semantic change that must be reviewed and
written into `canonical-ast.md`, or a regression. There is no third reading,
and no diff is accepted on the grounds that the new implementation is
"obviously" right.

Coverage bounds how much this proves: behaviour no corpus input reaches is
behaviour this comparison cannot check, which is exactly why the unpinned
number is the one to drive down first.

## What this plan does not decide

- Whether the Rust port begins from this contract's C implementation or from
  its test suite. The gate makes either viable: a fully covered C
  implementation plus the §14 acceptance gates is an executable specification,
  and the conformance manifest in `specs/canonical-ast/` is already
  language-neutral.
- The `NATIVE` coordinate profile's per-binding closed value set (§7.2), which
  each binding declares when it adopts in M7.
