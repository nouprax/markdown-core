# Markdown Core dialect

Status: normative. This document and the feature modules under
[`dialect/`](dialect/) define the Markdown Core dialect: the complete set of
syntax the parser recognizes, the external implementation that is locked as
evidence for each piece of it, and the rules that hold across all of it. `canonical-ast.json` and its prose companion
[`canonical-ast.md`](canonical-ast.md) remain the contract of the implementation
as it stands today; where a module describes a kind, field, or value that
the implementation does not have yet, its status line names the landing
item in the [landing plan](../plans/2026-09-04-canonical-vnext-landing-plan.md)
that adds it, and until that item merges the current contract stands.

## What the dialect is

Markdown Core is one Markdown dialect. Its base is CommonMark 0.31.2. On that
base it recognizes a fixed set of syntax features absorbed from GitHub Flavored
Markdown, the remark/micromark extension family, Obsidian Flavored Markdown,
Pandoc's Markdown, and `markdown-it-ins`. Every feature is always on: the
dialect has no parse options, no profiles, presets, umbrella switches, or
dialect modes, and no caller composes a smaller language. `obsidian`,
`pandoc`, `gfm`, and the like are names of sources, not of anything the
parser accepts.

Implementation boundaries follow feature semantics and ownership. Source
families do not define runtime modules or extension descriptors: cross links,
comments, marks, and footnotes use their own semantic operations and the shared
parser infrastructure. A shared source is not a reason to combine them into an
umbrella extension.

The upstream tools are sources and evidence, never authorities over behavior:

- A source defines which feature exists and what its common-case source form
  looks like. The module that adopts the feature is the sole normative statement
  of its grammar, its AST, its fallback, and its scopes.
  Where a module is silent, behavior is undefined until the module is amended;
  it is never inherited from the source.
- An executable oracle is a pinned implementation whose output is compared with
  the parser's over a corpus. A difference is a registered delta in the oracle's
  `deltas.json`, never a rule change. Byte-for-byte agreement with any oracle is
  not a goal of this dialect.
- The repository's own conformance fixtures are the oracle of record: the
  package fixtures under `packages/markdown-core/tests/fixtures/` and the shared
  canonical cases under `specs/canonical-ast/`.

Where two sources define the same bytes differently, the module states one
rule and the collision is recorded in [conflicts](dialect/conflicts.md); an
entry marked open there awaits a product decision, and the module's rule is
provisional until it is settled.

## Sources and executable oracles

| Source                                              | Pinned at                                                                     | Executable oracle                                                    | Gate                         |
| --------------------------------------------------- | ----------------------------------------------------------------------------- | -------------------------------------------------------------------- | ---------------------------- |
| CommonMark specification                            | 0.31.2, the `spec.txt` of the pinned cmark commit                             | cmark 0.31.2, `specs/oracles/cmark/`                                 | `pnpm check:commonmark-parity` |
| GitHub Flavored Markdown extensions                 | cmark-gfm 0.29.0.gfm.13, `specs/oracles/cmark-gfm/`                           | cmark-gfm at that commit                                             | `pnpm check:gfm-parity`      |
| remark/micromark extensions                         | `micromark-extension-directive` 4.0.0, `micromark-extension-math` 3.1.0, `remark-gfm` 4 through the lockfile | unified/remark, `specs/oracles/remark/`                              | `pnpm check:mdast-parity`    |
| Obsidian Flavored Markdown                          | `obsidianmd/obsidian-help` at `d780d6b48a92ee6a150304b40ee888f322bf43bf`      | `@quartz-community/remark-obsidian` 0.2.4 and `yaml` 2.9.0, `specs/oracles/obsidian/` | `pnpm check:obsidian-parity` |
| Pandoc's Markdown                                   | Pandoc 3.11 at `b913622e1ff87c69ab8b1a606577122e220925cd`, `specs/oracles/pandoc/source.json` | the official Pandoc 3.11 CLI with `--to=json`, `specs/oracles/pandoc/` | `pnpm check:pandoc-parity` |
| `markdown-it-ins`                                   | 4.0.0 with `markdown-it` 13.0.2, `docs/specs/dialect/insertion.md`         | `markdown-it` with the plugin, `specs/oracles/markdown-it-ins/`      | `pnpm check:ins-parity` |

Each oracle is locked to the features whose rows below name it. An oracle has
no opinion outside those features: cmark-gfm does not judge CommonMark, Pandoc
does not judge inherited syntax, and remark-obsidian does not judge callouts or
block identifiers, which it does not implement. Every gate is fail-closed: an
unregistered difference fails it, and a registered difference that stops
reproducing fails it too.

## Features

Every feature is part of the one language and has no switch. `Status` is
`present` when the implementation recognizes the syntax and produces the
module's model, `partial` when it recognizes the syntax but produces a model a
landing item still changes, and `missing` otherwise; the item named is the one
that makes the row `present`.

| Feature                             | Module                                                            | Source                                 | Executable oracle                    | Status                          |
| ----------------------------------- | ----------------------------------------------------------------- | -------------------------------------- | ------------------------------------ | ------------------------------- |
| CommonMark blocks and inlines       | [base](dialect/base.md)                                           | CommonMark                             | cmark                                | present                         |
| HTML comments as `Comment`          | [comments](dialect/comments.md)                                   | CommonMark, Obsidian                   | cmark for the token boundaries       | present                         |
| pipe tables                         | [tables](dialect/tables.md)                                       | GFM                                    | cmark-gfm                            | present                         |
| strikethrough                       | [strikethrough](dialect/strikethrough.md)                         | GFM                                    | cmark-gfm                            | present                   |
| autolinks                           | [links and images](dialect/links-and-images.md)                   | GFM                                    | cmark-gfm                            | present                         |
| task lists                          | [task lists](dialect/task-lists.md)                               | GFM                                    | cmark-gfm                            | present                         |
| footnotes                           | [footnotes](dialect/footnotes.md)                                 | GFM                                    | cmark-gfm, remark                    | present                         |
| formulas                            | [formulas](dialect/formulas.md)                                   | GitHub math, remark                    | remark (`micromark-extension-math`)  | present                         |
| directives and nameless containers  | [directives](dialect/directives.md)                               | remark-directive, Pandoc `fenced_divs` | remark, Pandoc for the nameless form | partial, `P8`             |
| resolved reference links and images | [links and images](dialect/links-and-images.md)                   | CommonMark                             | cmark                                | present                         |
| universal anchor field              | [anchors](dialect/anchors.md)                                     | Pandoc, Obsidian                       | Pandoc                               | present                   |
| universal attributes field          | [attributes](dialect/attributes.md)                               | Pandoc                                 | Pandoc                               | present                   |
| cross links and embeds              | [cross links](dialect/cross-links.md)                             | Obsidian                               | remark-obsidian                      | present                   |
| marks                               | [marks](dialect/marks.md)                                         | Obsidian                               | remark-obsidian                      | present                         |
| `%%` comments                       | [comments](dialect/comments.md)                                   | Obsidian                               | remark-obsidian                      | present                         |
| inline footnotes                    | [footnotes](dialect/footnotes.md)                                 | Obsidian, Pandoc                       | none; product fixtures               | present                   |
| task markers                        | [task lists](dialect/task-lists.md)                               | Obsidian                               | remark-obsidian                      | present                         |
| properties                          | [properties](dialect/properties.md)                               | Obsidian                               | `yaml`                               | present, `O6`                   |
| block identifiers                   | [block identifiers](dialect/block-identifiers.md)                 | Obsidian, adapted declaration syntax   | none; product fixtures               | present                         |
| callouts                            | [callouts](dialect/callouts.md)                                   | Obsidian                               | none; product fixtures               | present, `O8`                   |
| image dimensions                    | [links and images](dialect/links-and-images.md)                   | Obsidian                               | none; product fixtures               | present                         |
| insertion                           | [insertion](dialect/insertion.md)                         | `markdown-it-ins`                      | `markdown-it-ins`                    | present                         |
| inline code attributes              | [attributes](dialect/attributes.md)                               | Pandoc                                 | Pandoc                               | present                  |
| heading attributes                  | [attributes](dialect/attributes.md)                               | Pandoc                                 | Pandoc                               | present                  |
| fenced code attributes              | [attributes](dialect/attributes.md)                               | Pandoc                                 | Pandoc                               | present                  |
| link attributes                     | [attributes](dialect/attributes.md)                               | Pandoc                                 | Pandoc                               | present                  |
| automatic anchors                   | [anchors](dialect/anchors.md)                                     | Pandoc, GFM algorithm                  | Pandoc                               | missing, `P3`                   |
| implicit heading references         | [anchors](dialect/anchors.md)                                     | Pandoc                                 | Pandoc                               | missing, `P4`                   |
| bracketed spans                     | [bracketed spans](dialect/bracketed-spans.md)                     | Pandoc                                 | Pandoc                               | present                   |
| superscript and subscript           | [superscript and subscript](dialect/superscript-and-subscript.md) | Pandoc                                 | Pandoc                               | present                   |
| citations                           | [citations](dialect/citations.md)                                 | Pandoc                                 | Pandoc                               | missing, `P7`                   |
| fancy lists                         | [lists](dialect/lists.md)                                         | Pandoc                                 | Pandoc                               | missing, `P9a`                  |
| specimens                           | [specimens](dialect/specimens.md)                                         | Pandoc                                 | Pandoc                               | missing, `P9b`                  |
| definition lists                    | [definition lists](dialect/definition-lists.md)                   | Pandoc                                 | Pandoc                               | missing, `P10`                  |
| table captions                      | [tables](dialect/tables.md)                                       | Pandoc                                 | Pandoc                               | missing, `P11a`                 |
| simple tables                       | [tables](dialect/tables.md)                                       | Pandoc                                 | Pandoc                               | missing, `P11b`                 |
| multiline tables                    | [tables](dialect/tables.md)                                       | Pandoc                                 | Pandoc                               | missing, `P11c`                 |
| grid tables                         | [tables](dialect/tables.md)                                       | Pandoc                                 | Pandoc                               | missing, `P11d`                 |

The parser publishes no switch: `Document.parse(source)` is the one entry
point on every surface, and smart punctuation is not part of the language,
so quotation marks, hyphen runs, and periods are stored as written. Nothing
in the dialect strips anything: an HTML comment is a `Comment` node, and a
consumer that does not want comments drops the nodes. Pandoc's `startnum` has no
counterpart: a list's start number is always the value of its first marker.
Pandoc's `compact_definition_lists` has no counterpart: compact and loose
definitions are two source forms of one feature.

There is no internal layer selection either. The parser attaches every
extension on every parse; the package fixtures, the oracle gates, and the
position audits parse the one language through the same entry a consumer
uses; and an oracle's authority is a matter of which inputs it judges, never
of how they are parsed. Where the dialect deliberately leaves an oracle's
language, `specs/oracles/` registers the difference against the exact inputs
or as a projection the comparison applies, and where the engine has not
caught up with the dialect's own rules, the oracle's backlog names the item
that closes the gap.

## Ground rules

These rules hold in every module. A module that needs an exception states it
explicitly.

- There are no switches. Every feature is always recognized, and the
  CommonMark base parse is the meaning of every byte that no feature claims; a
  feature changes the parse of source the inherited grammar already accepts
  only where its module states the exact rule.
- No feature publishes two representations of one semantic fact, and no
  compatibility alias survives a model change.
- Values are stored as written. "As written" means no lowercasing, aliasing,
  trimming, slugging, or derivation of one field from another, unless the
  module states the transformation; the shared escape and character-reference
  decoding of the grammar that produced the value still applies. Matching,
  resolution, and rendering are consumer policy.
- A comment is a `Comment` node and is never stripped: an HTML comment under
  the inherited grammar and a `%%` comment. A consumer that
  does not want comments drops the nodes.
- There is no emoji support of any kind: no shortcode table, no alias step in
  anchor generation, and no special treatment of emoji scalars anywhere.
- `scope` records original source. Everything that is written has a scope and
  nothing else does: a synthesized value has no scope and adds no fictional
  position, and no operation copies, unions, or substitutes another
  occurrence's range.
- `TableCell.content` is `[Markup]`, the most general content model; no cell
  is normalized to a `Paragraph` and none is unwrapped from one.
- The parser emits no diagnostics. Where two authored declarations collide,
  both keep their values; the dialect never renames, drops, or reports one.
- The parser resolves nothing outside the document: no vault path, file
  system, URL, bibliography, CSL locale, MathJax, Mermaid, search engine, or
  renderer setting. Every such result is a consumer operation and is not a
  field of the AST.
- Valid UTF-8 is a caller precondition of the C entry point; its output for
  invalid input is unspecified, and every binding guarantees valid UTF-8 before
  calling it. No module adds validation or repair.
- `VERSION` stays `3.0.0` until it is released, and nothing is frozen before
  that: kind ordinals, wire layouts, manifest order, and identifiers may change
  in any reviewed change.
- Grammar notation is shared: `EOL` is a line ending or the end of input, so
  a construct on a document's last line needs no trailing line ending; `SP`
  is one space, `WSP` a space or tab, and `*3SP` up to three spaces.

## Examples

Every module states its rules in prose and grammar and illustrates each rule
with examples in the CommonMark specification's format: a fence line of 32
backticks followed by ` example`, the Markdown input, a line holding one `.`,
the expected canonical AST dump, and a closing fence of 32 backticks. The expected dump is the module's target model in the
grammar of [`canonical-ast-dump.md`](canonical-ast-dump.md), including the
encodings that document reserves for kinds and fields that have not landed
yet; where an example's syntax is present in the implementation today, its
dump differs from the current fixtures only by the model changes of the
landing plan's items `M0` through `M7` and by those reserved encodings.

Every example runs the whole dialect, so the fence line carries no tags and
there is nothing to configure. Examples are numbered by position within their
module, first to last, and a runner reports an example as its module and
number.

Every example is normative. The item that lands a module's behavior adds the
module's examples to the package fixtures byte for byte, in the same fixture
format, and a later change to an example is a behavior change reviewed like
any other. An example never demonstrates behavior its prose does not state;
where the two disagree, both are wrong and the module is amended.

## Recognition order

Recognition is one pass of the shared block parser followed by one pass of the
shared inline parser over each inline container, in the order the two tables
below fix. No module rescans a completed node, runs a regular expression over
finished source, or repairs a tree in a post-pass; the email autolink
post-pass named in the table is the only exception and is bounded to the
`Text` nodes it names.
Each module's own section names the step it occupies; the tables are the only
statement of cross-module precedence.

Within class A the leftmost opener in source order wins and its matched region
is opaque to every later class and to every later class-A candidate inside it.
At one position the rows are tried in table order, with one exception: at a
backslash, the formula openers `\\(` and `\\[` of step A4 are tested before
the escape of step A1, so a doubled backslash opens a formula and a single
backslash stays an escape. A class-A candidate that
fails consumes nothing: the cursor returns to the candidate's first byte and
the next alternative runs from there.

### Inline

| Step | Construct                                                                                       | Class                                                                 |
| ---- | ----------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- |
| A1   | backslash escape, after the `\\(` and `\\[` openers of A4 at a backslash                        | scanner                                                               |
| A2   | code span                                                                                       | scanner, opaque                                                       |
| A3   | raw HTML token, HTML comment as `Comment`, angle-bracket autolink, bare URL and `www.` autolink | scanner, opaque token or run                                          |
| A4   | formula `$`, `$$`, `` $`...`$ ``, `\\(`, `\\[`                                                  | scanner, opaque                                                       |
| A5   | inline comment `%%...%%`                                                                        | scanner, opaque                                                       |
| A6   | cross link `[[...]]`, `![[...]]`                                                                | scanner, opaque                                                       |
| A7   | inline footnote `^[...]`                                                                        | scanner, body parsed                                                  |
| A8   | citation key `@key`, `-@key`                                                                    | scanner; a bare key with no bracketed tail is finalized document-wide |
| A9   | example reference `(@label)`                                                                    | scanner, finalized document-wide                                      |
| A10  | text directive `:name[...]{...}`                                                                | scanner                                                               |
| A11  | character reference                                                                             | scanner                                                               |
| B1   | direct link and image tail, then an attribute container                                         | bracket close, first                                                  |
| B2   | resolving full and collapsed reference tails, then a container                                  | bracket close                                                         |
| B3   | `[...]{attrs}` span                                                                             | bracket close                                                         |
| B4   | `[@key...; ...]` cite group                                                                     | bracket close                                                         |
| B5   | resolving shortcut reference                                                                    | bracket close                                                         |
| B6   | defined footnote call `[^label]`                                                                | bracket close, last                                                   |
| C1   | `*`, `_` emphasis and strong                                                                    | delimiter stack                                                       |
| C2   | `~~` strikethrough                                                                              | delimiter stack                                                       |
| C3   | `~` subscript                                                                                   | delimiter stack                                                       |
| C4   | `^` superscript                                                                                 | delimiter stack                                                       |
| C5   | `==` mark                                                                                       | delimiter stack                                                       |
| C6   | `++` insertion                                                                                  | delimiter stack                                                       |
| D    | attribute suffix at every attachment site                                                       | immediately after its owner                                           |
| E    | GFM email autolink                                                                              | post-pass over `Text` only                                            |
Classes B and C are the inherited bracket and delimiter algorithms of
CommonMark, extended with the listed steps. Class B is the ordered procedure
run at every unescaped `]` that matches an active bracket opener; the first
success owns the pair and a failed alternative leaves the cursor at the `]`.
Class C is the delimiter stack: each listed delimiter is a run of units that
open or close under the flanking rules its module states, matched by the
inherited process-emphasis algorithm.

### Block starts

| Step | Block start                                                                                    |
| ---- | ---------------------------------------------------------------------------------------------- |
| 0    | properties envelope, first line of the document only                                           |
| 1    | container prefixes of open containers                                                          |
| 2    | fenced and indented code, HTML block, an HTML comment block as `Comment`                       |
| 3    | formula block `$$` and `\\[` lines                                                             |
| 4    | block comment `%%` line                                                                        |
| 5    | block quote, becoming `Callout`, with metadata on its first line                               |
| 6    | list markers, including fancy and example markers, then an optional task prefix                |
| 7    | container, nameless container, and leaf directive `:::name`, `::: {...}`, `::: word`, `::name` |
| 8    | ATX heading, with its attribute container                                                      |
| 9    | Setext heading                                                                                 |
| 10   | tables: caption-prefixed, pipe, grid, multiline, simple                                        |
| 11   | thematic break                                                                                 |
| 12   | footnote definition, reference definition                                                      |
| 13   | definition list                                                                                |
| 14   | block identifier line `#anchor-id#`                                                           |
| 15   | paragraph, with the `#anchor-id#` suffix at finalization                                      |

A block start is tested at the first non-space byte of the line after the open
containers' prefixes have been consumed. Steps 2 through 14 are tested in
order, and the first module whose start condition holds owns the line; a
module's start condition includes any lookahead its section requires, and a
candidate that fails leaves the line to the next step. Which steps may
interrupt a paragraph is stated by each module; a step whose module says
nothing cannot.

## Opacity

Source bytes owned by these constructs are opaque: no later step and no other
module recognizes anything inside them.

- a code span and a fenced or indented code block;
- an HTML token, an HTML block, and a `Comment` produced by either grammar;
- a formula body, inline or block;
- an inline or block `%%` comment;
- a completed cross link;
- the source of an angle-bracket autolink; and
- the run of a bare URL or `www.` autolink, from its first byte to its
  terminator.

Opacity is by ownership, not by region: a pair of matching inline HTML tags,
a pair of comments, or two cross links do not make the bytes between them
opaque. Escapes inside an opaque construct are the construct's own bytes and
are decoded only where its module says so.

## Failure and fallback

- Recognition failure is local and transactional. A candidate that does not
  complete consumes nothing; the cursor returns to its first byte and the next
  alternative in the recognition order runs from there. A failed candidate
  therefore never removes a bracket, fence, delimiter, or attribute container
  from a later construct.
- Fallback is to the next alternative of the recognition order at the same
  position and, when no alternative claims the bytes, to the inherited
  grammar, unless the module states another output.
- A construct that would exceed a limit below is not recognized; its opener
  bytes are literal at their position and parsing continues after them.
- Allocation failure aborts the whole `Document.parse` with the platform
  allocation error and publishes no partial document. No module publishes a
  partially constructed node, side table, or registry entry.
- A category violation reported by the C facade, such as a block kind in inline
  content, fails `Document.parse` on that binding with the platform
  contract-violation error and returns no document.

## Limits

These limits change output when exceeded and are therefore part of the
dialect. Changing one is a behavior change.

| Limit                                                             | Value         | Effect when exceeded                                                                   |
| ----------------------------------------------------------------- | ------------- | -------------------------------------------------------------------------------------- |
| footnote container depth (`MAX_FOOTNOTE_DEPTH`)                   | 100           | a definition at that depth or greater is not a definition; its line is ordinary content |
| link and footnote label length (`MAX_LINK_LABEL_LENGTH`)          | 1000 bytes    | a longer label is not a label; the brackets are ordinary bracket text                  |
| code span backtick string length (`MAXBACKTICKS`)                 | 80            | a longer backtick string is never a code span delimiter and is text; cmark shares the ceiling, so the cmark gate sees no divergence |
| directive label bracket nesting                                   | 32            | a label with a 33rd nested `[` is not a label; the directive has no label              |
| decimal list marker and example counter digits                    | 9             | a longer digit run is not a marker                                                     |
| Roman list marker value                                           | 999999999     | a numeral of greater value, whatever its components, is not a marker; the line is ordinary content |
| image dimension value                                             | 2147483647    | a larger value yields no dimensions; the whole label stays alt content                 |
| recognized metadata fields per block                              | 10            | the fixed field set is unique; unknown, invalid, and duplicate members are ignored      |
| autocompleted pipe-table cells per table (`MAX_AUTOCOMPLETED_CELLS`) | 524288     | once the synthesized empty cells exceed it, the next line is not a row and ends the table |
| pipe-table cells per row                                          | 65535         | a header or delimiter row with more cells is not a table; a body row with more cells ends the table before it |

Inline delimiter nesting and block container depth have no fixed syntax limit.
Deep nesting follows the shared parsing and allocation-failure rules. The
parser, transports, and bindings use stack-safe traversal; deep delimiter
size-doubling cases, nested emphasis/strong stress cases, and ten-thousand-level
nested-list conformance cases protect this behavior.

## Unicode and text

- The native parser bundles Unicode 17 tables, and every surface uses the
  native result. Categories named by a module are those tables' general
  categories: a letter is Lu, Ll, Lt, Lm, or Lo; a number is Nd, Nl, or No; a
  combining mark is Mn, Mc, or Me; connector punctuation is Pc; punctuation is
  the CommonMark 0.31.2 definition, the P and S categories; whitespace is the
  CommonMark Unicode whitespace set, Zs plus U+0009, U+000A, U+000C, and
  U+000D; ASCII whitespace is U+0009 through U+000D and U+0020.
- Case folding is the bundled full case-fold table; lowercase mapping is the
  simple lowercase mapping with no special casing and no locale. Updating
  either table is a behavior change.
- Reference labels, footnote labels, and every other "normalized label" use the
  inherited CommonMark normalization: full Unicode case fold, ASCII whitespace
  trimmed, internal ASCII whitespace runs collapsed to one space, compared byte
  for byte.
- Line endings are LF, CR, or CRLF, identical to the inherited line splitter.
  A line ending stored inside a literal is stored as written unless the module
  states a normalization.
- Adjacent `Text` nodes in one content array are merged into one node spanning
  from the first's start to the last's end, so `a\*b` is one `Text` node.

## Scopes

Coordinates are defined by [`canonical-ast.md`](canonical-ast.md#coordinates):
one-based lines, one-based byte columns, inclusive ends, tabs as one byte, the
`L:0` end sentinel for a block that closes with a line ending it consumed, and
a scope that never includes the line ending that terminates its last line,
except that `SoftBreak` and `LineBreak` cover the line-ending bytes they
stand for. Every `Markup` carries a scope. Besides `Markup`, exactly `Citation`,
`Footnote`, `Specimen`, and `Metadata` carry a scope, because they are
written; every other value is located by its owner's scope and has none.

Delimiters belong to the node they delimit: a scope covers the opening and
closing delimiters of its construct and everything between, and a child's
scope ends before a suffix its parent removed from visible content. A resolved
reference keeps the range of its own occurrence and never acquires the range
of the definition it resolved through.

## Conformance obligations

Every module ends with a required-cases section, and its examples are its
first required cases. A feature lands only with:

- package fixtures for every example of the module, byte for byte, and for
  every required case, including every malformed boundary, exact scopes,
  allocation failure at every
  allocation, and a size-doubling case, defined in
  [`test-architecture.md`](test-architecture.md);
- a canonical case in `specs/canonical-ast/` for every new kind, enum value,
  and nullable state, so that `scripts/check-canonical-ast-fixtures.mjs` sees
  every declared kind produced;
- the registration of every oracle delta it creates and the retirement of every
  oracle gap it closes, in the same change; and
- one row of the feature table above flipped to `present`.

A case that composes the syntax of two features is owned by whichever of the
two lands later; the landing plan lists those cases.
