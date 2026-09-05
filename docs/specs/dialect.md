# Markdown Core dialect

Status: normative. This document and the feature modules under
[`dialect/`](dialect/) define the Markdown Core dialect: the complete set of
syntax the parser recognizes, the option that enables each piece of it, the
external implementation that is locked as evidence for it, and the rules that
hold across all of it. `canonical-ast.json` and its prose companion
[`canonical-ast.md`](canonical-ast.md) remain the contract of the implementation
as it stands today; where a module describes a kind, field, value, or option
that the implementation does not have yet, its status line names the landing
item in the [landing plan](../plans/2026-09-04-canonical-vnext-landing-plan.md)
that adds it, and until that item merges the current contract stands.

## What the dialect is

Markdown Core is one Markdown dialect. Its base is CommonMark 0.31.2. On that
base it recognizes a fixed set of syntax features absorbed from GitHub Flavored
Markdown, the remark/micromark extension family, Obsidian Flavored Markdown,
Pandoc's Markdown, and `markdown-it-ins`. Every feature is one parse option;
the caller composes features freely. There are no profiles, presets, umbrella
switches, or dialect modes: `obsidian`, `pandoc`, `gfm`, and the like are names
of sources, not of anything the parser accepts.

The upstream tools are sources and evidence, never authorities over behavior:

- A source defines which feature exists and what its common-case source form
  looks like. The module that adopts the feature is the sole normative statement
  of its grammar, its AST, its option behavior, its fallback, and its scopes.
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
| Pandoc's Markdown                                   | Pandoc 3.11 at `b913622e1ff87c69ab8b1a606577122e220925cd`, `specs/oracles/pandoc/source.json` | the official Pandoc 3.11 CLI with `--to=json`, `specs/oracles/pandoc/` | `pnpm check:pandoc-parity` (lands with plan item `P0`) |
| `markdown-it-ins`                                   | 4.0.0 with `markdown-it` 13.0.2, `docs/specs/dialect/inserted-text.md`         | `markdown-it` with the plugin, `specs/oracles/markdown-it-ins/`      | `pnpm check:ins-parity` (lands with plan item `I0`) |

Each oracle is locked to the features whose rows below name it. An oracle has
no opinion outside those features: cmark-gfm does not judge CommonMark, Pandoc
does not judge inherited syntax, and remark-obsidian does not judge callouts or
block identifiers, which it does not implement. Every gate is fail-closed: an
unregistered difference fails it, and a registered difference that stops
reproducing fails it too.

## Features and options

Option names are the binding spelling. The C facade spells the same names in
`snake_case`, and the CLI `-e` names and the fixture `ts_ast_enable` tags are
the `snake_case` spellings. Every option is a boolean; the eight inherited
options default to `true` and every other option defaults to `false`. `Status`
is `present` when the implementation recognizes the syntax and produces the
module's model, `partial` when it recognizes the syntax but produces a model a
landing item still changes, and `missing` otherwise; the item named is the one
that makes the row `present`.

| Feature                            | Module                                                        | Option                            | Default | Source                | Executable oracle                  | Status                          |
| ---------------------------------- | ------------------------------------------------------------- | --------------------------------- | ------- | --------------------- | ---------------------------------- | ------------------------------- |
| CommonMark blocks and inlines      | [base](dialect/base.md)                                       | none                              | on      | CommonMark            | cmark                              | present                         |
| smart punctuation                  | [base](dialect/base.md)                                       | `smartPunctuation`                | `true`  | cmark `--smart`       | cmark                              | present                         |
| HTML comments as `Comment`         | [comments](dialect/comments.md)                               | none                              | on      | CommonMark, Obsidian  | cmark for the token boundaries     | missing, `M0`                   |
| pipe tables                        | [tables](dialect/tables.md)                                   | `tables`                          | `true`  | GFM                   | cmark-gfm                          | partial, `M6`                   |
| strikethrough                      | [strikethrough](dialect/strikethrough.md)                     | `strikethrough`                   | `true`  | GFM                   | cmark-gfm                          | present                         |
| autolinks                          | [links and images](dialect/links-and-images.md)               | `autolinks`                       | `true`  | GFM                   | cmark-gfm                          | present                         |
| task lists                         | [task lists](dialect/task-lists.md)                           | `taskLists`                       | `true`  | GFM                   | cmark-gfm                          | partial, `M5`                   |
| footnotes                          | [footnotes](dialect/footnotes.md)                             | `footnotes`                       | `true`  | GFM                   | cmark-gfm, remark                  | partial, `M4`                   |
| formulas                           | [formulas](dialect/formulas.md)                               | `formulas`                        | `true`  | GitHub math, remark   | remark (`micromark-extension-math`) | present                         |
| directives                         | [directives](dialect/directives.md)                           | `directives`                      | `true`  | remark-directive      | remark                             | partial, `M7`                   |
| resolved reference links and images | [links and images](dialect/links-and-images.md)              | none                              | on      | CommonMark            | cmark                              | missing, `M1`, `M2`             |
| universal anchor field             | [anchors](dialect/anchors.md)                                 | none                              | on      | Pandoc, Obsidian      | Pandoc                             | missing, `M7`                   |
| universal attributes field         | [attributes](dialect/attributes.md)                           | none                              | on      | Pandoc                | Pandoc                             | missing, `M7`                   |
| cross links and embeds             | [cross links](dialect/cross-links.md)                         | `crossLinks`                      | `false` | Obsidian              | remark-obsidian                    | missing, `O1`                   |
| marks                              | [marks](dialect/marks.md)                                     | `marks`                           | `false` | Obsidian              | remark-obsidian                    | missing, `O2`                   |
| `%%` comments                      | [comments](dialect/comments.md)                               | `comments`                        | `false` | Obsidian              | remark-obsidian                    | missing, `O3`                   |
| inline footnotes                   | [footnotes](dialect/footnotes.md)                             | `inlineFootnotes`                 | `false` | Obsidian, Pandoc      | none; product fixtures             | missing, `O4`                   |
| task markers                       | [task lists](dialect/task-lists.md)                           | `taskMarkers`                     | `false` | Obsidian              | remark-obsidian                    | missing, `O5`                   |
| properties                         | [properties](dialect/properties.md)                           | `properties`                      | `false` | Obsidian              | `yaml`                             | missing, `O6`                   |
| block identifiers                  | [block identifiers](dialect/block-identifiers.md)             | `blockIdentifiers`                | `false` | Obsidian              | none; product fixtures             | missing, `O7`                   |
| callouts                           | [callouts](dialect/callouts.md)                               | `callouts`                        | `false` | Obsidian              | none; product fixtures             | missing, `M3`, `O8`             |
| image dimensions                   | [links and images](dialect/links-and-images.md)               | `imageDimensions`                 | `false` | Obsidian              | none; product fixtures             | missing, `O9`                   |
| inserted text                      | [inserted text](dialect/inserted-text.md)                     | `insertedText`                    | `false` | `markdown-it-ins`     | `markdown-it-ins`                  | missing, `I1`                   |
| inline code attributes             | [attributes](dialect/attributes.md)                           | `inlineCodeAttributes`            | `false` | Pandoc                | Pandoc                             | missing, `P2a`                  |
| heading attributes                 | [attributes](dialect/attributes.md)                           | `headingAttributes`               | `false` | Pandoc                | Pandoc                             | missing, `P2b`                  |
| fenced code attributes             | [attributes](dialect/attributes.md)                           | `fencedCodeAttributes`            | `false` | Pandoc                | Pandoc                             | missing, `P2c`                  |
| link attributes                    | [attributes](dialect/attributes.md)                           | `linkAttributes`                  | `false` | Pandoc                | Pandoc                             | missing, `P2d`                  |
| automatic anchors                  | [anchors](dialect/anchors.md)                                 | `autoAnchors`                     | `false` | Pandoc, GFM algorithm | Pandoc                             | missing, `P3`                   |
| implicit heading references        | [anchors](dialect/anchors.md)                                 | `implicitHeadingReferences`       | `false` | Pandoc                | Pandoc                             | missing, `P4`                   |
| bracketed spans                    | [bracketed spans](dialect/bracketed-spans.md)                 | `bracketedSpans`                  | `false` | Pandoc                | Pandoc                             | missing, `P5`                   |
| superscript and subscript          | [superscript and subscript](dialect/superscript-and-subscript.md) | `superscript`, `subscript`    | `false` | Pandoc                | Pandoc                             | missing, `P6`                   |
| citations                          | [citations](dialect/citations.md)                             | `citations`                       | `false` | Pandoc                | Pandoc                             | missing, `P7`                   |
| fenced divs                        | [fenced divs](dialect/fenced-divs.md)                         | `fencedDivs`                      | `false` | Pandoc                | Pandoc                             | missing, `P8`                   |
| fancy lists                        | [lists](dialect/lists.md)                                     | `fancyLists`                      | `false` | Pandoc                | Pandoc                             | missing, `P9a`                  |
| example lists                      | [lists](dialect/lists.md)                                     | `exampleLists`                    | `false` | Pandoc                | Pandoc                             | missing, `P9b`                  |
| definition lists                   | [definition lists](dialect/definition-lists.md)               | `definitionLists`                 | `false` | Pandoc                | Pandoc                             | missing, `P10`                  |
| table captions                     | [tables](dialect/tables.md)                                   | `tableCaptions`                   | `false` | Pandoc                | Pandoc                             | missing, `P11a`                 |
| simple tables                      | [tables](dialect/tables.md)                                   | `simpleTables`                    | `false` | Pandoc                | Pandoc                             | missing, `P11b`                 |
| multiline tables                   | [tables](dialect/tables.md)                                   | `multilineTables`                 | `false` | Pandoc                | Pandoc                             | missing, `P11c`                 |
| grid tables                        | [tables](dialect/tables.md)                                   | `gridTables`                      | `false` | Pandoc                | Pandoc                             | missing, `P11d`                 |

The options `smartPunctuation`, `footnotes`, `tables`, `strikethrough`,
`autolinks`, `taskLists`, `formulas`, and `directives` are the inherited
options. `stripHTMLComments` is removed by `M0` and has no successor; nothing
in the dialect strips anything. Pandoc's `startnum` is not an option: a list's
start number is always the value of its first marker. Pandoc's
`compact_definition_lists` is not an option: compact and loose definitions are
two source forms of `definitionLists`. Every option is independent, except
that a feature which extends inherited syntax is additionally gated by that
syntax's inherited option: `inlineFootnotes` requires `footnotes`,
`taskMarkers` requires `taskLists`, the `\|` rule of cross links requires
`crossLinks` and the table option that parses the cell, and `$` inside every
feature is gated by `formulas`.

The `--profile` names of the CLI (`commonmark`, `commonmark-smart`, `gfm`,
`gfm-smart`, `gfm-extended`, `default`) are harness shorthands that select
option sets for the cmark and cmark-gfm comparison oracles. They define no
language, no module refers to them, and no source-named shorthand is added.

## Ground rules

These rules hold in every module. A module that needs an exception states it
explicitly.

- An option that is off leaves the inherited behavior byte for byte. Turning an
  option on changes the parse of source that the inherited grammar already
  accepts only where the module states the exact rule.
- An option that is on always recognizes its syntax. No option publishes two
  representations of one semantic fact, and no compatibility alias survives a
  model change.
- Values are stored as written. "As written" means no lowercasing, aliasing,
  trimming, slugging, or derivation of one field from another, unless the
  module states the transformation; the shared escape and character-reference
  decoding of the grammar that produced the value still applies. Matching,
  resolution, and rendering are consumer policy.
- A comment is a `Comment` node and is never stripped: an HTML comment under
  the inherited grammar and a `%%` comment under `comments`. A consumer that
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

## Recognition order

Recognition is one pass of the shared block parser followed by one pass of the
shared inline parser over each inline container, in the order the two tables
below fix. No module rescans a completed node, runs a regular expression over
finished source, or repairs a tree in a post-pass; the two post-passes named in
the tables are the only exceptions and each is bounded to the nodes it names.
Each module's own section names the step it occupies; the tables are the only
statement of cross-module precedence.

Within class A the leftmost opener in source order wins and its matched region
is opaque to every later class and to every later class-A candidate inside it.
A class-A candidate that fails consumes nothing: the cursor returns to the
candidate's first byte and the next alternative runs from there.

### Inline

| Step | Construct                                                            | Option                             | Class                                                                |
| ---- | -------------------------------------------------------------------- | ---------------------------------- | -------------------------------------------------------------------- |
| A1   | backslash escape                                                     | inherited                          | scanner                                                              |
| A2   | code span                                                            | inherited                          | scanner, opaque                                                      |
| A3   | raw HTML token, HTML comment as `Comment`, angle-bracket autolink    | inherited, `autolinks` for the autolink | scanner, opaque token bytes                                     |
| A4   | formula `$`, `$$`, `` $`...`$ ``, `\\(`, `\\[`                       | `formulas`                         | scanner, opaque                                                      |
| A5   | inline comment `%%...%%`                                             | `comments`                         | scanner, opaque                                                      |
| A6   | cross link `[[...]]`, `![[...]]`                                     | `crossLinks`                       | scanner, opaque                                                      |
| A7   | inline footnote `^[...]`                                             | `inlineFootnotes` and `footnotes`  | scanner, body parsed                                                 |
| A8   | citation key `@key`, `-@key`                                         | `citations`                        | scanner; a bare key with no bracketed tail is finalized document-wide |
| A9   | example reference `(@label)`                                         | `exampleLists`                     | scanner, finalized document-wide                                     |
| A10  | text directive `:name[...]{...}`                                     | `directives`                       | scanner                                                              |
| A11  | character reference                                                  | inherited                          | scanner                                                              |
| B0   | defined footnote call `[^label]`                                     | `footnotes`                        | bracket close, first                                                 |
| B1   | link and image tails, then an attribute container                    | inherited, `linkAttributes`        | bracket close                                                        |
| B2   | `[...]{attrs}` span                                                  | `bracketedSpans`                   | bracket close                                                        |
| B3   | `[@key...; ...]` cite group                                          | `citations`                        | bracket close                                                        |
| B4   | shortcut reference                                                   | inherited                          | bracket close, last                                                  |
| C1   | `*`, `_` emphasis and strong                                         | inherited                          | delimiter stack                                                      |
| C2   | `~~` strikethrough                                                   | `strikethrough`                    | delimiter stack                                                      |
| C3   | `~` subscript, or single-tilde strikethrough                         | `subscript`, `strikethrough`       | delimiter stack                                                      |
| C4   | `^` superscript                                                      | `superscript`                      | delimiter stack                                                      |
| C5   | `==` mark                                                            | `marks`                            | delimiter stack                                                      |
| C6   | `++` insert                                                          | `insertedText`                     | delimiter stack                                                      |
| D    | attribute suffix at every attachment site                            | per option                         | immediately after its owner                                          |
| E    | GFM bare autolink                                                    | `autolinks`                        | post-pass over `Text` only                                           |
| F    | smart punctuation                                                    | `smartPunctuation`                 | post-pass over `Text` only                                           |

Classes B and C are the inherited bracket and delimiter algorithms of
CommonMark, extended with the listed steps. Class B is the ordered procedure
run at every unescaped `]` that matches an active bracket opener; the first
success owns the pair and a failed alternative leaves the cursor at the `]`.
Class C is the delimiter stack: each listed delimiter is a run of units that
open or close under the flanking rules its module states, matched by the
inherited process-emphasis algorithm.

### Block starts

| Step | Block start                                                              | Option                                                                              |
| ---- | ------------------------------------------------------------------------ | ----------------------------------------------------------------------------------- |
| 0    | properties envelope, first line of the document only                     | `properties`                                                                        |
| 1    | container prefixes of open containers                                    | inherited                                                                           |
| 2    | fenced and indented code, HTML block, an HTML comment block as `Comment` | inherited                                                                           |
| 3    | formula block `$$` and `\\[` lines                                       | `formulas`                                                                          |
| 4    | block comment `%%` line                                                  | `comments`                                                                          |
| 5    | block quote, becoming `Callout`, with metadata on its first line         | inherited, `callouts`                                                               |
| 6    | list markers, including fancy and example markers                        | inherited, `fancyLists`, `exampleLists`                                             |
| 7    | container and leaf directive `:::name`, `::name`                         | `directives`                                                                        |
| 8    | fenced div `::: {...}`, `::: word`                                       | `fencedDivs`                                                                        |
| 9    | ATX heading, with `headingAttributes`                                    | inherited                                                                           |
| 10   | Setext heading                                                           | inherited                                                                           |
| 11   | tables: caption-prefixed, pipe, grid, multiline, simple                  | `tableCaptions` for the prefix, `tables` for pipe, `gridTables`, `multilineTables`, `simpleTables` |
| 12   | thematic break                                                           | inherited                                                                           |
| 13   | footnote definition, reference definition                                | `footnotes`, inherited                                                              |
| 14   | definition list                                                          | `definitionLists`                                                                   |
| 15   | block identifier line `^id`                                              | `blockIdentifiers`                                                                  |
| 16   | paragraph, with the `^id` suffix at finalization                         | inherited                                                                           |

A block start is tested at the first non-space byte of the line after the open
containers' prefixes have been consumed. Steps 2 through 15 are tested in
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
- a formula body, inline or block, under `formulas`;
- an inline or block `%%` comment under `comments`;
- a completed cross link under `crossLinks`; and
- the source of an angle-bracket or bare autolink.

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
- Fallback is to the inherited grammar at the same position, exactly as if
  the failed candidate's option were off for those bytes, unless the module
  states another output.
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
| inline delimiter nesting depth (`MARKDOWN_CORE_MAX_INLINE_DEPTH`) | 256           | further delimiter units at that depth are literal text                                 |
| footnote container depth (`MAX_FOOTNOTE_DEPTH`)                   | 100           | a definition at greater depth is not a definition; its line is ordinary content        |
| link and footnote label length (`MAX_LINK_LABEL_LENGTH`)          | 1000 bytes    | a longer label is not a label; the brackets are ordinary bracket text                  |
| directive label bracket nesting                                   | 32            | a label with a 33rd nested `[` is not a label; the directive has no label              |
| decimal list marker and example counter digits                    | 9             | a longer digit run is not a marker                                                     |
| image dimension value                                             | 2147483647    | a larger value yields no dimensions; the whole label stays alt content                 |
| properties alias expansion                                        | 1048576 bytes | a payload whose expanded alias occurrences exceed the budget invalidates the candidate |
| completed pipe-table cells per table                              | 524288        | the next line ends the table                                                           |
| completed pipe-table cells per table                              | 524288        | the next line ends the table                                                           |

Block container depth is not limited: the parser, every transport, and every
binding are stack-safe at any depth, and the conformance suites prove it with
a ten-thousand-level nested list.

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
one-based lines, one-based byte columns, inclusive ends, tabs as one byte, and
a scope that never includes the line ending that terminates its last line.
Every `Markup` carries a scope. Besides `Markup`, exactly `Citation`,
`Footnote`, `Metadata`, and `MetadataRecord` carry a scope, because they are
written; every other value is located by its owner's scope and has none.

Delimiters belong to the node they delimit: a scope covers the opening and
closing delimiters of its construct and everything between, and a child's
scope ends before a suffix its parent removed from visible content. A resolved
reference keeps the range of its own occurrence and never acquires the range
of the definition it resolved through.

## Conformance obligations

Every module ends with a required-cases section. A feature lands only with:

- package fixtures for every required case, including the option-off case for
  every documented form, every malformed boundary, exact scopes, allocation
  failure at every allocation, and a size-doubling case, defined in
  [`test-architecture.md`](test-architecture.md);
- a canonical case in `specs/canonical-ast/` for every new kind, enum value,
  and nullable state, so that `scripts/check-canonical-ast-fixtures.mjs` sees
  every declared kind produced;
- the registration of every oracle delta it creates and the retirement of every
  oracle gap it closes, in the same change; and
- one row of the option table above flipped to `present`.

A case that composes the syntax of two features is owned by whichever of the
two lands later; the landing plan lists those cases.
