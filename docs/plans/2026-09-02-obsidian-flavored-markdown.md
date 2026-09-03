# Obsidian Flavored Markdown implementation plan

Status: proposed. This plan implements the target contract in
[`docs/specs/obsidian-flavored-markdown.md`](../specs/obsidian-flavored-markdown.md)
without changing the existing parser profiles until the complete cross-platform
contract is ready.

## Outcome

Add one coherent Obsidian profile to the C parser and all three bindings. The
profile reuses the current CommonMark/GFM block and inline algorithms, adds the
documented OFM syntax as composable extensions, exposes every new semantic fact
through the immutable canonical AST, and keeps vault resolution and rendering
out of the parser.

The work is complete only when the official examples, negative boundaries,
cross-extension interactions, oracle comparison, allocation failures,
adversarial complexity, and all four public surfaces pass together. A partial
binding or a C-only hidden node is not a shippable intermediate state.

## Phase 1 — freeze the public model

1. Add the target value types and kinds to
   `docs/specs/canonical-ast.json`, `docs/specs/canonical-ast.md`, and
   `docs/specs/canonical-ast-dump.md`:
   `WikiLink`, `Highlight`, `InlineFootnote`, `Comment`, and `Callout`;
   wikilink fragment/block-subpath and callout-fold enums; shared image dimensions;
   `blockIdentifier` on addressable block kinds; and `taskMarker` on
   `ListItem`.
2. Replace stored `checked: Bool?` with the authored `taskMarker: String?`.
   Keep source compatibility only through a derived language convenience
   property when that does not duplicate wire state. Treat the public shape
   change as a major-version change rather than preserving two authorities.
3. Allocate stable C kind/field identifiers once, then update the native AST,
   C read-only facade, wire format, Swift/Kotlin/ES values, exhaustive visitors,
   walkers, dumpers, and AST projection audit atomically.
4. Add an `obsidian` preset in each binding and a CLI `--profile obsidian`.
   Keep the existing default and named profiles byte-for-byte stable. Add only
   the option bits needed to compose the preset; do not add a second parser.

Exit criterion: all public surfaces compile with exhaustive handling, the
canonical schema audit proves kind/field parity, and fixtures can express every
new fact before syntax recognition is enabled.

## Phase 2 — one OFM inline extension

1. Introduce one parser-owned OFM inline extension with scanners for wikilinks,
   comments, highlights, and inline footnotes. All scanners use the existing
   subject cursor, delimiter/bracket infrastructure, allocator, source map, and
   extension attachment order.
2. Make `![[...]]` and `[[...]]` one scanner and one `WikiLink` payload. Split
   path, fragment/block subpath, and display value once while scanning. Do not
   rescan the completed literal in a binding or renderer.
3. Make comments opaque during scanning. Support retained `Comment` nodes and
   parser-level stripping from the same recognized construct so stripping
   cannot change where other delimiters bind.
4. Parse highlight children through the normal inline engine, with code, HTML,
   and comments taking precedence. Implement inline footnote content through
   that same nested-inline path, without manufacturing reference identifiers.
5. Audit every inline extension caller and delete any product-specific
   delimiter skip table or repair path made obsolete by the shared model.

Exit criterion: official positive forms and unmatched/escaped/code/comment/HTML
negative forms have native golden AST tests, and size-doubling probes show
linear scanner work on long runs of `[`, `]`, `=`, `%`, and mixed openers.

## Phase 3 — block metadata and callouts

1. Add block identifiers during block finalization, when ownership is known.
   One attachment operation handles paragraph suffixes, structured-block
   follower lines, and list-item suffixes. It writes the optional field
   on the owned block and removes the marker from visible content.
2. Refine a block quote into `Callout` only when its first content line has the
   valid marker. Parse type, fold state, and title before ordinary body blocks
   are finalized; do not parse a block quote and then mutate/repair its tree in
   a post-pass.
3. Use ordinary container recursion for nested callouts. Unknown/custom types
   remain callouts; alias-to-style mapping stays outside the parser.
4. Cover title-only, empty-body, formatted-title, nested, invalid-position,
   mixed-case, custom-type, and whole-structured-block identifier cases.

Exit criterion: every identifier has exactly one owner, no marker survives as
visible text, nested callouts are stack-safe, and failure/OOM unwinds through
the existing node ownership path.

## Phase 4 — task markers, media parameters, and tables

1. Generalize the existing task-list scanner from `[ xX]` to one Unicode code
   point. Store the marker, derive completion, and retain the existing rule that
   only the item prefix is inspected.
2. Parse external image `W`, `alt|W`, and `alt|W x H` suffixes in the shared
   image construction path. Keep wikilink display parameters raw until vault
   resolution establishes the embedded file kind.
3. Move wiki alias-pipe awareness into the shared table/inline boundary so
   `[[target\|label]]` and `![[image\|100]]` stay inside one cell. Do not add a
   table-only wikilink parser.
4. Preserve current GFM semantics for ordinary tables and task items under the
   existing profiles.

Exit criterion: task markers round-trip through every public AST, two-hyphen
tables retain current behavior, escaped wiki pipes never create extra cells,
and image dimensions are absent rather than guessed on malformed suffixes.

## Phase 5 — suppress Markdown inside inline HTML

1. Add the smallest inline HTML element stack required to know when inline
   bytes are inside a matched element. Reuse the current HTML tag scanner; do
   not introduce a second HTML tokenizer.
2. While the stack is non-empty, emit tags as `HTML` and intervening bytes as
   literal `Text`. Disable CommonMark and OFM inline constructs in that region.
3. Preserve inherited handling for void elements, raw-text elements, comments,
   declarations, processing instructions, CDATA, and unmatched/malformed tags.
4. Add nested-element, same-name nesting, mixed block/inline, malformed closer,
   and long-adversarial-tag tests.

Exit criterion: the official `<span>`/`<div>` invariant holds without changing
the CommonMark profiles or making tag-stack memory grow with unrelated input.

## Phase 6 — product fixtures and external evidence

1. Add one package-owned C fixture for the OFM target contract. It owns official
   examples, strict fallbacks, option gates, cross-extension conflicts, scopes,
   and source-order behavior. Do not copy product goldens into
   `specs/oracles/`.
2. Extend `specs/canonical-ast/` with compact cross-binding cases covering every
   new kind, enum state, nullable field, ownership edge, and escaping rule.
3. Move each resolved entry out of
   `specs/oracles/obsidian/deltas.json` in the implementation commit that makes
   it agree. If a deliberate AST-shape difference remains, register a general
   projection and prove that it fires; never replace a semantic difference with
   normalization.
4. Keep official-only requirements—callouts, block identifiers, inline
   footnotes, image dimensions, and HTML suppression—under product goldens,
   because the selected oracle does not parse them. Its silence is not
   agreement.
5. Add deterministic fuzz seeds and pathological cases for delimiter runs,
   nested callouts/HTML, escaped table pipes, long paths/headings, and repeated
   block identifiers. Assert semantic output and structural resource bounds,
   not wall-clock thresholds.

Exit criterion: C correctness/conformance, Swift macOS, Kotlin JVM, ES Node and
browser, all static audits, and every external parity gate pass. Required CI on
the remaining supported hosts then supplies the platform release evidence.

## Delivery sequence

The durable review sequence is model, inline engine, block ownership, existing
extension integration, HTML suppression, and evidence. Each change must leave
all existing profiles green. No phase may ship the `obsidian` preset publicly
until its AST exists on every platform and the full target fixture is enabled;
before that point the preset remains internal test plumbing.

The final release notes must list the documented OFM subset, the parser-only
boundary, the `checked` to `taskMarker` migration, the unchanged legacy profile
semantics, and the exact official help snapshot used for conformance.
