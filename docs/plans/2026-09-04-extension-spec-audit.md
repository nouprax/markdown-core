# Extension specification audit

Status: audit with a resolution checklist. It covers every file under
`docs/specs` at the head of `claude/feature-specs-plan-xlt8nu` after the ground
rules of the [landing plan](2026-09-04-canonical-vnext-landing-plan.md) were
recorded. The goal it measures against is a closed, well-defined extension set
in which every input has exactly one specified output on every surface.
Byte-for-byte agreement with Pandoc, Obsidian, or any other oracle is not a
goal; a specification that states its own rule and registers the oracle
difference is.

## Verdict

The thirty-two specification files do not yet define that set. Each module
states its own grammar well enough for its examples, but the set as a whole
leaves inputs with two or more possible outputs in the places listed below, and
several rules every module relies on are written nowhere. Nothing found here is
unfixable, and most resolutions are one or two sentences; the checklist at the
end sequences them by file.

| Section                    | Files | Entries |
| -------------------------- | ----: | ------: |
| Shared contracts           |    12 |     109 |
| Obsidian modules           |    10 |      76 |
| Pandoc modules             |    10 |     110 |
| Seams between modules      |    32 |      13 |
| Structural gaps            |     — |      10 |
| Decisions to settle        |     — |       9 |

The structural gaps, each of which many findings reduce to:

- **X-1 No recognition order across families.** The Obsidian integration module
  orders Obsidian constructs against inherited syntax, the Pandoc index orders
  nothing, and no file orders Obsidian against Pandoc against inserted text
  against the nine existing options. Every pairwise question below, such as
  `^[a](b)^`, `[^a]{.x}`, `H~2~O` with `subscript` on, or `:::warning` with
  `directives` on, is answered nowhere. The proposed order is in the two tables
  under "Recognition order".
- **X-2 Grammars that exist only in code or upstream.** The directive envelope
  (`:`, `::`, `:::`, names, labels, closing fences), the five formula delimiters
  and their mapping to `Formula` and `FormulaBlock`, the scope coordinate
  semantics (base, byte versus code point, inclusivity, tabs, line endings), the
  GFM footnote grammar as this parser implements it, and the shared attribute
  ABNF's undefined nonterminals (`line-ending`, `unicode-letter`,
  `unicode-number`, `escaped-punctuation`, `character-reference`,
  `permitted-line-ending`).
- **X-3 Oracle coupling.** Twelve normative statements define behavior by
  reference ("as Pandoc does", "matching Pandoc's recoverable behavior", "the
  inherited GFM footnote grammar", "the official help snapshot is normative",
  "the `markdown-it-ins` result is the syntax oracle"), and the GFM layer names
  two authorities that disagree on single-tilde strikethrough.
- **X-4 The option set is not closed.** `canonical-ast.md` says `ParseOptions`
  "contains exactly these booleans" while the modules add twenty-two; the
  profiles `default`, `commonmark`, `gfm`, `gfm-extended`, and `obsidian` are
  named but never defined, and the CLI has two more; module options are spelled
  in two conventions; `four_space_rule` is referenced but does not exist; what
  the `obsidian` switch composes with the inherited gates is unstated.
- **X-5 Current and target contracts are not labeled.** `canonical-ast.json` is
  the contract for the implementation as it stands, while the shared contracts
  and profile modules describe the target the landing plan sequences. Their
  status lines ("normative target contract") do not say which is in force, so a
  reader finds two attribute models, two link models, three footnote models, and
  three `Document` shapes with no statement of which is current. These are
  transitional and are resolved by the landing-plan items named beside each
  finding; the fix here is one status sentence per file.
- **X-6 Spec text that still contradicts the settled ground rules.** Cells
  normalized to a `Paragraph`, fenced-code language lowercasing and aliasing,
  synthesized paragraphs and anchors that carry scopes, a "frozen" manifest
  order, and the emoji step already removed.
- **X-7 The dump grammar cannot print the target model.** It has no encoding for
  universal fields, tagged values, doubles, arrays of non-Markup values, scoped
  non-Markup values, or nested collections, and it never says whether adjacent
  `Text` nodes merge, so `children=N` is undetermined for some inputs today.
- **X-8 Unpinned Unicode and inherited versions.** Case folding, letter and
  number classes, whitespace, punctuation, and lowercase mapping are used by
  anchors, citations, labels, and delimiters without a pinned Unicode version;
  `test-architecture.md` names "latest CommonMark" as the inherited authority.
- **X-9 Limits and failure surfaces.** The inline depth limit (256), footnote
  depth limit (100), and label length limit (1000) change output when exceeded
  and are stated in no specification; allocation failure is described at three
  granularities; "parse error" is used as an outcome that `Document.parse` does
  not have.
- **X-10 Option-off and malformed-input outputs.** Many modules state only the
  option-on case, and several fallbacks say "inherited" for constructs whose
  inherited handling is itself unwritten.

## Method

Every file was read completely, in four passes: the shared contracts, the
Obsidian modules, the Pandoc modules, and a pass over the seams between them.
Each finding names the file and line, quotes the text, classifies the problem,
and proposes the rule to adopt rather than asking for clarification. Engine
facts that findings rely on were checked against the C sources: the footnote
label set picks no winner among duplicate definitions, the three limits above
are numeric constants, single-tilde strikethrough is accepted unless a CLI-only
flag is set, the case-fold table records no Unicode version, the task-list
scanner's separator class, the extended-autolink post-pass, and the single
implemented extension attach order (strikethrough, autolink, task list, formula,
directive, table).

Classes: **A** ambiguity (two readings, undefined terms, modal words where
behavior must be fixed); **B** underspecification (missing start or end
conditions, escapes, whitespace, precedence, option-off behavior, malformed
fallback, scopes); **C** non-determinism (order or environment dependence,
unpinned versions, unstated limits); **D** contradiction (within a file, between
files, against the canonical contract, or against a ground rule); **E** oracle
coupling (behavior defined only by reference to an upstream); **T** transitional
(a difference between the current contract and the target model that a named
landing-plan item resolves; listed so the status lines can say so).

The ground rules applied are the ones the landing plan records: `VERSION` stays
`3.0.0` and nothing is frozen before release; the dump prints every field as it
is; scope records original source, so written things have scopes and synthesized
things do not; `TableCell.content` is `[Markup]` with no `Paragraph`
normalization; `obsidian` off is the inherited behavior byte for byte;
fenced-code info, language, and attributes are stored as written; there is no
emoji support; the reference expansion bound is an invariant.

## The closed extension set

This is the set the specifications describe once the findings are resolved.
Names are the binding spelling; the C facade uses `snake_case` and the CLI uses
the extension names. Every option is independent unless its row says it is gated
by another.

### Inherited base

| Layer      | Authority                                                                 | Determinism source                                                                 |
| ---------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| CommonMark | CommonMark 0.31.2 as implemented by the pinned cmark oracle               | cmark spec tests plus registered deltas in `specs/oracles/cmark/`                  |
| GFM        | cmark-gfm at the pinned version, with `specs/oracles/cmark-gfm/deltas.json` | GFM spec tests plus registered deltas; one authority, not "GFM 0.29 and cmark-gfm" |

### Options

| Option                     | Default | Family    | Defining text                          | Off means                                          | Open findings              |
| -------------------------- | ------- | --------- | -------------------------------------- | -------------------------------------------------- | -------------------------- |
| `smartPunctuation`         | `true`  | inherited | `canonical-ast.md`                     | no quote, dash, or ellipsis replacement            | CA-22                      |
| `footnotes`                | `true`  | inherited | `canonical-ast.md`, `obsidian/footnotes.md` | `[^x]` and `[^x]:` are text                   | CA-14, S-1, OF-1           |
| `stripHTMLComments`        | `true`  | inherited | `canonical-ast.md`                     | HTML comments retained as `HTML` or `HTMLBlock`    | CA-22, S-10                |
| `tables`                   | `true`  | inherited | `canonical-ast.md`, `pandoc/tables.md` | no pipe tables                                     | CA-11, S-5                 |
| `strikethrough`            | `true`  | inherited | `canonical-ast.md`                     | `~` runs are text                                  | S-7 (single tilde)         |
| `autolinks`                | `true`  | inherited | `canonical-ast.md`                     | no bare autolinks                                  | PA-7, ON-5                 |
| `taskLists`                | `true`  | inherited | `canonical-ast.md`, `obsidian/tasks.md` | `[ ]` prefixes are text                           | S-6, OT-1                  |
| `formulas`                 | `true`  | inherited | none                                   | every formula delimiter is text                    | CA-21 (grammar missing)    |
| `directives`               | `true`  | Remark    | `remark/attributes.md`                 | `:name` is text                                    | RM-2 (envelope missing), S-8 |
| `obsidian`                 | `false` | Obsidian  | `obsidian-flavored-markdown.md`        | inherited behavior byte for byte                   | OI-2, S-10                 |
| `stripObsidianComments`    | `true`  | Obsidian  | `obsidian/comments.md`                 | `Comment` nodes retained; no effect while `obsidian` is off | OC-5, OC-9        |
| `insertedText`             | `false` | inserted  | `inserted-text.md`                     | `+` runs are text                                  | IT-5, IT-6                 |
| `inlineCodeAttributes`     | `false` | Pandoc    | `pandoc/attributes.md`                 | the suffix is inline text                          | PA-2                       |
| `headerAttributes`         | `false` | Pandoc    | `pandoc/attributes.md`                 | the list remains heading text                      | PA-6, S-10                 |
| `fencedCodeAttributes`     | `false` | Pandoc    | `pandoc/attributes.md`                 | inherited info contract                            | PA-3, PA-4, PA-5           |
| `linkAttributes`           | `false` | Pandoc    | `pandoc/attributes.md`                 | the suffix is text                                 | PA-7, PA-8                 |
| `autoAnchors`              | `false` | Pandoc    | `pandoc/headings-and-anchors.md`       | `anchor` stays null unless explicit                | PH-1, PH-2, S-3            |
| `implicitHeaderReferences` | `false` | Pandoc    | `pandoc/headings-and-anchors.md`       | no virtual definitions                             | PH-7                       |
| `bracketedSpans`           | `false` | Pandoc    | `pandoc/bracketed-spans.md`            | `[x]{.a}` follows inherited bracket rules          | PS-1                       |
| `superscript`              | `false` | Pandoc    | `pandoc/superscript-and-subscript.md`  | `^` is text                                        | PU-1, S-7                  |
| `subscript`                | `false` | Pandoc    | `pandoc/superscript-and-subscript.md`  | `~` is text; inherited strikethrough applies       | PU-4, S-7                  |
| `citations`                | `false` | Pandoc    | `pandoc/citations.md`                  | `@` is text; footnote `Cite` nodes unaffected      | PC-1, PC-5, S-1            |
| `fencedDivs`               | `false` | Pandoc    | `pandoc/fenced-divs.md`                | `:::` lines are paragraph text or directives       | PF-1, S-8                  |
| `fancyLists`               | `false` | Pandoc    | `pandoc/lists.md`                      | inherited `N.` and `N)` only                       | PL-2, PL-7                 |
| `startnum`                 | `false` | Pandoc    | `pandoc/lists.md`                      | decision D-3                                       | PL-3                       |
| `exampleLists`             | `false` | Pandoc    | `pandoc/lists.md`                      | `(@)` is text                                      | PL-9, PC-12                |
| `definitionLists`          | `false` | Pandoc    | `pandoc/definition-lists.md`           | paragraphs                                         | PD-1, PD-2, S-8            |
| `tableCaptions`            | `false` | Pandoc    | `pandoc/tables.md`                     | caption paragraphs are not claimed                 | PT-5, S-5                  |
| `simpleTables`             | `false` | Pandoc    | `pandoc/tables.md`                     | inherited fallback                                 | PT-8 through PT-11         |
| `multilineTables`          | `false` | Pandoc    | `pandoc/tables.md`                     | inherited fallback                                 | PT-12, PT-13               |
| `gridTables`               | `false` | Pandoc    | `pandoc/tables.md`                     | inherited fallback                                 | PT-15 through PT-19        |

Not options: `compactDefinitionLists` (two source forms of `definitionLists`),
`four_space_rule` (referenced once, defined nowhere; deleted by PD-3), Pandoc
`yaml_metadata_block` (excluded), and the CLI-only double-tilde strikethrough
flag (S-7 decides whether it becomes an option or is removed).

### Profiles

A profile is a named `ParseOptions` value. Today only the CLI defines them, in
`packages/markdown-core/core/main.c`, and no specification does. The vectors as
implemented, which the canonical contract should state:

| Profile            | Extensions attached                                          | Options                                               |
| ------------------ | ------------------------------------------------------------ | ----------------------------------------------------- |
| `commonmark`       | none                                                         | none                                                  |
| `commonmark-smart` | none                                                         | `smartPunctuation`                                    |
| `gfm`              | table, strikethrough, autolink, task list                    | `footnotes`                                           |
| `gfm-smart`        | table, strikethrough, autolink, task list                    | `footnotes`, `smartPunctuation`                       |
| `gfm-extended`     | table, strikethrough, autolink, task list, formula, directive | `footnotes`                                           |
| `default`          | table, strikethrough, autolink, task list, formula, directive | `footnotes`, `smartPunctuation`, `stripHTMLComments`  |
| `obsidian`         | decision D-1                                                 | `obsidian`, `stripObsidianComments`, plus D-1         |

## Recognition order

These two tables are the order the findings imply. They belong in
`canonical-ast.md`, with every module's precedence paragraph replaced by a
pointer to them (S-0). An entry marked with a finding identifier is one the
files currently leave undecided; the finding states the rule this table assumes.
Within class A the leftmost opener in source wins and its matched region is
opaque to every later class.

### Inline

| Step | Construct                                              | Option                      | Class                          | Decided by |
| ---- | ------------------------------------------------------ | --------------------------- | ------------------------------ | ---------- |
| A1   | backslash escape                                       | inherited                   | scanner                        | CommonMark |
| A2   | code span                                              | inherited                   | scanner, opaque                | CommonMark |
| A3   | raw HTML token, `<autolink>`                           | inherited                   | scanner, opaque token bytes    | CommonMark |
| A4   | formula `$`, `$$`, `` $`...`$ ``, `\(`, `\[`           | `formulas`                  | scanner, opaque                | CA-21, S-7 |
| A5   | inline comment `%%...%%`                               | `obsidian`                  | scanner, opaque                | OC-1, S-7  |
| A6   | wikilink `[[...]]`, `![[...]]`                         | `obsidian`                  | scanner, opaque                | OW-1, OW-4, S-4 |
| A7   | inline footnote `^[...]`                               | `obsidian` and `footnotes`  | scanner, body parsed           | OF-2, S-1  |
| A8   | citation key `@key`, `-@key`                           | `citations`                 | scanner                        | PC-5, PC-9 |
| A9   | example reference `(@label)`                           | `exampleLists`              | scanner, document-wide         | PL-11, PC-12 |
| A10  | text directive `:name[...]{...}`                       | `directives`                | scanner                        | RM-2       |
| A11  | GFM bare autolink                                      | `autolinks`                 | post-pass over `Text` only     | ON-5, PA-7 |
| A12  | character reference                                    | inherited                   | scanner                        | CommonMark |
| B0   | defined footnote call `[^label]`                       | `footnotes`                 | bracket close, first           | S-1        |
| B1   | link and image tails, then `{attrs}`                   | inherited, `linkAttributes` | bracket close                  | PS-1       |
| B2   | `[...]{attrs}` span                                    | `bracketedSpans`            | bracket close                  | PS-1       |
| B3   | `[@key...; ...]` cite group                            | `citations`                 | bracket close                  | PS-1, PC-6 |
| B4   | shortcut reference                                     | inherited                   | bracket close, last            | PS-1       |
| C1   | `*`, `_` emphasis and strong                           | inherited                   | delimiter stack                | CommonMark |
| C2   | `~~` strikethrough                                     | `strikethrough`             | delimiter stack                | inherited  |
| C3   | `~` subscript, or single-tilde strikethrough           | `subscript`, `strikethrough`| delimiter stack                | S-7        |
| C4   | `^` superscript                                        | `superscript`               | delimiter stack                | S-7        |
| C5   | `==` highlight                                         | `obsidian`                  | delimiter stack                | OH-1       |
| C6   | `++` insert                                            | `insertedText`              | delimiter stack                | IT-5       |
| D    | attribute suffix at registry sites                     | per option                  | immediately after its owner    | PA-7       |
| E    | smart punctuation                                      | `smartPunctuation`          | remaining `Text`               | CA-22      |

### Block starts

| Step | Block start                                                   | Option                                 | Decided by      |
| ---- | ------------------------------------------------------------- | -------------------------------------- | --------------- |
| 0    | Properties envelope, first line only                          | `obsidian`                             | OP-1            |
| 1    | container prefixes                                            | inherited                              | CommonMark      |
| 2    | fenced and indented code, HTML block                          | inherited                              | CommonMark      |
| 3    | block comment `%%` line                                       | `obsidian`                             | OC-2, S-8       |
| 4    | block quote, becoming `Callout`, with metadata on line one    | inherited, `obsidian`                  | OK-1            |
| 5    | list markers, including fancy and example markers             | inherited, `fancyLists`, `exampleLists`| PL-8, PL-15     |
| 6    | container and leaf directive `:::name`, `::name`              | `directives`                           | RM-2, S-8       |
| 7    | fenced div `::: {...}`, `::: word`                            | `fencedDivs`                           | PF-1, S-8       |
| 8    | ATX heading, with `headerAttributes`                          | inherited                              | PA-6            |
| 9    | Setext heading                                                | inherited                              | PT-11, S-8      |
| 10   | tables: caption-prefixed, pipe, grid, multiline, simple       | `tables` and the Pandoc table options  | PT-5, PT-11, PT-14 |
| 11   | thematic break                                                | inherited                              | PT-11           |
| 12   | footnote definition, reference definition                     | `footnotes`, inherited                 | S-1, PC-14      |
| 13   | definition list                                               | `definitionLists`                      | PD-2, PD-9, S-8 |
| 14   | block identifier line `^id`                                   | `obsidian`                             | OB-2, S-5       |
| 15   | paragraph, with the `^id` suffix at finalization              | inherited                              | OB-1, S-7       |

## Findings by file

Each finding gives the location, its class, the problem, and the rule to adopt.
A transitional finding names the landing-plan item that resolves it.

### Shared contracts

#### `canonical-ast.md`

- **CA-1** `:5-9` — D — the invariants column has drifted from the JSON (`Link`
  lacks "never absent", `Image` lacks "absent and empty title remain distinct",
  `Formula.mode` has no type, `FormulaBlock` cites a note that does not exist)
  and the projection audit compares names only. Rule: the JSON invariant strings
  are the sole normative text and are byte-copied into the prose column; add the
  missing invariants to the JSON.
- **CA-2** `:36` — A — "MS-private syntax" is undefined. Rule: delete the
  phrase.
- **CA-3** `:45-47` — B — invalid UTF-8 has no specified output at the C entry
  point, and the text does not say that this is deliberate. Rule (decision D-5):
  state that valid UTF-8 is a caller precondition, that the C entry point's
  output for invalid input is unspecified, and that every binding guarantees
  valid UTF-8 before calling it.
- **CA-4** `:50-54` (also `canonical-ast-dump.md:69-70`, `metadata.md:181-182`)
  — E — scopes "inherit the native C parser's ... semantics exactly", but no
  written coordinate contract exists; base, unit, inclusivity, tabs, CRLF, the
  bytes covered by `SoftBreak` and `LineBreak`, and the empty document are all
  unstated. Rule: add to "Coordinates": `line` is 1-based and increments once
  per line ending (LF, CR, or CRLF); `column` is the 1-based byte index within
  the line; `start` is the first byte of the node's first code point and `end`
  the last byte of its last code point, inclusive; a tab is one byte; a node's
  scope never includes the line ending that terminates its last line;
  `SoftBreak` and `LineBreak` cover the line-ending bytes and, for `LineBreak`,
  the preceding backslash or trailing spaces; state the empty document's value.
- **CA-5** `:91` — B — "refuses any other value" has no observable meaning.
  Rule: returns `false` and leaves the node unchanged.
- **CA-6** `:95-116` — T (`M7`) — the directive attribute model (slot update,
  `null` versus `{}`, bare attributes, `#` and `.` as `id` and `class`)
  contradicts `attributes.md:52-53,74-76,122`, `anchors.md:55-56`, and
  `remark/attributes.md:59-62`. Rule: `M7` deletes this section and the
  `DirectiveAttribute` value; until then the status line says the shared model
  supersedes it when `M7` lands.
- **CA-7** `:102-107` — B — the attribute grammar here states no name character
  set, terminators, escapes, entities, or line-ending rules. Rule: replaced by
  the single grammar in `attributes.md` (CA-6).
- **CA-8** `:130-131` — B — the category-violation error is unnamed. Rule:
  `Document.parse` fails with the platform contract-violation error and returns
  no document.
- **CA-9** `:142` — A/B — "raw info string" is ambiguous about escape and
  character-reference processing; when `info` is `null` rather than empty and
  the token separator set are unstated. Rule: `info` is the info string after
  CommonMark backslash-escape and character-reference processing and after
  stripping leading and trailing spaces and tabs, with no other transformation;
  `info` is `null` for indented blocks and for a fence whose info string is
  empty after stripping; `language` is the maximal prefix before the first space
  or tab, or `null` when `info` is `null`.
- **CA-10** `:142` — B — `closed` is undefined for fenced blocks. Rule: `closed`
  is `true` iff a closing fence line was found.
- **CA-11** `:145-146` — B — rows with fewer or more cells than the delimiter
  row have no specified `cells` count, and a padded cell would need a scope that
  no source supports. Rule: state the implemented behavior: `TableRow.cells`
  holds exactly the cells written in that row, and `Table.alignments.count` is
  the delimiter-row column count; register the difference from cmark-gfm padding
  and truncation as a delta. Decision D-6 if the implementation pads today.
- **CA-12** `:147` and `canonical-ast.json:218` — D (rule 2) — "inline content".
  Rule: `content` holds either only inline kinds or only block kinds as the
  owning table syntax states; pipe-table cells hold inline content; no cell is
  wrapped in a `Paragraph`.
- **CA-13** `:148` — B — `::x` and an empty `:::x` container are
  indistinguishable and the envelope grammar is absent (RM-2). Rule: a leaf
  block directive and an empty container produce identical `DirectiveBlock`
  values with `content=[]`.
- **CA-14** `:150,166` — B — footnote identifier normalization, the winner among
  duplicates, unreferenced definitions, unresolved `[^x]`, and precedence over
  reference definitions are unstated. Rule: S-1 owns; the current-model
  statement is that `identifier` is `^` plus the label normalized as
  `ReferenceDefinition.identifier`, every definition is emitted at its position,
  a `[^label]:` line is never a reference definition, `[^label]` is a call iff a
  definition exists, and references match the first definition in source order.
- **CA-15** `:151` — C — the Unicode version and whitespace set behind "full
  Unicode case fold, trimmed, internal whitespace collapsed" are unpinned. Rule:
  whitespace is U+0009 to U+000D and U+0020; case folding is the bundled table
  at a pinned Unicode version recorded in `core/case_fold.inc` (X-8).
- **CA-16** `:151` versus `attributes.md:167-168` and `destinations.md:97-98` —
  T (`M2`) — a public `ReferenceDefinition` leaf versus "parser-owned, not
  emitted". Rule: after `M2` the definition is parser state; until then the
  status lines say so.
- **CA-17** `:161` — A — "(Q26)" is an undefined reference. Rule: delete.
- **CA-18** `:161,163` versus `destinations.md:50-51,95-97,113-114` — T (`M2`)
  plus B — `LinkReference` versus resolved `Link`, and neither says what an
  unresolved `[foo]` becomes. Rule: an unmatched reference is inherited literal
  text (state it in both models).
- **CA-19** `:168-169` and `canonical-ast.json:17-21` versus `anchors.md:16-17`
  and `attributes.md:37` — T (`M7`) — the universal fields are absent from the
  contract and the dump. Rule: `inheritedField` becomes the ordered list
  `scope`, `anchor`, `attributes`, and the dump prints them (CD-6).
- **CA-20** `:189-202` — T (`X0`) plus B — "exactly these booleans" versus the
  twenty-two module options. Rule: the table is the single option registry;
  every option enters it with its default and its off rule (the closed set
  above).
- **CA-21** `:204-208` — B — no file states which formula delimiter yields
  `Formula(embedded)`, `Formula(standalone)`, or `FormulaBlock`, nor flanking,
  escaping, opacity, or the block fence rule. Rule: add a formulas module
  stating: `$...$` gives `Formula(mode=embedded)`; `$$...$$` inside inline
  content gives `Formula(mode=standalone)`; a block whose first line begins with
  `$$` at indentation 0 to 3 and ends at the first later line ending with `$$`
  gives `FormulaBlock`; `\(...\)` gives embedded and `\[...\]` standalone; ``
  $`...`$ `` gives embedded with the code span's body; an opening `$` must be
  followed by a non-whitespace code point and a closing `$` must be preceded by
  non-whitespace and not followed by an ASCII digit; bodies are opaque and are
  recognized at their opening delimiter during the scan, before the delimiter
  pass; `\$` is literal. Derive the exact rules from `extensions/formula.c` and
  cite them as implemented behavior.
- **CA-22** `:194,197` — B — the effect of `smartPunctuation` and
  `stripHTMLComments` on the AST is stated nowhere. Rule: `smartPunctuation`
  replaces, in `Text` literals only, `"` and `'` by the cmark flanking rule,
  `--` by U+2013, `---` by U+2014, and `...` by U+2026, changing no node
  boundaries; `stripHTMLComments` omits an inline `HTML` node whose literal is
  exactly one comment and an `HTMLBlock` whose literal is exactly one comment,
  keeps the parent even when emptied, and merges `Text` nodes separated only by
  an omitted comment.
- **CA-23** `:209-210` — A — "retained" leaves open whether destinations are as
  written or unescaped. Rule: `destination`, `source`, and `title` are the
  CommonMark-unescaped values with angle-bracket wrappers removed and no
  percent-encoding or normalization.
- **CA-24** `:126-166` — B — no rule says whether adjacent `Text` nodes merge,
  so `children=N` is undetermined for inputs such as `a\*b`. Rule: adjacent
  `Text` nodes in one content array are always merged into one node spanning
  from the first's start to the last's end (state the implemented behavior).
- **CA-25** `:222-234` and `canonical-ast.json:8` — B — the walk does not say
  whether it descends into Markup arrays nested inside value types
  (`Citation.prefix` and `suffix`). Rule: the walk descends into Markup arrays
  nested in value-typed fields in the value's declared field order, and value
  types receive no callbacks (or receive value callbacks; decision D-4).
- **CA-26** `:24` versus the value types — D (rule 5) — which value types carry
  `scope` is never stated as a rule. Rule: add to "Core rules": besides
  `Markup`, exactly `Citation`, `Footnote`, `Metadata`, and `MetadataRecord`
  carry `scope`, because they are written; every other value is located by its
  owner's scope.
- **CA-27** `:204-205` — B — no statement of the order in which extension passes
  apply, so overlapping candidates such as `++www.a.com++` or `~~$x~~$` have no
  fixed output. Rule: the recognition-order tables (X-1).

#### `canonical-ast.json`

- **CJ-1** `:15` — A — `DirectiveAttribute` is listed under `enums` and its
  fields live only in prose. Rule: move to a `valueTypes` object with named
  fields, or delete under CA-6.
- **CJ-2** `:17-21` — T (`M7`) — `inheritedField` is singular (CA-19).
- **CJ-3** `:148,170,334` — A — "mode is standalone" names a field the kind does
  not have. Rule: a per-kind `placement` property, and remove the phrase from
  `invariants`.
- **CJ-4** `:101` — A — "only for" is not "iff". Rule: `start` is non-null if
  and only if `flavor == ordered`.
- **CJ-5** `:218` — D (rule 2) — see CA-12.
- **CJ-6** `:397-437` — T (`M1`) — `destination: String` and `source: String`
  versus `dest: Destination`.
- **CJ-7** `:415,436` — D — JSON invariants omit rules the prose has (CA-1).
- **CJ-8** `:462,488` — T (`M2`) — see CA-18.
- **CJ-9** `:22-527` — T — omits `Insert`, `Cite`, `CrossLink`, `Mark`,
  `Footnote`, and `Document.metadata`; each lands with its item. Rule: the
  status lines of the defining files say "not yet in `canonical-ast.json`; lands
  with item N".

#### `canonical-ast-dump.md`

- **CD-1** `:25,31` — A — read literally, a kind with no fields prints two
  consecutive spaces; the example shows one. Rule: tokens are separated by
  exactly one space; a kind with no fields prints `Kind scope=... children=N`.
- **CD-2** `:44-47` — A — "structural children" is undefined, `Table.header`
  counts while `Directive.label` does not, and `List.items` and `TableRow.cells`
  are not mentioned. Rule: a table per kind: `content.count` for content-bearing
  kinds, `items.count` for `List`, the head, body, and foot row count for
  `Table`, `cells.count` for `TableRow`, and zero for leaves and `Directive`.
- **CD-3** `:56` — A — "JSON string escaping" is not one canonical form. Rule:
  escape `"` and `\`, use the short escapes for U+0008, U+0009, U+000A, U+000C,
  and U+000D, `\u00XX` with lowercase hex for every other code point below
  U+0020, and raw UTF-8 for everything else.
- **CD-4** `:59-60` — A — whether enum elements inside arrays are quoted. Rule:
  `alignments=[none,left]`; enum elements are unquoted inside arrays.
- **CD-5** `:61-63` — B — the non-null directive attribute sequence syntax is
  undefined and encodes the slot model. Rule: deleted under CA-6; universal
  fields print per CD-6.
- **CD-6** `:64-65,88-110` — T (`M7`) plus rule 4 — `anchor` and `attributes`
  are printed nowhere. Rule: immediately after `scope`, every node prints
  `anchor=<string or null> classes=[...] records=[["k","v"],...]`; kind-specific
  fields follow.
- **CD-7** `:69-70` — E — coordinates by reference (CA-4).
- **CD-8** `:88-110` — T — the table lacks `Insert`, `Cite`, `CrossLink`,
  `Mark`, and `Metadata`; each lands with its item. Rule (X-7): tagged values
  print as `dest=url("...")`, `dest=cross(path="...",anchor=null)`,
  `referent=bib(key="...",mode=normal)`, `referent=footnote(id="...")`,
  `value=scalar(text("..."))`, and `value=list([...])`; every scoped owned value
  (`Citation`, `Footnote`, `TableCaption`, `Definition`, `Metadata`,
  `MetadataRecord`) prints as a nested line and `children=N` counts every
  directly nested line; doubles print as the shortest round-trip decimal;
  `Definition.content: [[Markup]]` prints each body as a nested `DefinitionBody`
  line.
- **CD-9** `:115-119` — B — the example is the only place end-inclusive 1-based
  columns can be inferred. Rule: promote to normative text (CA-4).
- **CD-10** `:124` — A — the maintenance command is unnamed. Rule: name it.

#### `anchors.md`

- **AN-1** `:3` (and every "normative target contract" status line) — A/T —
  "target" does not say whether the file is in force. Rule (X-5): "Status:
  normative target; supersedes `canonical-ast.json` when landing-plan item N
  merges; until then the current contract stands."
- **AN-2** `:16-17` — T (`M7`) — see CA-19.
- **AN-3** `:57` — A/E/B — "Pandoc/GFM automatic heading-anchor synthesis"
  delegates the algorithm and gate; under rule 6 synthesis must be off with no
  Pandoc switch. Rule: automatic heading anchors are synthesized only when
  `autoAnchors` is true, by the algorithm in `pandoc/headings-and-anchors.md`;
  with no profile switch on, `anchor` is `null` on every node.
- **AN-4** `:71-73` — T (`P2d`) — reference-occurrence inheritance presupposes
  copying definition metadata, which the current contract forbids. Rule: state
  that inheritance exists only for `linkAttributes` definitions once `P2d`
  lands.
- **AN-5** `:76-81` — A — synthesis is whole-document (later explicit anchors
  affect earlier headings) but does not say so. Rule: synthesis runs after block
  and inline parsing of the whole document completes.
- **AN-6** `:84` — A/D — "may produce diagnostics", but there is no diagnostics
  channel. Rule: the parser emits no diagnostics; both declarations keep their
  values.
- **AN-7** `:104-106` versus `destinations.md:103-104` and `metadata.md:229-230`
  — D — allocation failure aborts "the owning parse operation", "construction of
  the owning reference node", and "the document parse". Rule (X-9): allocation
  failure aborts the whole `Document.parse` with the platform allocation error
  and no partial document is published.
- **AN-8** `:117` (also `attributes.md:209`, `destinations.md:117`,
  `inserted-text.md:171`) — A — "size-doubling" is undefined. Rule: define once
  in `test-architecture.md`: a size-doubling case parses inputs of n and 2n
  bytes and asserts a fixed AST shape and a bounded output-to-source ratio.

#### `attributes.md`

- **AT-1** `:37` — T (`M7`) — see CA-19.
- **AT-2** `:52-53` and `:74-75` — T (`M7`) — `{}` versus absent, and
  duplicates, versus the current directive model (CA-6).
- **AT-3** `:70-71` — C — "Unicode whitespace-delimited words" is
  version-dependent. Rule: split on U+0009 to U+000D, U+0020, and category Zs of
  the pinned Unicode version; drop empty words.
- **AT-4** `:84-109` — B/C — the ABNF uses `escaped-punctuation`,
  `character-reference`, `permitted-line-ending`, `line-ending`,
  `unicode-letter`, and `unicode-number` without defining them. Rule:
  `line-ending = LF / CR / CRLF`; `permitted-line-ending` is a line ending not
  followed by a blank line and inside the extent the owning profile grants the
  container; `escaped-punctuation` is a backslash followed by an ASCII
  punctuation character; `character-reference` is a CommonMark named or numeric
  reference decoded per CommonMark; `unicode-letter` is Lu, Ll, Lt, Lm, Lo;
  `unicode-number` is Nd, Nl, No; all under the pinned Unicode version.
- **AT-5** `:95,133-138` — A — the quoted and unquoted alternation is unordered
  and the prose is modal. Rule: if the first character after `=` is a quote and
  a matching closing quote exists before the end of the container, the value is
  the quoted value; otherwise it is the unquoted value; no other backtracking.
- **AT-6** `:135-136` — B — whether the grammar runs over raw source or the
  block's inline content string. Rule: the grammar operates on the owning
  block's inline content string after block-structure processing; a permitted
  line ending becomes one space.
- **AT-7** `:134` — A (rule 3 boundary) — character-reference decoding versus
  "as written". Rule: define once: "as written" means no lowercasing, aliasing,
  or class derivation; the shared grammar's escape and reference decoding still
  applies.
- **AT-8** `:153` — E — "follows Pandoc's `combineAttr`" without saying which
  wins on divergence. Rule: the three numbered steps are normative; the function
  is cited for provenance only.
- **AT-9** `:167-168` — T (`M2`) — see CA-16.
- **AT-10** `:197` — D — allocation granularity (AN-7).
- **AT-11** `:10-16` — E — official native and JSON output listed as authority
  for a grammar the file writes out. Rule: the grammar here is normative; a
  disagreement with the pinned reader is a registered delta.

#### `destinations.md`

- **DE-1** `:17-29` — T (`M1`) — `dest: Destination` versus the current
  `destination` and `source` strings.
- **DE-2** `:31-36` — T (`O1`) — `CrossLink` is not yet in the contract.
- **DE-3** `:50-51,95-97,113-114` — T (`M2`) — see CA-18.
- **DE-4** `:53-55` — E — fallback for `[[]]`, `[[#]]`, and `[[|x]]` is
  delegated to the wikilink module. Rule: the wikilink module states the exact
  inherited fallback for every source form that would violate these invariants
  (OW-1).
- **DE-5** `:62-64` — A — "may preserve a hierarchy such as `Parent#Child`".
  Rule: `anchor` is every byte after the first `#` of the target; if those bytes
  begin with `^`, that one `^` is removed; nothing else is transformed.
- **DE-6** `:103-104` — D — allocation granularity (AN-7).
- **DE-7** `:112` — A — "rejection of invalid combinations" with no public
  constructor. Rule: the conformance suite asserts the parser never emits
  `cross(path="", anchor=null)` or an empty non-null anchor.

#### `citation-model.md`

- **CI-1** `:12-34` — T (`M4`, `P7`) — the kinds are not yet in the contract and
  no switch enables citations.
- **CI-2** `:41-43` — B — the walk has no callback for the non-Markup
  `Citation`, so item boundaries and affix roles are unobservable. Decision D-4.
- **CI-3** `:46,62-65` — T (`M4`) — three footnote models coexist
  (`FootnoteDefinition` and `FootnoteReference` in content, a
  `Document.footnotes` side table, and `Cite(footnote(id))`); whether `id` keeps
  the caret is unstated (S-1). Rule: `footnote.id` and `Footnote.id` never
  contain the source caret.
- **CI-4** `:45` — E/B — key normalization is delegated and `Citation.scope`
  boundaries are unstated. Rule: S-1 owns the scope rule: `Cite.scope` covers
  the complete group including delimiters; `Citation.scope` covers the item's
  own bytes and excludes group delimiters and separators.
- **CI-5** `:79-80` — A — "rejection" (DE-7).

#### `metadata.md`

- **ME-1** `:20-25` — T (`M4`, `M7`) — `Document(content, metadata, footnotes)`
  versus the current contract and `obsidian/footnotes.md:34-38`. Rule: the JSON
  owns `Document(content, metadata: Metadata?, footnotes: [Footnote])` in that
  order; both modules cite it.
- **ME-2** `:12-13` — E — "follows that documented Obsidian domain" grants the
  external page precedence. Rule: the rules in this file are the sole normative
  text; the page is provenance.
- **ME-3** `:77-79` — A — explicit keys (`? key`), anchored keys, and tagged
  keys are neither accepted nor rejected. Rule: a key is directly authored iff
  it is a plain, single-quoted, or double-quoted scalar carrying no tag, anchor,
  or explicit-key indicator; any other key form invalidates the block.
- **ME-4** `:101-105,139,162-164` — B — accepted payloads for tagged scalars.
  Rule: a tagged scalar is valid iff its decoded content is exactly what the
  plain form of that branch requires: `!!null` empty or `null`; `!!bool` `true`
  or `false`; `!!int` `-?(0|[1-9][0-9]*)`; `!!float` the number grammar; `!!str`
  any single-line scalar, yielding `text` even for `null`, `true`, or numeric
  spellings.
- **ME-5** `:111-113,242-243` — A — whether folded multi-line source that
  decodes to one line is supported. Rule: support is decided on the decoded
  value only; any form whose decoded content contains no U+000A or U+000D is
  supported.
- **ME-6** `:119-120` — A — "Checkbox" is undefined. Rule: `bool` and `null`
  items invalidate the block.
- **ME-7** `:179-184` — E — the envelope grammar is delegated and this is the
  only text fixing end-inclusive scopes. Rule: cite `obsidian/properties.md`
  explicitly and make CA-4 normative.
- **ME-8** `:186-190` — B — record scope end for flow mappings, trailing
  comments, and empty values. Rule: the record scope starts at the first byte of
  the key including quotes and ends at the last non-whitespace byte of the
  value's last owned line, excluding trailing comments, flow separators, and the
  line ending; for an empty value it ends at the colon.
- **ME-9** `:226-227` (also `inserted-text.md:128`,
  `test-architecture.md:213-215`) — C — "shared structural limits" are numeric
  and output-affecting but unstated. Rule (X-9): a shared limits section in
  `canonical-ast.md`: inline delimiter nesting 256, footnote container depth
  100, link label length 1000, the alias expansion budget and metadata record
  limit as implemented; exceeding a metadata limit invalidates the candidate;
  exceeding the inline limit makes further delimiter units literal.
- **ME-10** `:132` — B — payload bytes outside YAML `c-printable`. Rule: such a
  payload invalidates the candidate.

#### `inserted-text.md`

- **IT-1** `:3,121-122` — T (`X0`, `I1`) — the switch is absent from
  `ParseOptions` and its default is not stated. Rule: `insertedText` defaults to
  `false`.
- **IT-2** `:24-28` — T (`I1`) — `Insert` is not yet in the contract.
- **IT-3** `:12-13,173` versus `:89-90` — E — the algorithm is normative and the
  `markdown-it-ins` result is "the syntax oracle" without saying which wins.
  Rule: the delimiter and composition sections are normative; oracle
  disagreements are registered deltas, never rule changes.
- **IT-4** `:57-58` — C — CommonMark 0.30 and 0.31 define punctuation
  differently and the version is unpinned here. Rule: Unicode punctuation is
  CommonMark 0.31.2 section 2.1 (P* and S*) and whitespace is Zs plus U+0009,
  U+000A, U+000C, U+000D, under the pinned Unicode version.
- **IT-5** `:70-71` — B — a unit that can both open and close has no stated
  precedence. Rule: such a unit is first tried as a closer against the nearest
  eligible opener; if none matches it stays on the stack as a potential opener.
- **IT-6** `:82-87` — A — an odd run of five or more whose units both close and
  open is covered by neither sentence. Rule: the literal `+` of an odd run is
  placed after all of that run's closing units and before all of its opening
  units; units that match nothing are text and merge with adjacent text.
- **IT-7** `:108-112` — B — autolinks, extended autolinks, formulas, directive
  envelopes, and the composed `Mark`, `CrossLink`, and `Cite` are not addressed
  for opacity. Rule: the recognition-order tables; `Mark` is owned by
  `obsidian/highlights.md`, `CrossLink` by `obsidian/wikilinks-and-embeds.md`,
  and `Cite` by `citation-model.md`.
- **IT-8** `:128` — C — "normal inline nesting limit" is undefined (ME-9).

#### `test-architecture.md`

- **TA-1** `:3,93-94,206-207,315-316` — D (rule 7) — the manifest paths,
  options, order, and vocabulary are described as frozen. Rule: replace
  "freezes" with "records" and add that until 3.0.0 is released manifest order,
  paths, coverage vocabulary, kind ordinals, and wire layouts may change in any
  reviewed change.
- **TA-2** `:121` — C/A — "latest CommonMark" is time-dependent and conflicts
  with a GFM 0.29 baseline. Rule: inherited CommonMark is 0.31.2 as implemented
  by the pinned cmark; GFM is cmark-gfm at the pinned version; goldens change
  only by reviewed change.
- **TA-3** `:233-240` — E/C — oracles are named as authorities and "latest
  stable" is unpinned. Rule: oracles are evidence, the module text is the rule,
  and each divergence is a registered delta.
- **TA-4** `:213-215` — B — the 10,000-deep nesting case interacts with unstated
  limits (ME-9).
- **TA-5** `:249-250` — C (test infrastructure) — a timeout range instead of
  per-test values. Rule: state each `TIMEOUT`.
- **TA-6** `:207` — B — the manifest must carry every `ParseOptions` field once
  `X0` adds switches. Rule: the projection audit fails on a missing field.

#### `remark.md` and `remark/attributes.md`

- **RM-1** `remark.md:3` — A/T — status line (AN-1).
- **RM-2** `remark.md:11-12` and `remark/attributes.md:9-12` — E (major) — "the
  upstream remark-directive project owns the directive envelope syntax"; no file
  states the envelope grammar, so `directives`, an option that defaults to on,
  has no written definition. Rule: add `remark/directives.md`: a name is an
  ASCII letter followed by ASCII alphanumerics, `-`, or `_`, not ending in `-`
  or `_`; a text directive is `:` name, an optional `[label]` of
  balanced-bracket inline content with backslash escapes and no blank line, and
  an optional container; a leaf directive is a line of at most three spaces of
  indentation, `::` name, optional label and container, optional trailing
  whitespace; a container opener is at most three spaces, a run of at least
  three colons, name, optional label and container, optional trailing
  whitespace, whose content is block content and which closes at the first later
  line of at most three spaces consisting of exactly that many colons; an
  unclosed container ends where its enclosing container ends and yields the same
  node; any other byte on an opener line makes it paragraph text; no space is
  allowed between the colons and the name.
- **RM-3** `remark.md:26-28` — B — no module row for the envelope or for
  `directives` off. Rule: add the row.
- **RA-1** `remark/attributes.md:20-25,57-62` — T (`M7`) — `{}` versus absent
  and the `hasAttributes` bit versus the current model (CA-6).
- **RA-2** `:35-36` — A — the order of name, label, and container (`:x{a=b}[c]`)
  is only inferable. Rule: the order is name, optional label, optional
  container; a `[` after a container is text; only one container may attach.
- **RA-3** `:66-68` — E/A — line-crossing containers are delegated to the
  remark-directive scanner. Rule: in an inline directive the container may span
  lines as the shared grammar allows; in leaf and container block directives the
  container must close on the opener line, otherwise it is invalid.
- **RA-4** `:74-77` — B — for block directives the leftover opener-line bytes
  have no home. Rule: for a leaf or container block directive an invalid
  container on the opener line makes the line paragraph text; for an inline
  directive the directive commits and the container bytes are inline text.
- **RA-5** `:79` — A — the switch is not named. Rule: "With `directives=false`".

### Obsidian index and modules

#### `obsidian-flavored-markdown.md`

- **OI-1** `:41-43` — E — the footnote grammar's owner is unnamed (GFM 0.29 has
  no footnote section), and the engine's label set "picks no winner" among
  duplicate definitions, so the precedence `obsidian/footnotes.md` calls
  inherited exists nowhere. Rule: name the pinned cmark-gfm footnote
  implementation for call and definition recognition, label normalization,
  continuation indentation of at least four columns, and the container depth
  limit of 100; duplicate precedence is defined by the footnote module (OF-1),
  not inherited.
- **OI-2** `:194-196` — B — the preset's option vector is never stated and the
  combination of `obsidian` with the inherited gates is undefined (`obsidian`
  with `tables`, `taskLists`, or `footnotes` off; `stripObsidianComments` with
  `obsidian` off). Rule: decision D-1 fixes the vector; each Obsidian rule is
  gated by `obsidian` and the inherited gate of the syntax it extends (task
  markers by `taskLists`; `^[...]`, `[^label]` lowering, and
  `Document.footnotes` by `footnotes`; the delimiter-row and `\|` rules by
  `tables`; `$` by `formulas`); wikilinks, highlights, comments, callout
  metadata, block identifiers, Properties, and image dimensions are gated by
  `obsidian` alone; `stripObsidianComments` has no effect while `obsidian` is
  off.
- **OI-3** `:136` versus `obsidian/inherited-and-integration.md:66,76-77` — D/A
  — the audit row says two-hyphen delimiter cells are already present under the
  inherited grammar while the integration module makes "at least two hyphens" a
  profile rule (S-5). Rule: the inherited delimiter-row grammar applies
  unchanged; two hyphens is a positive example, not a minimum.
- **OI-4** `:170-171` — B — the profile set is not closed (the CLI also has
  `commonmark-smart` and `gfm-smart`). Rule: the profiles table above, stated in
  `canonical-ast.md`.
- **OI-5** `:174-175` — D — `MetadataListItem` is missing from the value set.
  Rule: add it.
- **OI-6** `:163-165` — B — the output-affecting limits are unstated (ME-9).
- **OI-7** `:108-109` — B — the parser output for `[[## text]]` and `[[^^text]]`
  is underivable without OW-1. Rule: with OW-1, `[[## text]]` has an empty
  heading part and is text; `[[^^text]]` is `cross(path="^^text", anchor=null)`.
- **OI-8** `:186-188` — B — "autolink form" does not say whether GFM extended
  autolinks are resolved or what an email destination carries. Rule: both
  angle-bracket and extended autolinks; an email autolink's `Destination.url`
  carries the inherited `mailto:` prefix.
- **OI-9** `:208-211` versus `obsidian/properties.md:164-167` — D — no
  per-module toggle versus "unless they explicitly enable this same extension".
  Rule: delete the clause (OP-7).

#### `obsidian/wikilinks-and-embeds.md`

- **OW-1** `:11-15` — B — `heading-part`, `vault-path`, and `label` have no
  character sets, so `[[Note#]]`, `[[#]]`, `[[A##B]]`, `[[A#B#]]`, `[[a]b]]`,
  `[[a[b]]`, `[[[Note]]]`, `[[A#^id#x]]`, and `[[A#H#^id]]` have no derivable
  result. Rule: `path-char` is any scalar except `#`, `|`, `[`, `]`, LF, and CR;
  `vault-path` and `heading-part` are one or more path characters, with `^` an
  ordinary heading byte; `label` is any scalar except `[`, `]`, LF, and CR; the
  candidate ends at the first `]]` after the opener; a `[` or `]` before that
  makes the opener text and scanning resumes at the next byte, so `[[[Note]]]`
  is text `[`, a `CrossLink`, and text `]`; `#^` is a block anchor only when it
  immediately follows the path and its identifier runs to `|` or `]]`, otherwise
  the first `#` begins a heading anchor and a later `#^` is heading text; an
  empty heading part at any position makes the opener text.
- **OW-2** `:23-25` — B — `[[a\|b]]` outside a table and every other escape are
  undefined. Rule: inside `[[...]]` the pair `\|` is the label separator
  everywhere, the backslash is dropped, and no other backslash escape exists;
  `\[[Note]]` and `\![[Note]]` are inherited escapes, the latter yielding a
  literal `!` and an `embedded=false` `CrossLink`.
- **OW-3** `:20-21` — B — no trimming rule. Rule: `path`, `anchor`, and `label`
  are stored exactly as written.
- **OW-4** `:89-91` — B — precedence against inherited bracket syntax
  (`[[Note]](url)`, `[[Note]][ref]`, `[a [[N]]](u)`, `![alt [[N]]](u)`,
  `[[N]]{.c}`). Rule: at `[[` the wikilink scanner runs before inherited bracket
  handling; a recognized `CrossLink` is complete at its `]]` and a following
  `(`, `[`, or `{` is text; link content and image alt content may contain a
  `CrossLink`.
- **OW-5** `:81-87` — B — no option-off sentence. Rule: with `obsidian` off,
  `[[`, `]]`, and `![[` follow inherited bracket handling.
- **OW-6** `:63-64` — B — a `CrossLink` in a heading contributes nothing to an
  automatic anchor. Rule: it contributes `label` when non-null, otherwise `path`
  (PH-1).

#### `obsidian/highlights.md`

- **OH-1** `:10-12,32-37` — B — no open or close eligibility, so `if a == b and
  c == d`, `== ==`, `a==b==c`, and `==a==b==` have no derivable result. Rule: a
  candidate is a maximal run of unescaped `=` of length exactly two; it can open
  iff left-flanking and close iff right-flanking under the CommonMark
  definitions with the same classes as `*`; runs of one or three or more are
  text; matching uses the shared process-emphasis algorithm without the rule of
  three; intraword pairs are allowed; a matched pair with an empty region is
  text; `==a==b==` is `Mark("a")` then text `b==`.
- **OH-2** `:12` — A — `==%%c%%==` with stripping. Rule: validity is decided on
  source; `Mark.content` may be empty after stripping.
- **OH-3** `:33-35` — B — block pass and extended autolinks (`==a` then `==` as
  a Setext underline; `http://x/?a==b== c`). Rule: block structure is decided
  first, so a Setext underline is never a closer; extended autolinks run after
  delimiter processing over `Text` only, so a URL ends at a `Mark` boundary.
- **OH-4** `:39-42` — B — no option-off sentence. Rule: with `obsidian` off, `=`
  runs are text.

#### `obsidian/comments.md`

- **OC-1** `:10-12` — A — "the next unescaped `%%`" inside an opaque body is
  self-contradictory and runs of three or more percent signs have no rule. Rule:
  the opener is the first two `%` of a run not preceded by an unescaped
  backslash; the body ends at the first later `%%`; backslashes inside the body
  are ordinary bytes; `%%%%` is an empty comment and `%%%a%%%` is
  `Comment("%a")` followed by text `%`.
- **OC-2** `:14-17` — B — no block start condition (indentation bound, tabs,
  paragraph interruption, container prefixes, lazy lines, blank lines, the
  decision before commit). Rule: after container prefixes, a line whose content
  is `%%` at indentation zero to three followed only by spaces or tabs opens a
  candidate that may interrupt a paragraph; the candidate scans forward,
  interpreting no block syntax, for the first later line whose content is
  exactly `%%` under the same indentation and prefixes, with intervening lines
  carrying the prefixes and possibly blank; if found it commits as a block
  `Comment` whose literal is the intervening lines after prefix removal,
  otherwise the opener line is paragraph text; any other `%%` is an inline
  comment whose body may span lines of one inline container but not a block
  boundary; an unmatched opener is text.
- **OC-3** `:11-12` versus `:16-17` — D — block markers are opaque only inside a
  committed block comment; an inline comment cannot cross a block boundary, so
  in `%%`, `# h`, `end %% x` the heading is a heading and both `%%` are text.
- **OC-4** `:30-31` versus `canonical-ast.md:128-131` — D — `Comment` is the one
  kind valid in both inline and block content; state it in both files.
- **OC-5** `:33-37` — B — the stripped AST is not fixed. Rule: stripping removes
  only the `Comment` node; adjacent `Text` siblings are not merged; a container
  whose content becomes empty is kept with `content=[]`; a removed block
  `Comment` leaves no node; no scope changes.
- **OC-6** `:43-44` — A — "suppresses every other inline and block extension"
  excludes inherited constructs. Rule: suppresses all inline and block
  recognition, inherited and extension, until its closer.
- **OC-7** `:28-29` — B — container prefixes and CRLF inside the literal. Rule:
  the literal is the source between the delimiters after container-prefix
  removal; line endings are stored as written.
- **OC-8** missing — B — table cells are split before inline parsing. Rule:
  table boundary scanning does not recognize comments; a `|` inside `%%...%%` in
  a table row splits the cell and the unmatched `%%` bytes are text; `\|` inside
  a comment in a table becomes `|` in the literal.
- **OC-9** `:33-34` — B — `stripObsidianComments` with `obsidian` off. Rule: no
  effect.

#### `obsidian/footnotes.md`

- **OF-1** `:16-18,93,99-100` — E — "the inherited GFM footnote grammar" and
  "the winning inherited definition" name a rule that exists nowhere. Rule:
  restate the grammar here: a definition is at most three spaces, `[^`, a label
  of one or more scalars other than `]`, space, tab, and `[`, then `]:`, with
  continuation lines indented at least four columns, recognized only when
  `footnotes` is on and at container depth below 100; the key is the label
  normalized exactly as `ReferenceDefinition.identifier`; when two definitions
  share a key the first in source order wins, each later one is parsed in place
  as ordinary blocks beginning with the literal `[^label]:`, produces no
  `Footnote`, and calls resolve to the winner; only a literal caret opens a
  call.
- **OF-2** `:20-27` — B — bracket precedence (`^[a](b)`, `x^[^1]`, `[a ^[b]
  c](d)`, `^[see [x](y)]`, `^a^[b]^`, `\^[x]`). Rule: an unescaped `^`
  immediately followed by `[` pushes an inline-footnote opener onto the shared
  bracket stack; the `]` that matches it closes the footnote without attempting
  any link, reference, or attribute tail, so `^[a](b)` is a `Cite` followed by
  text `(b)`; a link opener inside the body is closed by its own `]` and tail; a
  footnote opener inside a link label is closed by the first `]`; at one `^` the
  inline footnote wins over a `[^label]` call and over superscript, so `^[^1]`
  is a footnote whose body is text `^1`; `\^[` never opens.
- **OF-3** `:24-25` — B — `^[ ]`. Rule: a body of only spaces and tabs is
  invalid and the opener is text.
- **OF-4** `:63-65,120-122` — D (rule 5) — the inline body is "normalized to one
  `Paragraph`" with a scope. Rule (S-1): `Footnote.content` is inline-or-block
  content; the inline form stores the parsed inline body directly; no
  `Paragraph` is synthesized.
- **OF-5** `:80-85` — A — the `inline-N` uniqueness set and nesting order. Rule:
  let A be the ids of every `Footnote` produced from a winning or unreferenced
  definition; inline footnotes are numbered by the start position of their `^[`,
  an outer footnote before a nested one; `inline-N` is final if it is not in A
  and not already assigned, otherwise `inline-N-K` for the smallest K of at
  least 1 in neither set.
- **OF-6** `:73-78` — C — the case-fold table records no Unicode version (X-8).
  Rule: record it; an update is a behavior change.
- **OF-7** `:138-140` — E — the "inherited literal fallback" for an unresolved
  call is unwritten. Rule: an unresolved call is parsed by inherited bracket
  handling as ordinary bracket text and may become a shortcut reference only
  when a link definition labelled `^label` exists; it never creates a
  `Footnote`.
- **OF-8** `:34-38` — D — the `Document` field list disagrees with
  `metadata.md:20-25` (ME-1).
- **OF-9** missing — B — `footnotes` off with `obsidian` on. Rule: `^[...]` and
  `[^...]` are text.
- **OF-10** `:131-136` — A — "already recognized" is order-dependent wording.
  Rule: replaced by OF-2.
- **OF-11** `:117-120` versus `citation-model.md:43` and
  `pandoc/citations.md:120-122` — D — three scope rules for one value (S-1).
  Rule: `Cite.scope` covers the complete group including delimiters and
  `Citation.scope` the item's own bytes; `[^label]` gives `Cite` `[^label]` and
  `Citation` `^label`; `^[content]` gives `Cite` and `Footnote` `^[content]` and
  `Citation` `content`.

#### `obsidian/block-identifiers.md`

- **OB-1** `:29-30` — B/A — tabs, trailing whitespace after the identifier,
  `\^id`, and the own-line form `text` then `^id`. Rule (decision D-2 accepts
  the own-line form): rule one matches when the paragraph's last line, after
  removing trailing spaces and tabs, ends with `^block-id` preceded by one or
  more spaces or tabs, or consists solely of `^block-id` at indentation zero to
  three when the paragraph has an earlier line; `\^` never starts an identifier;
  the removed bytes are the identifier, its caret, the preceding whitespace, and
  in the own-line form the preceding line ending.
- **OB-2** `:31-35` — B — the indentation bound, the number of blank lines, the
  container level, the owner among nested lists, and trailing whitespace. Rule:
  the identifier line is indented zero to three spaces, contains `^block-id`,
  and may end with spaces or tabs; one or more blank lines precede it and one or
  more blank lines or end of input follow it; the line, the blank lines, and the
  owner are direct content of the same container; the owner is the last block of
  that container before the blank lines and must be a `List`, `Callout`, or
  `Table`, the outermost list at that level for nested lists; otherwise the line
  is a paragraph.
- **OB-3** `:36-38` — A — "bullet-point content line" is undefined and rules one
  and three both match `- item ^id`. Rule: rule three applies when the item's
  first block is a `Paragraph` and the candidate is on the marker line; the
  anchor is stored on the `ListItem` and the `Paragraph` keeps `anchor=null`;
  otherwise rule one applies to the paragraph owning the final line.
- **OB-4** missing — B — `> [!note] Title ^id`. Rule: callout metadata is
  extracted before block-identifier attachment; a candidate on a metadata line
  is title text.
- **OB-5** `:66-69` — B — no option-off sentence. Rule: with `obsidian` off the
  bytes are ordinary content.
- **OB-6** `:53-55` — A — "a candidate that would assign a second anchor" cannot
  arise under the three rules. Rule: delete, or state the one applicable
  ordering: if another enabled rule already assigned the owner's anchor, the
  candidate is ordinary content.
- **OB-7** `:66` versus `:78-79` — D — attachment "while block ownership is
  finalized" versus opacity by inline owners known only after inline parsing (``
  a `b ^id `` with an unclosed backtick). Rule (S-7): the suffix rule is lexical
  on the final line; only block-level owners protect it; inline code, inline
  HTML, and inline comments do not.

#### `obsidian/callouts.md`

- **OK-1** `:31-32` — A — a blank first line, one to three extra spaces, or four
  or more spaces before `[!`. Rule: the candidate is the first line of the
  container after `>` and its optional space are removed; up to three further
  spaces may precede `[!`; a blank first line, four or more spaces, or any other
  leading byte means no metadata.
- **OK-2** `:35-38,42` — A — `[!note]Title` and `[!faq]+Title` match neither the
  grammar nor a failure sentence. Rule: `]` or the fold marker must be followed
  by a space, a tab, or end of line; otherwise the line is not a metadata line
  and the callout is metadata-free.
- **OK-3** `:43` — B — trailing spaces on a single-line container. Rule:
  trailing spaces and tabs are removed before the title is parsed; a title never
  contains `SoftBreak` or `LineBreak`.
- **OK-4** `:60-61` — B — the body paragraph after the metadata line. Rule: the
  paragraph that began on the metadata line is split; its remaining lines, lazy
  lines included, form a `Paragraph` whose scope starts at the first byte of the
  second line; with no remaining lines the body is empty.
- **OK-5** missing — B — a Setext underline after the metadata line. Rule:
  metadata is decided when the first line is consumed, before Setext resolution;
  a following underline belongs to the body.
- **OK-6** `:57-59` — A — `> [!note] %%t%%` with stripping. Rule: `title` is
  null iff no title bytes were authored; otherwise it is the parsed content and
  may be empty after stripping.
- **OK-7** `:15-21` — B — the fold enum is unnamed and the traversal position of
  `title` is unstated. Rule: the enum is `CalloutFold`; `title` is visited
  before `content` and is not counted in `children`.
- **OK-8** `:79-82` — B — no option-off sentence. Rule: with `obsidian` off,
  `[!type]` is paragraph text inside a metadata-free `Callout`.
- **OK-9** `:46-49,55-56` — E — `variant` is "ASCII-lowercased" because Obsidian
  matches case-insensitively. Decision D-7: store as written and leave matching
  to consumers, consistent with metadata keys.

#### `obsidian/tasks.md`

- **OT-1** `:13-15` — B/D — the inherited scanner's separator class is `[
  \t\v\f]`, so `- [x]` followed by a vertical tab is a task under `gfm` and not
  under the module's grammar; end of input without a newline is undecided. Rule:
  the separator is space, tab, vertical tab, form feed, or a line ending; end of
  input counts as a line ending.
- **OT-2** `:47-52` — A — `checked` and `isComplete` are two names for one
  property. Rule: bindings expose exactly `isTask` and `isComplete`.
- **OT-3** `:67-68` — D — "existing source-decoding contract" for invalid UTF-8
  versus the precondition. Rule: delete; a marker is one scalar of the
  caller-supplied UTF-8.
- **OT-4** missing — B — `taskLists` off with `obsidian` on. Rule: no task
  recognition (OI-2).

#### `obsidian/properties.md`

- **OP-1** `:27-35` — A — the payload grammar lets the closing fence share a
  line with payload bytes. Rule: the payload is zero or more lines, each any
  scalars except LF and CR and not exactly `---`.
- **OP-2** `:33` — B — a lone CR line ending. Rule: line endings are LF, CR, or
  CRLF, identical to the inherited splitter.
- **OP-3** `:108-115` — B — tagged scalar keys and the explicit-key indicator.
  Rule: a key written with `?` is accepted when its node is an untagged plain or
  quoted scalar; a key carrying any tag invalidates the candidate.
- **OP-4** `:130-131` — B — "line" for decoded escapes. Rule: a decoded text
  containing U+000A or U+000D invalidates the candidate; U+0085, U+2028, and
  U+2029 are ordinary characters.
- **OP-5** `:121-126` — A — read literally the plain-scalar row contradicts the
  rows above it. Rule: any other plain scalar, or any single- or double-quoted
  scalar, gives `scalar(text(...))`.
- **OP-6** `:143-145` — A — "unsupported" is not tied to the transactional
  wording. Rule: invalidates the complete candidate.
- **OP-7** `:164-167` — D — "unless they explicitly enable this same extension
  in the future" versus no per-module toggle. Rule: Properties recognition is
  enabled iff `obsidian` is on.
- **OP-8** `:171` — E — "The official help snapshot is normative". Rule: the
  snapshot is the source of requirements; this module and `metadata.md` are the
  normative statement and decide every case the snapshot does not.
- **OP-9** `:152-157` — C — acceptance depends on unstated limits (ME-9).

#### `obsidian/inherited-and-integration.md`

- **ON-1** `:66,75-77` — D/A — the two-hyphen rule (OI-3).
- **ON-2** `:65` — T (`M6`) — "the existing typed model" names the pre-vNext
  shape. Rule: Obsidian pipe tables produce `Table(caption=null, columns,
  head=[header row], content=[rows], foot=[])` with `TableColumn(alignment,
  relative=null)` and `TableCell(rowspan=1, colspan=1, content)` holding inline
  content.
- **ON-3** `:83-86,102-104` — C — "overflow" has no bound and `Int` differs per
  platform. Rule: `W` and `H` are ASCII digit strings with no leading zero and
  values from 1 to 2147483647; larger values keep the whole label as alt content
  on every surface; `Int` means a 32-bit signed integer.
- **ON-4** `:99-101` — B — the alt-pipe location, empty alt, raw bytes versus
  parsed content, pipes in code spans, and reference images. Rule: the suffix is
  matched against the raw source bytes between the last top-level unescaped `|`
  that is not inside a code span or nested brackets and the closing `]`; an
  empty alt before the pipe is allowed; the rule applies to direct and resolved
  reference images alike.
- **ON-5** `:21-27` — B — the extended-autolink post-pass is absent from the
  precedence list (`http://x/?a==b== c`, `www.x.com/[[y]]`, `%%http://x%%`).
  Rule: extended autolinks are recognized last, over `Text` nodes only, after
  delimiter processing and comment stripping, and never inside
  `Comment.literal`, `CrossLink`, `Code`, or `HTML`.
- **ON-6** `:136-146` — B — the five-step precedence omits Properties, block
  comments, the order of task prefix, callout metadata, and block identifier on
  one line, the choice at one `[`, and Setext or thematic-break lines against
  `==`, `^id`, and `%%`. Rule: replaced by the recognition-order tables (X-1).
- **ON-7** `:70-73` — B — the boundary scanner knows only `\|`. Rule: pipes
  inside code spans, comments, or wikilink source are cell boundaries unless
  escaped.
- **ON-8** `:112-114` — E — "the existing inline `Formula`" whose grammar exists
  only in code (CA-21).

### Pandoc index and modules

- **PX-1** `pandoc.md:7-10` — E — "The Pandoc User's Guide owns the source
  language" delegates the language to an external document, so wherever a module
  is silent the reader is invited to fill in Pandoc behavior. Rule: the modules
  in this directory are the sole normative statement of the accepted source
  language; the User's Guide, reader, and CLI are evidence; where a module is
  silent, behavior is undefined until the module is amended and is never
  inherited from Pandoc.
- **PX-2** `pandoc.md:15-16` — E — "Pandoc 3.11 also owns the one shared
  braced-attribute grammar" names a second authority for a grammar that
  `attributes.md` writes out in full. Rule: the grammar is defined by
  `attributes.md`; Pandoc 3.11 is its evidence.
- **PX-3** `pandoc.md:20-21` — D — "does not change inherited CommonMark/GFM
  behavior" is contradicted by `lists.md:78-79` (`startnum` off stores
  `start=1`), `lists.md:67-69` (nested-start restriction), `lists.md:71-72`
  (denies inherited `1)`), and `tables.md:198-200` (a simple table may claim a
  thematic break). Rule: with every option in this index off, output is
  byte-identical to the inherited profile; an enabled option may change the
  parse of source the inherited grammar already accepts only where its module
  states the exact rule.
- **PX-4** `pandoc.md:63` (also `bracketed-spans.md:44`, `citations.md:106`,
  `superscript-and-subscript.md:62`, `attributes.md:180`) — A — "comments" is
  undefined outside the `obsidian` profile. Rule: in this directory "comment"
  means a `Comment` recognized by the Obsidian comment rule; with
  `obsidian=false` no byte is a comment; HTML comments are HTML tokens.
- **PX-5** `pandoc.md:66-68` — B — "without consuming a bracket ... required by
  a later construct" is not decidable. Rule: on recognition failure the cursor
  returns to the candidate's first byte and the next alternative in the module's
  stated order, or the inherited rule, runs from that position; a failed
  candidate consumes nothing.
- **PX-6** `pandoc.md:68-70` — B/C — the "shared nesting limit" has no value and
  no overflow output; allocation failure has no defined surface. Rule: one
  shared limits paragraph (see X-9) that every module references; a construct
  that would exceed a limit is not recognized and its opener bytes are literal
  at that position; allocation failure aborts `Document.parse` with the shared
  allocation error and no document.

#### `pandoc/attributes.md`

- **PA-1** `:56-58` — D — "Table, row, and cell anchors are null" is absolute
  although `obsidian/block-identifiers.md:31-35,40` attaches `^id` to a `Table`.
  Rule: under the rules of this directory no table, row, cell, or caption
  receives an anchor or attributes; an enabled Obsidian block identifier may
  still populate `Table.anchor`.
- **PA-2** `:89-90` — D (rule 3) — "The first class is the conventional language
  and its sole stored authority; a binding may expose a derived language". Rule:
  classes on `Code` have no parser-side meaning and no surface derives a
  language from them.
- **PA-3** `:116-117` — D (rule 3) — "A single bare language word is shorthand
  for the first class"; `CodeBlock.info` with a container present is unstated.
  Rule: a bare word before the container is never an attribute member; when a
  valid container is the last non-whitespace content of the opening fence line,
  `info` is the inherited info string over the line with the container and the
  whitespace before it removed, `language` is its first token, both stored as
  written; `canonical-ast.md:142` is amended to say so.
- **PA-4** `:125-126` — D (rule 3) — lowercasing, `c++` to `cpp`, `objective-c`
  to `objectivec`, "sole stored language authority". Rule: delete both
  sentences; nothing is lowercased, aliased, or derived from a class.
- **PA-5** `:128-130` — B — where in the line the container may occur and what
  `info` is when it is malformed. Rule: the inherited fence rule is applied to
  the complete opening line first; a container is recognized only as the last
  non-whitespace content of that line; a container followed by other bytes, or a
  malformed container, is ordinary info-string text and attaches nothing.
- **PA-6** `:94-96` — B — order of container removal and the ATX closing-hash
  rule; empty heading. Rule: ATX: if the line's last non-whitespace bytes form a
  valid container, remove it and the whitespace before it, then apply the
  inherited closing-sequence rule to the remainder, and attach only if the first
  step succeeded; Setext: the first step applies to the last content line; `#
  {#x}` is a `Heading` with empty content and `anchor="x"`.
- **PA-7** `:134-136` — B — bare autolinks (`https://x.y{.a}`), a resolving
  shortcut reference with `bracketed_spans` on, and a container after an
  unresolved reference tail. Rule: attachment sites are a direct link or image
  tail, a full or collapsed reference tail that resolves, a shortcut reference
  that resolves only when `bracketedSpans` is off, and angle-bracket autolinks;
  a bare autolink never accepts a container and its inherited termination rule
  applies to the braces; when the link fails the container is released to text
  and decided by the bracket procedure (PS-1).
- **PA-8** `:148-149` — B — position grammar of a definition-side container.
  Rule: with `linkAttributes` on the definition grammar is destination, optional
  title, optional spaces or tabs, at most one line ending plus optional spaces
  or tabs, one container, then whitespace to the line ending; a malformed
  container, or the option off, leaves the inherited grammar unchanged, so the
  line is then not a definition; the container is normalized once when the
  definition is stored.
- **PA-9** `:171-173` — A/B — "may be unitless pixels or use px, cm..." and the
  typed `Image.width: Int?` of the Obsidian integration module. Rule: the parser
  validates nothing and stores any `width` or `height` value as a `Record`; the
  integer `Image.width` and `Image.height` fields are populated only by the
  Obsidian alt-suffix rule; a Pandoc record never populates them and vice versa.
- **PA-10** `:177-178` — A — "Link/Image attachment precedes Span fallback,
  bracketed Span precedes shortcut-reference fallback" gives two answers for
  `[label]{.x}` with `label` defined. Rule: the ordered procedure in PS-1.
- **PA-11** `:198` — D (rule 3) — required cases "language aliases; image
  units". Rule: remove "language aliases"; replace "image units" with
  "unit-bearing width and height records stored verbatim".
- **PA-12** `:150-151` — transitional — "stored in the parser's reference map
  rather than emitted as a public node" while the current contract still has a
  public `ReferenceDefinition`; resolved by `M2`.

#### `pandoc/bracketed-spans.md`

- **PS-1** `:31-38` — B — no single ordered decision at a closing `]`. Rule: at
  an unescaped `]` matching an active opener, test in order and take the first
  success, a failed alternative leaving the cursor at the `]`: a valid direct
  tail `(...)` gives `Link` or `Image`; a full `[label]` or collapsed `[]` tail
  whose label resolves, explicitly or virtually, gives `Link` or `Image`; with
  `bracketedSpans` on, a valid container starting at the byte after `]` gives
  `Span`; with `citations` on, a valid cite group gives `Cite`; a resolving
  shortcut reference not followed by `[]` or a link label gives `Link` or
  `Image`; otherwise the inherited literal fallback applies and any container
  after it is text; for an image opener `![` the span, cite, and shortcut steps
  yield a literal `!` followed by the node; attachment to the link results
  follows PA-7.
- **PS-2** `:40-42` — B — the bracket pair after a malformed container. Rule: a
  malformed container fails the span step; the cite, shortcut, and fallback
  steps then apply to the same pair and the `{` is text.
- **PS-3** `:44` — A — "comments" (PX-4).
- **PS-4** missing — B — Remark inline directives (`directives` defaults on) and
  `[[wiki]]{.x}`. Rule: an inline directive envelope claims its `[label]` and
  container before the bracket procedure runs; a `Span` may occur inside a
  directive label; `CrossLink` is not a `Link`, so a container after `]]` is
  text.
- **PS-5** `:35-38` — B — the `citations` on, `bracketedSpans` off case lives
  only in the landing plan. Rule: with `bracketedSpans` off, `[@foo]{.key}` is a
  bracketed `Cite` followed by literal `{.key}`.

#### `pandoc/citations.md`

- **PC-1** `:22-23` vs `:29-32` — A — the grammar admits `Foo_bar--baz` and
  trailing `.`; the prose forbids them. Rule: `key-char = Unicode-letter |
  Unicode-number | "_"`; `bare-key = key-char *( [key-punctuation] key-char )`,
  so punctuation must be followed by a key character; `*` is not a valid first
  character.
- **PC-2** `:19,22` — C — Unicode categories and version. Rule: letters are Lu,
  Ll, Lt, Lm, Lo; numbers are Nd, Nl, No; classified with the Unicode tables
  bundled in the native parser at the pinned version; locale-independent on
  every surface.
- **PC-3** `:24,35` — A/C — whitespace set undefined. Rule: whitespace is the
  CommonMark Unicode whitespace set.
- **PC-4** `:35-36` — A — "balanced within the current inline parsing subject".
  Rule: balanced before the end of the same inline container; braces owned by
  code spans or HTML tokens do not count.
- **PC-5** missing — B — no opener precondition (`foo@bar`, `1@bar`, `(@bar`).
  Rule: `@`, or the `-` of `-@`, opens a cite only at the start of the inline
  container or when the preceding scalar is not a Unicode letter, number, or
  `_`; otherwise it is text, independently of the autolink option.
- **PC-6** `:43-44` — B — spacing after `[` and `;`, `[see; @foo]`, `[@foo;]`,
  `[ ]`. Rule: `bracketed-group = "[" spacing citation-item *( ";" spacing
  citation-item ) spacing "]"` with spacing of optional whitespace and at most
  one line ending; a group with an item lacking a key is not a citation and the
  bracket continues at the shortcut step of PS-1.
- **PC-7** `:48-51` — A/B — affix whitespace trimming. Rule: prefix and suffix
  exclude leading and trailing whitespace; a suffix of only whitespace is empty;
  `[see @doe99, pp. 3]` gives prefix `see` and suffix `, pp. 3`.
- **PC-8** `:53-54` — B — `[Smith-@1990]`. Rule: an unescaped `-` immediately
  before `@` is always the mode marker, and PC-5 is evaluated at the `-`.
- **PC-9** `:66-67` vs `:71` — A/B — the example `@smith04 [p. 33]` contradicts
  "immediately following"; tails with `;` items, tails starting with `^`, tails
  followed by `(`, `[`, or a container. Rule: optional spaces or tabs and at
  most one line ending may separate the key from `[`; the tail is not claimed if
  it begins with `^` or if its `]` is immediately followed by `(`, `[`, or a
  valid container; `@k [s1; @k2, s2]` produces one `Cite` whose first item is
  author-in-text with suffix `s1` followed by the further items.
- **PC-10** `:119-126` — A — whitespace inside item scopes. Rule: item scope
  runs from the first non-whitespace byte after `[` or `;` to the last
  non-whitespace byte before `;` or `]`; an author-in-text group and item scope
  run from the marker or `@` through the tail's closing `]`.
- **PC-11** `:79-82,93-95` — A/E — locator defaults and "not rendered after
  citation processing" describe a downstream tool. Rule: mark as non-normative
  notes; the parser rule stays "braces are suffix text".
- **PC-12** `:111-115` and `lists.md:123-126` — A — which spellings example
  labels capture. Rule: only `(@label)` is subject to example resolution; bare
  `@label` and `[@label]` are citations whenever `citations` is on; `(@label)`
  with a registered label is `ExampleReference`, and with an unregistered label
  it is `(` plus an author-in-text `Cite` plus `)` when `citations` is on, else
  text. This is a documented deviation from Pandoc, which also resolves bare
  `@label`.
- **PC-13** `:99-102` — B — a non-resolving reference tail leaves the group
  undecided. Rule: a cite group is tested only after a direct tail, a resolving
  reference tail, and an enabled valid container have failed; a non-resolving
  tail does not block citation recognition, so `[@foo][nope]` is a `Cite` plus
  literal `[nope]`.
- **PC-14** missing — B — `[@key]: /url` at line start. Rule: a line the
  inherited grammar accepts as a link reference definition is a definition
  regardless of a leading `@`; the citation rule is inline-only.
- **PC-15** `:134-136` — A — "top-level heading". Rule: a heading whose parent
  is `Document`; the parser stores the class as written and does nothing else.
- **PC-16** `:106` — A — "comment bodies" (PX-4).

#### `pandoc/definition-lists.md`

- **PD-1** `:52` — B — `body-continuation` is never defined; blank lines between
  two markers of one term are underivable. Rule: a body continuation is a line
  owned by the body under the inherited list-item continuation rules for the
  body's continuation column, an indented nonblank line, a blank line followed
  by such a line, or a lazy paragraph-continuation line; blank lines between two
  marker lines of the same term belong to the definition; a marker line is at
  the same container depth when its indentation is measured from the enclosing
  container's content start.
- **PD-2** `:50` — B — any nonblank line qualifies as a term line, including
  headings, list items, definitions, and indented lines. Rule: the candidate is
  tested only where the inherited parser would open a paragraph; a term line is
  never a line claimed by a higher-precedence block start, is not itself a
  marker line, has at most three columns of indentation, and is not a line the
  inherited grammar extracts as a link reference or footnote definition.
- **PD-3** `:87-90` — D — `four_space_rule` is not an option of this repository.
  Rule: delete the clause.
- **PD-4** `:76-81` — B — no procedure for the caption exclusion. Rule: during
  lookahead, if `tableCaptions` and at least one table option are on and the
  candidate marker line is a caption line (PT-5) whose paragraph is followed by
  blank lines and a line that opens an enabled table syntax, the definition-list
  candidate fails; only the blank-gap form is affected.
- **PD-5** `:101-103` — B — lazy continuation consequences. Rule: lazy
  continuation applies in compact and loose definitions alike; a lazy line is
  never re-examined as a term, so `Term1`, `: d1`, `Term2`, `: d2` is one term
  whose first body paragraph is `d1 Term2` and whose second body is `d2`; a
  blank line is required before a new term.
- **PD-6** `:106-109` — A/D — "enclosing HTML close tag" is Pandoc state this
  parser does not have; "raw line scanner" is undefined. Rule: a line at the
  current depth that starts another list item, a fenced-code opener, or an
  enclosing fenced-div closer ends lazy absorption and the block parser decides
  its owner; delete the HTML clause.
- **PD-7** `:54` — B — a marker followed only by whitespace. Rule: it is the
  marker-only form.
- **PD-8** `:67-68` — B — term trimming and hard breaks. Rule: the term is
  parsed as a one-line paragraph with leading and trailing whitespace removed; a
  trailing backslash or trailing spaces produce no `LineBreak`.
- **PD-9** `:111` — B — paragraph interruption. Rule: a definition list cannot
  interrupt a paragraph; a marker line after a paragraph line that is not the
  candidate term is paragraph text.
- **PD-10** `:139-141` — B — trailing blank lines. Rule: both scopes end at the
  end of the last nonblank line of the last body.
- **PD-11** `:16-21` — transitional — `content: [[Markup]]` has no dump encoding
  (see X-7).

#### `pandoc/fenced-divs.md`

- **PF-1** `:9-12` — B — indentation, whitespace after the colons, class-word
  charset, trailing colon run, trailing whitespace. Rule: `opener = *3SP 3*":"
  *WSP ( attributes / class-word ) *WSP *":" *WSP EOL` with `class-word = 1*(
  non-whitespace scalar other than ":" "{" "}" )`; a tab in the leading
  indentation disqualifies the line.
- **PF-2** `:35-36` — A — "a line containing at least three consecutive colons
  and no attributes" admits `text ::: text`. Rule: `closer = *3SP 3*":" *WSP
  EOL`.
- **PF-3** `:43-44` — E — "matching Pandoc's recoverable unclosed-div behavior".
  Rule: delete the clause; an unclosed `Div` ends where its enclosing
  container's content ends or at end of input, with `closed=false`.
- **PF-4** `:46-47` — A — "should be separated ... by blank lines". Rule: an
  opening fence may interrupt a paragraph as a fenced-code opener does; a
  closing fence may interrupt a paragraph inside its div; no blank line is
  required.
- **PF-5** `:30-31` — A — "last consumed content". Rule: an unclosed `Div.scope`
  ends at the end of the last line of its last child block, or at the end of the
  opener line when it has no children.
- **PF-6** missing — B — Remark container directives share the opener and closer
  (see S-8).
- **PF-7** `:40-42` — B — "eligible closer". Rule: a line is an eligible closer
  when, after the enclosing containers' prefixes are stripped, it matches the
  closer grammar.

#### `pandoc/headings-and-anchors.md`

- **PH-1** `:23-26` — B — no per-kind text projection. Rule: `Text` and `Code`
  contribute `literal`; `Emphasis`, `Strong`, `Strikethrough`, `Span`,
  `Superscript`, `Subscript`, `Mark`, `Insert`, `Link`, `Image`, and
  `DirectiveLabel` contribute their concatenated child text; `SoftBreak` and
  `LineBreak` contribute one space; `HTML`, `Comment`, and a footnote `Cite`
  contribute nothing; a bibliography `Cite` contributes, per item, prefix text,
  `@key`, and suffix text in order; `ExampleReference` contributes `@label`;
  `Formula` contributes `literal`; `CrossLink` contributes `label` if non-null
  and otherwise the source path and anchor text; `Directive` contributes its
  label text.
- **PH-2** `:27` — C — lowercase mapping unspecified. Rule: the simple lowercase
  mapping with no special casing and no locale, from the Unicode tables bundled
  in the native parser; every surface uses the native result.
- **PH-3** `:28-31` — C — categories. Rule: whitespace is CommonMark Unicode
  whitespace; letters are L*; numbers are Nd, Nl, No; combining marks are Mn,
  Mc, Me; connector punctuation is Pc.
- **PH-4** `:44-45` — A — the string `base-N` must be unregistered. Rule: append
  `-N` with the smallest N of at least 1 for which `base-N` is not registered.
- **PH-5** `:46-47` — A — "may produce a diagnostic" with no diagnostics
  channel. Rule: the parser emits no diagnostic; both declarations remain.
- **PH-6** `:41-48` — B — footnote bodies and cross-container order. Rule: the
  registry covers every node reachable from `Document.content` and
  `Document.footnotes`; generation order is ascending `Heading.scope.start`.
- **PH-7** `:55-62,72-77` — B — virtual definitions with unwritable labels,
  their title and merge contribution, multi-line labels. Rule: the virtual
  definition has `title=null`, `anchor=null`, and empty attributes for `merge`;
  its key is the label source with inherited normalization; a heading whose
  label cannot be written as a reference label contributes no definition.

#### `pandoc/lists.md`

- **PL-1** `:40-41` — D — `start >= 1` while CommonMark accepts `0.`. Rule:
  `start >= 0`; `0.` and `0)` are valid decimal markers with value 0.
- **PL-2** `:71-72` — D/B — "only the inherited decimal-period form"; `style`
  and `delimiter` for inherited lists unstated. Rule: with `fancyLists` off the
  inherited `N.` and `N)` markers are the only ordered markers, storing
  `style=decimal` and `delimiter=period` or `oneParen`; `style=default` and
  `delimiter=default` are stored only for `#` markers under `fancyLists`.
- **PL-3** `:78-79` — D — `startnum` off stores `start=1` for the inherited
  `5.`, changing inherited output with an option that defaults to off. Rule:
  `startnum` governs only alphabetic and Roman markers; decimal markers always
  store the inherited value. (Decision: alternatively drop `startnum` and always
  store the first marker's value.)
- **PL-4** `:46-55` vs `:86-87` — A — `@.` and `@)`. Rule: `@` occurs only
  inside parentheses: `(@)`, `(@label)`, `(N@)`, `(N@label)`.
- **PL-5** `:50,62-63` — B — "valid Roman numeral" has no grammar. Rule: `roman
  = M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`, at least one
  character, one case, value the sum of its parts, the whole marker consumed.
- **PL-6** `:56-59` — B — tabs; Pandoc's `p. 3` exception. Rule: padding follows
  the inherited rule of one to four columns of space or tab; the capital-period
  case requires at least two columns of whitespace or end of line after the `.`;
  the `p.` exception is not adopted.
- **PL-7** `:61-62,66-67` — B — later markers must be classified against the
  committed style, else `h. i. j.` splits at `i`. Rule: after the first item
  commits a style, each later marker is read in that style first; `#` continues
  any style; a marker unreadable in the committed style with the same delimiter
  ends the list.
- **PL-8** `:67-69` — A/D — "nested" and "equivalent" undefined; interaction
  with `fancyLists` off. Rule: with `fancyLists` on, an ordered list whose first
  item lies inside a list item or a definition body must have numeric value 1
  (`1`, `a`, `A`, `i`, `I`, `#`) or the line is paragraph text; with it off the
  inherited rule applies; example lists are exempt.
- **PL-9** `:118-121` — E/B — `(N@)` defined by reference to Pandoc; counter
  start, digit limit, `(0@)`, later markers. Rule: the counter starts at 1; `N`
  is one to nine decimal digits with value at least 1; `(0@)` and longer runs
  are not markers; on the first item of a list `N` sets the counter before that
  item is numbered; on a later item `N` is ignored and the marker is otherwise
  valid; numbering follows ascending item `scope.start` across content and
  footnotes.
- **PL-10** `:99-101` — C/B — label charset. Rule: `label = alnum-run *( ("_" /
  "-") alnum-run )` with alphanumerics from the bundled Unicode tables.
- **PL-11** `:104-105` — B — "Elsewhere". Rule: in inline content outside code,
  HTML tokens, autolinks, and comments, the exact spelling `(@label)` with no
  internal whitespace is an `ExampleReference` when the label is registered
  anywhere in the document; where it is a valid list marker, the marker rule
  wins.
- **PL-12** `:111-116` — A — the two sentences contradict for the contiguous
  case. Rule: a repeated label never splits a list; the item is an ordinary
  item, `exampleLabel` records the label, and the counter does not advance; a
  list whose first item repeats a label has `start` equal to that label's first
  number.
- **PL-13** `:120-121` — B — column semantics. Rule: the continuation column of
  an example-list item is the container start plus four columns after tab
  expansion regardless of marker width.
- **PL-14** `:138` — B/D — "parse error" is not an outcome of `Document.parse`.
  Rule: decimal markers and `N` are limited to nine digits, so no counter can
  overflow; delete the sentence.
- **PL-15** missing — B — paragraph interruption. Rule: only a marker with
  numeric value 1 may interrupt a paragraph; example markers never do.
- **PL-16** `:54` — A — `#)` and `(#)`. Rule: `#)` stores `oneParen`, `(#)`
  stores `twoParens`, and only `#.` stores `delimiter=default`.

#### `pandoc/superscript-and-subscript.md`

- **PU-1** `:24-26,30-33,62` — A/B — two incompatible closer models
  (character-level search with reparse versus an engine-aware search);
  "eligible" undefined; crossing with emphasis and links unstated. Rule: see
  S-7; `^` and `~` are delimiter-run units on the shared stack, a run of exactly
  one is a unit, a unit closes the nearest unmatched opener of its kind, and an
  opener is discarded when unescaped whitespace or a line ending is reached
  before a closer; same-kind delimiters do not nest.
- **PU-2** `:38-39,73` — C/A — whitespace set; character references decoding to
  whitespace. Rule: CommonMark Unicode whitespace; a character reference never
  invalidates a candidate.
- **PU-3** `:39-41` — B/E — scope of the `\ ` escape. Rule: `\ ` is recognized
  only inside a superscript or subscript body candidate, where it counts as
  non-whitespace and yields U+00A0; elsewhere the inherited literal remains.
- **PU-4** `:33-34,58-60` — B/D — inherited single-tilde strikethrough (see S-7
  and the CLI-only double-tilde flag). Rule: with `subscript` on, a tilde run of
  length one is never a strikethrough delimiter, a run of two is strikethrough
  when matched under the inherited algorithm and otherwise two subscript
  delimiters, and runs of three or more are literal; with `subscript` off
  inherited behavior is unchanged.
- **PU-5** `:57-58` — B — `^[text](url)^` (see S-1 for the adopted rule: the
  inline footnote wins).
- **PU-6** missing — B — Obsidian block identifiers and autolinks containing `^`
  or `~`. Rule: block-identifier attachment is decided before inline parsing and
  the bytes it removes are never delimiters; bytes owned by an autolink are
  opaque.
- **PU-7** `:35-37` — E — justification by oracle; keep the rule, drop the
  justification.

#### `pandoc/tables.md`

- **PT-1** `:51-52` — D (rule 2) — "a simple inline cell is normalized to one
  paragraph". Rule: pipe and simple table cells store their parsed inline
  content directly; multiline and grid cells store the block sequence the block
  parser produces from the cell text; no cell is wrapped in or unwrapped from a
  `Paragraph`. The same stale text sits in
  `docs/plans/2026-09-03-pandoc-markdown-extensions.md:215-217`.
- **PT-2** `:55-57` — A — "paragraph-like block". Rule: `TableCaption.content`
  is the caption paragraph's parsed inline content after removing the marker and
  the whitespace after it; a multi-line caption contributes `SoftBreak`s.
- **PT-3** `:59-61,165,188` — B/C — no width formula; Pandoc's normalizes
  against a runtime column setting. Rule: simple and pipe tables store
  `relative=null`; multiline: `w[i]` is the scalar count from the start of dash
  run i to the start of run i+1, the last run its own length; grid: `w[i]` is
  the scalar count strictly between the column's boundary positions;
  `relative[i] = w[i] / sum(w)` in IEEE-754 double division with no other
  constant.
- **PT-4** missing — B/C — unit of column arithmetic (Pandoc uses display
  width). Rule: all column positions count Unicode scalars of the line after tab
  expansion to four-column stops; no display-width computation.
- **PT-5** `:121-124,133-134` — B/A — caption marker indentation, `:` followed
  by punctuation (`:::`, `:-`, definition markers), case, the empty case. Rule:
  `caption-line = *3SP ( "Table:" / "table:" / ":" ) rest` where for `:` the
  next scalar is not punctuation (P* or S*) and the word spellings are
  case-sensitive; a marker with no content produces `TableCaption(content=[])`;
  the caption paragraph is separated from the table by zero or more blank lines
  and nothing else.
- **PT-6** `:127-129` — A — a caption paragraph between two tables. Rule: it
  belongs to the preceding table.
- **PT-7** `:122` — B — "any supported table". Rule: pipe, simple, multiline,
  and grid tables; every Pandoc table option is independent of the inherited
  `tables` option.
- **PT-8** `:138-141` — B/A — separator grammar, header line, "matching" footer,
  minimum rows. Rule: `separator = *3SP dash-run *( 1*SP dash-run ) *SP EOL`;
  the header line is the immediately preceding nonblank line and must be the
  first line of a paragraph candidate; body rows are every following line until
  a blank line or a footer line of the same shape followed by a blank line or
  end of input; at least one body row or a footer is required.
- **PT-9** `:143-148` — A — "flush right and extended on the left". Rule: with
  the header text segment right-trimmed, `leftSpace` means the segment begins
  with a space or tab and `rightSpace` means its length is less than the dash
  run's; (true,false) is right, (false,true) is left, (true,true) is center, and
  (false,false) or an empty segment is none; multi-line headers use the shortest
  non-empty line's segment; headerless tables use the first body line.
- **PT-10** missing — B — cell segmentation. Rule: columns are cut at the start
  position of each dash run; bytes before the first run belong to column one;
  bytes from the last run's start to the end of line belong to the last column;
  each segment is trimmed.
- **PT-11** `:196-200` — A/B — Setext, thematic break, lookahead. Rule: a single
  dash run without internal whitespace that the inherited grammar treats as a
  Setext underline is an underline, never a separator; a line with two or more
  dash runs is a simple-table separator when the shape holds and otherwise a
  thematic break; a full-width dash line is a multiline opener only when the
  complete table parses under bounded lookahead; each line is scanned at most
  twice.
- **PT-12** `:157-161,166-168` — B — neither multiline boundary is defined;
  one-row fallback. Rule: `full-boundary = *3SP 3*"-" *SP EOL` as one run; the
  segment boundary is the simple-table separator; the header block is every
  nonblank line between them; rows are separated by blank lines; the last row
  may be followed directly by the full boundary unless it is the only row, in
  which case the candidate is retried as a simple table and otherwise follows
  inherited fallback.
- **PT-13** `:163-165` — B — join rule. Rule: each cell's per-line segments are
  joined with LF and parsed as a block sequence.
- **PT-14** missing — B — order of the table syntaxes at one block start. Rule:
  grid (line begins with `+`), then multiline with header, then simple, then
  headerless multiline; pipe tables are decided by the inherited rule at the
  header line.
- **PT-15** `:177` — D — "The top boundary establishes every vertical column
  boundary" contradicts the corpus case `grid-table-row-and-column-spans`. Rule:
  the column boundary set is the union of `+` positions on every horizontal
  boundary line; every `+` and `|` must sit at a boundary position or the
  candidate fails.
- **PT-16** `:176-184` — B — grid line grammar, mixed lines, rows and spans.
  Rule: a grid line begins and ends with `|` or `+` at the table margin; between
  adjacent boundary positions a segment is horizontal (all `-` or all `=`,
  optional edge colons) or cell text; a cell anchored at row r and column c
  spans right until a `|` or `+` at a boundary position and down until the first
  line where its full width is a horizontal segment; an `=` segment is a head or
  foot boundary only when the whole line is `=`.
- **PT-17** `:186-188` — B — colons on both candidate lines. Rule: with a head
  separator only its colons count; without one the top line's; colons elsewhere
  are ignored.
- **PT-18** `:190-192` — A — "retain their meaning or fail". Rule: at most one
  head separator and at most one foot; any other `=` line fails recognition.
- **PT-19** missing — B — cell text extraction. Rule: cell text is, per line,
  the scalars strictly between the cell's boundary positions, right-trimmed; if
  every non-empty line begins with a space one space is removed from each; lines
  are joined with LF and parsed by the block parser; a `|` or `+` at a
  non-boundary position fails the table.
- **PT-20** missing — B/D — scopes of multi-line cells versus the
  contiguous-range rule of `canonical-ast.md:56-58`. Rule: cell and descendant
  scopes use original-source coordinates from the first scalar of the first
  segment to the last scalar of the last segment; the range may include other
  cells' bytes; `canonical-ast.md` records this exception.
- **PT-21** `:202-203` — B — fallback position. Rule: fallback restarts
  inherited block parsing at the candidate's first line.
- **PT-22** `:32-35` — transitional/C — `Double` has no dump encoding and
  platform-dependent formatting. Rule: `columns=[left:0.25,none:null]`; doubles
  print as the shortest round-trip decimal.
- **PT-23** missing — B — `^id` after a table that owns a following caption.
  Rule: it attaches to the `Table`, whose scope already includes the caption.

## Seams between modules

- **S-0** — no file owns cross-family precedence
  (`obsidian-flavored-markdown.md:71` covers Obsidian only, `pandoc.md:59-70`
  orders nothing, `inserted-text.md:95-96` says "same machinery as emphasis").
  Rule: `canonical-ast.md` gains the two recognition-order tables; every
  module's precedence paragraph becomes a pointer to them and keeps only its own
  grammar and fallback.
- **S-1 Footnotes and citations** — the identifier keeps the caret in the
  current contract and drops it in two target files (`canonical-ast.md:150,166`;
  `obsidian/footnotes.md:76-77`; `citation-model.md:64-65`); the "inherited GFM
  footnote grammar" is written nowhere (OF-1); `Citation.scope` has three rules
  (OF-11); an author-in-text tail versus a footnote call or link (`@doe [^1]`,
  `@doe [p. 3](url)`; PC-9); `[^a]{.x}` and `[^a](u)` with spans or link
  attributes on (PS-1); `^[a](b)^` with superscript on (OF-2); the gate for
  `^[...]` (OI-2); a synthesized paragraph with a scope (OF-4); a `Cite` in a
  heading anchor (PH-1). Rules: `citation-model.md` owns the id (never the
  caret) and the scope split; `obsidian/footnotes.md` owns the referenced
  grammar, the duplicate rule, and the bracket rule that a defined `[^label]` is
  a `Cite` whatever follows and that the inline footnote wins regardless of
  following bytes; `pandoc/citations.md` owns the tail exclusion; `^[...]` is
  recognized iff `obsidian` and `footnotes` are both on.
- **S-2 Attributes** — the current directive model versus the shared model
  (CA-6, T `M7`); fenced-code lowercasing and aliasing (PA-2 to PA-4); a bare
  autolink followed by `{.a}` (PA-7); dual ownership statements between
  `pandoc/attributes.md:3-6` and the span and div modules. Rule: `attributes.md`
  owns the grammar, `pandoc/attributes.md` owns only the attachment-registry
  rows, and the recognition modules own recognition.
- **S-3 Anchors** — whether an ID stored on an unreferenced definition is
  reserved (`anchors.md:76-78`, `pandoc/attributes.md:148-152`); `[[#Heading]]`
  "addresses `Markup.anchor`" while headings have none under `obsidian`. Rules:
  the registry reserves the final anchors of emitted nodes only, an inherited ID
  is reserved by the occurrence, and an unreferenced definition reserves
  nothing; the parser stores the spelled wikilink value and matching is consumer
  policy. The registry order itself is consistent across files.
- **S-4 Destinations and links** — `dest` versus the current strings (T `M1`);
  the reference kinds the modules remove (T `M2`); `[[...]]` against inherited
  bracket syntax (OW-4); two width representations on `Image` (PA-9). Rule: the
  typed `width` and `height` fields are owned by the integration module and
  records never populate them.
- **S-5 Tables** — two `Table` models and paragraph-normalized cells (PT-1, T
  `M6`); a preceding caption claiming a completed paragraph against the
  no-rescan rules (`pandoc/tables.md:127-129` versus `pandoc.md:61-63`,
  `pandoc/definition-lists.md:111-112`, `obsidian/properties.md:161-162`); the
  two-hyphen rule (OI-3); a caption versus a `^id` line after a table (PT-23);
  "table, row, and cell anchors are null" (PA-1). Rule: a caption line is a
  table-candidate block start parsed in one lookahead with the following table,
  and if no table follows the bytes are released to paragraph parsing;
  `pandoc/tables.md` owns the one model; `^id` after a caption attaches to the
  `Table`.
- **S-6 Lists and tasks** — `startnum` off forcing `start=1` (PL-3, decision
  D-3); `fancyLists` off denying the inherited `1)` (PL-2); `start >= 1` versus
  CommonMark `0.` (PL-1); the nested-start restriction on decimal markers
  (PL-8); `checked` versus `marker` (T `M5`); which option gates Obsidian task
  markers (OI-2, OT-4); the undefined `four_space_rule` (PD-3). Rule: Obsidian
  markers require both `taskLists` and `obsidian`, and `taskLists` off disables
  every task prefix.
- **S-7 Inline delimiters** — two processing models, the forward "next eligible
  closer" of `pandoc/superscript-and-subscript.md:30-32` against the shared
  delimiter stack of `inserted-text.md`, `obsidian/highlights.md`, and the
  integration module, so `*a~b*c~` and `~~a~b~~` have two outputs (PU-1);
  single-tilde strikethrough is inherited in this engine and collides with
  `subscript` (PU-4); formula bodies are missing from every opacity list;
  `CrossLink` opacity is stated only by the block-identifier module (`[[a%%b]]`,
  `==[[a==b]]==`); a comment opener versus earlier opener-scanners; the
  paragraph `^id` suffix timing (OB-7). Rules: `^` and `~` are delimiter-run
  units on the shared stack; with `subscript` on a single-tilde run is never
  strikethrough; the shared opaque list includes formula bodies and a completed
  `CrossLink`; opener-scanner constructs are attempted at their opener in source
  order and the first success owns its bytes, so a `%%` inside an owned region
  is not an opener. Decision D-8 settles the CLI-only double-tilde flag.
- **S-8 Block starts** — a fenced div `:::` against a Remark container directive
  `:::name` with `directives` on by default (`pandoc/fenced-divs.md:9-11,35-38`;
  `remark/attributes.md:44-47`); the directive envelope grammar absent (RM-2);
  Pandoc table lines against Setext, thematic breaks, and paragraphs (PT-11); a
  standalone `%%` line unplaced among block starts (OC-2); a definition-list
  term line against other block starts (PD-2, PD-9). Rules: with `directives`
  on, a colon run followed immediately by a directive name is a container
  directive and never a `Div`; a `Div` opener requires `{` or whitespace after
  the colon run and an unbraced class word may not contain `{`; a bare `:::`
  line closes the innermost open container of either kind; a Setext heading
  beats every table candidate, a complete simple, multiline, or grid candidate
  beats a thematic break and a paragraph, and a dash line that is part of no
  complete candidate is a thematic break; the block `%%` line is tested after
  code and HTML opacity and container prefixes and before every other block
  start; the definition-list test runs after every other enabled block start and
  before the paragraph.
- **S-9 Metadata** — `Document` has three shapes (ME-1, T `M4` and `M7`); no
  option named for Properties (OP-7). Rule: gated by `obsidian` only.
- **S-10 Options and profiles** — "exactly these booleans" (CA-20); camelCase
  versus `snake_case` (`pandoc.md:41-51` and every Pandoc module); profiles
  named but undefined (OI-4); what `obsidian` composes (OI-2); the example-label
  rule stated twice in `pandoc/citations.md:111-115` and
  `pandoc/lists.md:123-126`; `stripHTMLComments` against "pinned cmark behavior"
  in the integration module; `headerAttributes` off never stated. Rules:
  `canonical-ast.md` owns camelCase binding names and the `pandoc.md` table
  gains a "ParseOptions field" column; `pandoc/lists.md` owns the example-label
  rule and `citations.md` points to it; the integration module adds "subject to
  `stripHTMLComments`"; `pandoc/attributes.md:94` adds the off case.
- **S-11 Scope and the dump** — no module kind, field, enum, or value is in the
  contract or the dump grammar yet (T, every kind-adding item; X-7); synthesized
  paragraphs with scopes (OF-4, PT-1); which scoped values are `Markup`
  (`citation-model.md:36-38`, `obsidian/footnotes.md:61-62`,
  `metadata.md:61-65`, `pandoc/tables.md:50`); `Comment` in both content
  categories (OC-4). Decision D-4 settles the value-versus-kind question; the
  JSON then marks each type.
- **S-12 Oracle-as-definition statements** —
  `obsidian/footnotes.md:16-18,76,99-100` (OF-1);
  `obsidian-flavored-markdown.md:41-43` naming "GFM 0.29 and cmark-gfm" as two
  authorities that disagree on single tildes; `remark.md:8-12` (RM-2);
  `attributes.md:10-16` (AT-11); `inserted-text.md:173-174` (IT-3);
  `pandoc/attributes.md:125-127` (PA-4); `pandoc/fenced-divs.md:42-44` (PF-3);
  `obsidian/callouts.md:46-49,55-56` (OK-9); `obsidian/properties.md:171`
  (OP-8); `metadata.md:12-13` (ME-2); `pandoc.md:7-10` (PX-1);
  `test-architecture.md:205-207,233-240` (TA-1, TA-3). Rule for all: the module
  text is the rule; the oracle is evidence; a divergence is a registered delta;
  the GFM layer names one authority, cmark-gfm at its pinned version minus
  registered deltas.

## Decisions this audit needs

Every other finding has one obvious rule, stated beside it. These few are
product choices; each has a recommended default that the resolution checklist
assumes.

- **D-1 The `obsidian` preset vector.** Recommended: the `default` profile's
  extensions and options plus `obsidian=true` and `stripObsidianComments=true`,
  with `directives` and `smartPunctuation` off because Obsidian has neither;
  each Obsidian rule is additionally gated by the inherited option it extends
  (OI-2).
- **D-2 The own-line block identifier `text` then `^id` on the next line.**
  Recommended: accepted as the paragraph form (OB-1); the alternative is that
  the second line keeps `^id` as text.
- **D-3 `startnum`.** Recommended: keep it but let it govern only alphabetic and
  Roman markers, so decimal markers always store the inherited number (PL-3);
  the alternative drops the option and always stores the first marker's value.
- **D-4 Traversed values.** Whether `Citation`, `Footnote`, `TableCaption`, and
  `Definition` are `Markup` kinds with visitor methods and empty universal
  fields, or scoped values reached through typed callbacks. Recommended:
  `Citation` and `Footnote` stay scoped values with value callbacks, as the
  landing plan says; `TableCaption`, `Definition`, and `DefinitionList` are
  `Markup` kinds; `Metadata` and `MetadataRecord` are scoped values outside
  `Markup` with no callbacks; `TableColumn`, `Destination`, `CitationReferent`,
  and `Attributes` are unscoped values.
- **D-5 Invalid UTF-8 at the C entry point.** Recommended: keep the caller
  precondition that #191 established and add no validation pass;
  `canonical-ast.md` states that the C entry point's output for invalid UTF-8 is
  unspecified and that every binding guarantees valid UTF-8 before calling it,
  so no implementation item is needed. The alternative, a validation scan
  returning an invalid-input error, would be new engine work needing its own
  landing item.
- **D-6 Pipe-table rows with too few or too many cells.** Recommended: state
  whatever the engine does today as the rule and register the difference from
  cmark-gfm; if it pads, a padded cell's scope is the empty range at the row's
  end.
- **D-7 Callout `variant` case.** Recommended: stored as written (OK-9),
  matching is consumer policy, consistent with metadata keys.
- **D-8 The CLI-only double-tilde strikethrough flag.** Recommended: remove it;
  with `subscript` on a single tilde is subscript and never strikethrough, and
  with `subscript` off the inherited single-tilde strikethrough stands.
- **D-9 Example-label capture.** Recommended: only the `(@label)` spelling
  resolves against example labels; bare `@label` is always a citation (PC-12), a
  documented deviation from Pandoc.

## Resolution checklist

One checkbox is one specification pull request. Each lists the findings it
closes; a finding marked transitional is closed by its landing-plan item, and
the item here only fixes the status line. Order is by dependency: the shared
contracts first, because every module points to them, then the modules, then the
seams that need two modules edited together.

- [ ] **A1 — `canonical-ast.md` and `canonical-ast.json`:** the coordinate
      contract, the limits section, the recognition-order tables, the
      `ParseOptions` registry with defaults and off rules, the profiles table,
      the formulas grammar or a pointer to a new module, the `smartPunctuation`
      and `stripHTMLComments` effects, `Text` merging, `CodeBlock.info` and
      `closed`, table cell-count handling, destination unescaping, the
      scoped-value rule, the walk-into-values rule, and the status sentences
      that name which target file supersedes which section. Closes CA-1 through
      CA-27 except the transitional ones, CJ-1, CJ-3, CJ-4, CJ-7, and decisions
      D-4, D-5, D-6.
- [ ] **A2 — `canonical-ast-dump.md`:** token separation, the `children` table,
      the string escaping form, enum elements in arrays, the universal-field
      line, tagged-value and nested-value encodings, double formatting, the
      maintenance command. Closes CD-1 through CD-10.
- [ ] **A3 — `test-architecture.md`:** "records" instead of "freezes", pinned
      CommonMark and cmark-gfm versions, oracles as evidence, per-test timeouts,
      the manifest carrying every option, the size-doubling definition. Closes
      TA-1 through TA-6 and AN-8.
- [ ] **A4 — `attributes.md`, `anchors.md`, `destinations.md`,
      `citation-model.md`, `metadata.md`, `inserted-text.md`:** the ABNF
      nonterminals, the quoted-value rule, the input string the grammar runs on,
      "as written", the merge steps as normative, the anchor synthesis gate and
      timing, no diagnostics, one allocation-failure rule, the anchor
      derivation, the footnote id without caret, the two scope rules, the
      metadata key and tagged-scalar rules, record scopes, the inserted-text
      unit rules and version pins, and one status sentence per file. Closes
      AN-1, AN-3, AN-5 through AN-8, AT-3 through AT-8, AT-11, DE-4, DE-5, DE-7,
      CI-2 through CI-5, ME-2 through ME-10, IT-3 through IT-8.
- [ ] **A5 — `remark/directives.md` (new) and `remark/attributes.md`:** the
      directive envelope grammar, the leaf-versus-empty-container rule,
      name-label-container order, line-crossing containers, invalid containers
      on opener lines, the option name. Closes RM-2, RM-3, RA-2 through RA-5,
      CA-13.
- [ ] **B1 — `obsidian-flavored-markdown.md`:** the footnote-grammar owner, the
      preset vector and gate matrix, the profile list, `MetadataListItem`, the
      limits pointer, extended autolinks, deletion of the per-module toggle
      clause, one GFM authority. Closes OI-1 through OI-9 and decision D-1.
- [ ] **B2 — `obsidian/wikilinks-and-embeds.md` and `obsidian/highlights.md`:**
      character sets, single brackets, `#` edge cases, escapes, no trimming,
      bracket precedence, option-off, the anchor projection; highlight
      eligibility and matching, empty content after stripping, block-pass and
      autolink boundaries, option-off. Closes OW-1 through OW-6, OH-1 through
      OH-4.
- [ ] **B3 — `obsidian/comments.md`:** the opener and closer rule, the block
      start and commit procedure, block-versus-inline classification, both
      content categories, the stripped shape, all recognition suppressed, the
      literal, table cells, `stripObsidianComments` with `obsidian` off. Closes
      OC-1 through OC-9.
- [ ] **B4 — `obsidian/footnotes.md`:** the referenced grammar and duplicate
      rule, the bracket-stack rule for `^[`, whitespace-only bodies, no
      synthesized paragraph, the `inline-N` set, the pinned fold version,
      unresolved calls, the `Document` field list, option-off, the scope split.
      Closes OF-1 through OF-11.
- [ ] **B5 — `obsidian/block-identifiers.md` and `obsidian/callouts.md`:** the
      paragraph and own-line forms, the identifier-line rule,
      item-versus-paragraph ownership, metadata before identifiers, option-off,
      the second-anchor sentence, the lexical suffix rule; the metadata
      candidate line, separator requirement, trailing whitespace, the paragraph
      split, Setext, empty titles, `CalloutFold`, option-off, `variant` as
      written. Closes OB-1 through OB-7, OK-1 through OK-9, decisions D-2 and
      D-7.
- [ ] **B6 — `obsidian/tasks.md`, `obsidian/properties.md`,
      `obsidian/inherited-and-integration.md`:** the separator class, `isTask`
      and `isComplete`, the UTF-8 sentence, option gating; the payload and
      line-ending grammar, keys, decoded lines, the scalar table, transactional
      wording, the toggle clause, the snapshot sentence, the limits pointer; the
      inherited delimiter grammar, the target table shape, the dimension range
      and pipe rule, the extended-autolink step, the precedence pointer, the
      boundary scanner, the formula pointer. Closes OT-1 through OT-4, OP-1
      through OP-9, ON-1 through ON-8.
- [ ] **C1 — `pandoc.md` and `pandoc/attributes.md`:** the modules as sole
      authority, the grammar owner, the inherited-behavior sentence, "comment"
      defined, the transactional failure rule, the limits pointer; table anchors
      under Pandoc rules, no language from classes, the bare word and `info`
      rule, no lowercasing or aliasing, the container position, the ATX order,
      the attachment sites, the definition-side grammar, records versus typed
      dimensions, the ordered bracket procedure pointer, the required cases, a
      "ParseOptions field" column. Closes PX-1 through PX-6, PA-1 through PA-11.
- [ ] **C2 — `pandoc/bracketed-spans.md` and `pandoc/citations.md`:** the
      ordered `]` procedure, malformed containers, directives and wikilinks, the
      spans-off case; the key grammar, Unicode classes, whitespace, balance, the
      opener precondition, group spacing, affix trimming, the `-` marker, tails,
      item scopes, non-normative notes, example labels, non-resolving tails,
      definitions, top-level headings. Closes PS-1 through PS-5, PC-1 through
      PC-16, decision D-9.
- [ ] **C3 — `pandoc/definition-lists.md` and `pandoc/fenced-divs.md`:** body
      continuation, admissible term lines, `four_space_rule` deleted, the
      caption exclusion, laziness, absorption end, the marker-only form, term
      parsing, no interruption, scopes; the opener and closer grammar, no oracle
      clause, interruption allowed, unclosed scope, the directive rule, eligible
      closers. Closes PD-1 through PD-10, PF-1 through PF-7.
- [ ] **C4 — `pandoc/headings-and-anchors.md` and `pandoc/lists.md`:** the
      projection table, the lowercase mapping, categories, `base-N`, no
      diagnostics, footnote bodies, virtual definitions; `start >= 0`, inherited
      markers off, `startnum` scope, `@` only in parentheses, the Roman grammar,
      padding, committed style, nesting, `(N@)`, labels, `(@label)` placement,
      repeated labels, continuation columns, no overflow sentence, interruption,
      `#)` and `(#)`. Closes PH-1 through PH-7, PL-1 through PL-16, decision
      D-3.
- [ ] **C5 — `pandoc/superscript-and-subscript.md`:** the delimiter-stack model,
      whitespace, the `\ ` scope, single tildes, the footnote rule pointer,
      block identifiers and autolinks, no oracle justification. Closes PU-1
      through PU-7, decision D-8.
- [ ] **C6 — `pandoc/tables.md`:** no cell normalization, inline captions, the
      width formula, scalar columns, the caption grammar, captions between
      tables, every table form, the simple-table grammar, alignment,
      segmentation, precedence against Setext and thematic breaks, the multiline
      boundaries, joining, syntax order, the boundary union, grid lines and
      spans, colons, `=` lines, cell text, rectangular scopes, fallback, the
      dump encoding, `^id` after captions; and the stale normalization text in
      `docs/plans/2026-09-03-pandoc-markdown-extensions.md`. Closes PT-1 through
      PT-23.
- [ ] **D1 — the seams:** land the recognition-order tables in
      `canonical-ast.md` and replace every module's precedence paragraph with a
      pointer, in one pull request after A1 through C6, so no module keeps a
      private order. Closes S-0 through S-12 and X-1.

When every box is ticked, the extension set above is closed: every option has a
defining module that states its grammar, its option-off output, its malformed
fallback, its scopes, and its place in the one recognition order, and no rule is
defined by reference to an oracle.
