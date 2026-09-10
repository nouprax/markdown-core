# P5/P6 merged-PR audit and delimiter consolidation

Scope: every changed file in [PR #226](https://github.com/nouprax/markdown-core/pull/226),
including its two review fixes. The reproducible comparison is
`git diff 25282384 0c68bd1d6193d3a5ce58bd91bf9939fc78b5b55f`: 100 files.
The follow-up is based on that merged commit.

## Complete audit coverage

| Area | Files | Invariants checked and disposition |
| --- | ---: | --- |
| Specifications, plans, root README/changelog | 14 | P5/P6 grammar, M4 bracket precedence, scopes, attributes, declared oracle differences and completed landing claims. Preserve normative behavior; clarify the leading-bracket sentence and document the new stack model. |
| C implementation and facade | 14 | All delimiter/bracket callers, node kinds/containment, source marks, field ownership, heading continuation, contextual escapes, OOM and cleanup. Replace side state and duplicated delimiter reduction as described below. |
| C tests, fixtures and runner wiring | 14 | Every new/changed input, retained historical inputs, exact scopes, failure sweeps and work counters. Preserve old inputs; add boundary-reduction and shared-constructor regressions. |
| ES model, transport, visitors, tests and README | 16 | Kinds 34/35/36, content/attribute decoding, exhaustive dispatch, walk order, released-native ownership and scopes. No additional parser or matching path exists. |
| Kotlin model, JNI/Native, ABI, visitors and tests | 16 | Wire ordinals and materializers agree with C; typed content survives native release; JVM/klib surfaces and visitor phases agree. No binding repair required. |
| Swift materialization, model, visitors and tests | 10 | C-kind dispatch, value ownership, attributes/scopes, ordinary children and both walk phases. No binding repair required. |
| Canonical inputs, goldens and manifest | 7 | Three cases cover Span empty/populated content, scripts, fields and heading/reference composition. Their values remain unchanged by the refactor. |
| Oracle and JVM-surface ledgers | 6 | Historical P5/P6 inputs and exact difference digests remain; P2 helper entries are existing internal JVM lowering, not newly exposed API. Add isolated whitespace agreements without waivers. |
| Fixture audit, extension audit and fuzz scope tools | 3 | Coverage checks see the new kinds/Span states; `]` belongs to bracket dispatch; remark's foreign-Span exclusion has a retained exact witness. Preserve these guards. |
| **Total** | **100** | |

## Findings and implemented disposition

- [x] **The whitespace information is necessary; the three side-state fields are
   not.** `subject.script_boundary`, `delimiter.lower_bound` and
   `subject.pending_fields` mixed source facts, pairing bounds and continuation
   state. They are removed. Marker, raw-boundary and field-completion entries
   now share one stack and lifecycle. Word-content boundaries use the existing
   opener-search memo. A completed scope retains one boundary summary so the
   information survives Link/Span and formatting reduction without rescanning.

- [x] **Parsed delimiter containers had two construction paths.** Strikethrough
   retyped its opener and separately moved children, placed scopes and removed
   delimiter entries. It now uses the same constructor as emphasis, marks,
   insertion and scripts. That removes a real scope defect: `~~a\nb~~` ended at
   `1:3`, although its closer is at `2:3`. Parent-containment rejection is now
   checked by the shared constructor for every parsed delimiter rule, preserving
   the authored fallback and the existing strikethrough rejection invariant.

   The same failure audit exposed unchecked bracket-owner attachment: rejecting
   Span, Link, Media or either footnote Cite form could discard authored content
   after transferring it to an unattached node. All five regression cases fail
   before the fix. Every bracket alternative now checks the same parent policy
   before moving children or consuming a suffix; tests assert both retained
   content and zero leaked allocations after document disposal.

- [x] **Extension callbacks owned stack mutation.** Formula now only constructs
   its opaque AST value. Core reduces the interior and removes endpoints on
   success or failure. The unused extension stack traversal/removal entry points
   are removed, so callbacks cannot erase content-boundary facts. These are
   private engine headers/functions; the installed C facade and binding ABI do
   not change.

- [x] **Owned fields require token-order completion.** Moving field parsing to a
   final whole-tree pass would change reference/inline-footnote occurrence order
   and lose heading suspension behavior. The stack's field event keeps that
   lifecycle explicit and finishes before the next token. Fields retain their
   independent delimiter scope and report raw whitespace through their reduced
   stack. The source-map adoption and extension-OOM guards from the review fixes
   remain necessary and are retained.

- [x] **Contextual escape completion is a distinct semantic phase.** The
   `ESCAPED_SPACE` token flag and final ownership walk are retained. They do not
   form a second delimiter parser. Decoding during speculative pairing would
   mishandle a failed outer candidate, an owned label, or a document-owned
   footnote. Anchor reservation likewise must see final live ownership.

- [x] **Span already uses the correct shared bracket abstraction.** Direct and
   resolving reference tails precede Span; shortcut references and footnote
   calls follow it, as M4 specifies. Attribute recognition and child transfer
   are shared. The literal image bang, nested bracket scopes and discarded
   footnote-call content retain their existing ownership rules. Opaque token
   scanners stay opaque; their raw interior is not ordinary inline whitespace.

- [x] **The original oracle evidence was too compound for the specific question.**
   The old `p6-whitespace-recovery` case includes `^a b^`, recovery and Unicode
   whitespace in one deliberate-difference row. Four isolated agreement cases
   now test the facts below without any ledger waiver.

## Pandoc evidence

The local oracle is the pinned Pandoc **3.11**, using
`markdown_strict+superscript+subscript`, JSON output and an empty data directory.
The [pin](../../specs/oracles/pandoc/source.json) and
[corpus](../../specs/oracles/pandoc/corpus.json) retain the invocation contract.

| Input | Pandoc JSON inline structure | Markdown Core |
| --- | --- | --- |
| `^a b^` | `Str("^a"), Space, Str("b^")` | Text; no Superscript |
| `~a b~` | `Str("~a"), Space, Str("b~")` | Text; no Subscript |
| `^ab^` | `Superscript([Str("ab")])` | Superscript |
| `^a&#32;b^` | `Superscript([Str("a"), Space, Str("b")])` | Superscript with decoded space |

This establishes the ordinary-space rule, not full equivalence with Pandoc.
In particular, this strict profile leaves `^a\ b^` literal; the dialect's
contextual NBSP rule is an existing documented difference. Empty bodies,
Unicode whitespace/recovery, decoded TAB, maximal tilde runs and opaque code
also retain their explicit historical differences. The corpus now has 55
cases: 31 agreements, 14 deliberate differences and 10 future feature gaps.

## Verification

The [delimiter architecture](../architecture/inline-delimiters.md) states the
shared data model, scope reduction proof, ownership and failure invariants.
New tests cover raw/entity/escaped whitespace across formatting, nested Span,
links, owned fields and inline footnotes; cross-line Strikethrough scopes;
containment rejection for parsed delimiter and bracket kinds; strict allocation
failure; and size doubling through 8192 units. The allocation comparison keeps
identical whitespace in the ordinary-text baseline and retains exact equality;
there are no input-size or cardinality branches in the implementation.

A 472-case differential against a static executable of the merged commit found
four cross-line Strikethrough scope corrections and no other output changes.
It combined 23 bodies, ten ownership/formatting wrappers, both script markers,
and twelve scope/escape regressions.

C correctness (83 tests) and conformance (2 tests), ASan/UBSan/TSan (83 each),
Swift and its external consumer, Kotlin JVM/Native/Android-host, ES Node/browser
and all binding conformance checks pass. All six oracle gates pass, as do
400-input seed-1 CommonMark/GFM/remark fuzz runs. All seven host benchmark
cases pass. The position, containment and reference-order ledgers are unchanged.
`pnpm verify` and the host release dry run pass on the final implementation.
The repository audit is repeated after staging and in a clean committed tree
before push. Full Linux/macOS release aggregation remains a CI gate.
