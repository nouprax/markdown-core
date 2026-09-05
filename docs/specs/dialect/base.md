# Base language

Status: normative module of the [Markdown Core dialect](../dialect.md). This
module states what the dialect inherits and the few rules the inherited layer
leaves to the implementation. It has no option of its own except
`smartPunctuation` (default `true`). Source: the CommonMark specification
0.31.2. Executable oracle: cmark 0.31.2 under `specs/oracles/cmark/`, run with
every dialect option off. Landing: present; the HTML-comment rule lands with
`M0` and the resolved-reference model with `M1` and `M2`.

## The inherited layer

The base of the dialect is CommonMark 0.31.2 as implemented by the pinned
cmark, with the deltas registered in `specs/oracles/cmark/deltas.json`. Every
block and inline construct of that specification is recognized exactly as it
defines: block structure, container prefixes, laziness, tabs, indentation,
Setext and ATX headings, thematic breaks, fenced and indented code, HTML
blocks, link reference definitions, paragraphs, backslash escapes, character
references, code spans, emphasis and strong emphasis, links, images,
angle-bracket autolinks, raw HTML, and hard and soft line breaks.

On top of it sit the GFM extensions, each in its own module with the pinned
cmark-gfm as executable oracle: [pipe tables](tables.md),
[strikethrough](strikethrough.md), [bare autolinks](links-and-images.md),
[task lists](task-lists.md), and [footnotes](footnotes.md). GFM is one source
and one oracle, cmark-gfm at its pinned commit minus the registered deltas;
the GFM specification text is not a second authority over it.

"Inherited" in every module means the output of this layer with the module's
option off. The eight inherited options (`smartPunctuation`, `footnotes`,
`tables`, `strikethrough`, `autolinks`, `taskLists`, `formulas`,
`directives`) default to `true`; with all of them off, the output is the
CommonMark parse of the source.

The CLI flags `--liberal-html-tag` and `--strikethrough-double-tilde` are
harness flags with no dialect meaning; the second is removed by `P6`.

## Kinds of the base language

The base language produces these kinds of
[`canonical-ast.md`](../canonical-ast.md) without any dialect extension:
`Document`, `Paragraph`, `Heading`, `ThematicBreak`, `List` and `ListItem`,
`CodeBlock`, `HTMLBlock`, `Text`, `SoftBreak`, `LineBreak`, `Code`, `HTML`,
`Emphasis`, `Strong`, `Link`, and `Image`, plus `BlockQuote` until `M3` renames
it to `Callout`, and the reference kinds until `M2` resolves them. The rules
below fix what the CommonMark specification leaves to the implementation.

### Text

Adjacent `Text` nodes in one content array are merged into one node whose
scope runs from the first's start to the last's end. `a\*b` is therefore one
`Text` node with literal `a*b`, and a `children` count is never undetermined
by escape or character-reference boundaries. A `Text` node is never empty.

### Line breaks

`SoftBreak` covers the line-ending bytes of a soft break. `LineBreak` covers
the line-ending bytes together with the backslash or the two-or-more trailing
spaces that produced it. Neither node's literal is stored; a consumer that
needs the bytes reads the scope.

### Code

`CodeBlock.literal` holds the block's content bytes as written, with the
inherited indentation removal and no other transformation. `fenced` is `true`
for a fenced block and `false` for an indented one. `closed` is `true` if and
only if a closing fence line was found; an indented block is always `closed`.

`CodeBlock.info` is the fence's info string after CommonMark backslash-escape
and character-reference processing and after stripping leading and trailing
spaces and tabs, with no other transformation. `info` is `null` for an
indented block and for a fence whose info string is empty after stripping.
`CodeBlock.language` is the maximal prefix of `info` before the first space or
tab, or `null` when `info` is `null`. Nothing is lowercased, aliased, or
derived from a class: `c++` is the language `c++`. Under `formulas` a fence
whose `info` is exactly `formula` produces a `FormulaBlock` instead, as the
[formulas](formulas.md) module states, and under `fencedCodeAttributes` the
[attributes](attributes.md) module removes an attribute container from the
info region before this rule computes `info`.

`Code.literal` is the code span's content after the inherited stripping and
line-ending-to-space conversion.

### HTML

An inline raw HTML token is an `HTML` leaf holding the token bytes as
written, except that an HTML comment token is a `Comment`, as the
[comments](comments.md) module states. An HTML block is an `HTMLBlock` holding
its lines as written, except that an HTML block that opens with `<!--` and
whose end line holds only whitespace after the first `-->` is a block
`Comment`. Paired opening and closing tags do not establish an element region:
`<span>**bold**</span>` is `HTML`, `Strong`, and `HTML` siblings, and every
module recognizes its syntax between separate HTML tokens. HTML declarations,
processing instructions, CDATA sections, and malformed tags follow the
inherited grammar and are `HTML` or `HTMLBlock`.

### Links, images, and references

Direct links and images, angle-bracket autolinks, reference definitions, and
the full, collapsed, and shortcut reference forms follow the inherited grammar.
Their consumer model, including `Destination`, unescaping, the resolved
reference model, and the fallback of an unresolved reference, is stated by the
[links and images](links-and-images.md) module. A link reference definition is
parser state and produces no node once `M2` lands; until then the current
contract's `ReferenceDefinition`, `LinkReference`, and `ImageReference` stand.

A footnote definition line `[^label]:` is never a link reference definition,
and a line the inherited grammar accepts as a link reference definition is one
regardless of a leading `@` or any other byte that another module would read
inline; block starts are decided before inline recognition.

### Headings

`Heading.content` is the inline content of the heading after the inherited
removal of the ATX opening and closing sequences and of the Setext underline.
An attribute container at the end of the heading is removed first under
`headingAttributes`, as the [attributes](attributes.md) module states. A
heading's `anchor` is `null` unless an explicit or automatic rule of the
[anchors](anchors.md) module populates it.

### Block quotes

Every `>` container is a `BlockQuote` in the current contract and a `Callout`
with `variant=null`, `fold=none`, and `title=null` once `M3` lands; the
[callouts](callouts.md) module owns that kind. The inherited prefix, laziness,
continuation, and blank-line rules are unchanged by any option.

## Smart punctuation

With `smartPunctuation=true`, these substitutions are made in `Text` literals
only, during inline parsing, and change no node boundary:

- A run of two or more hyphens becomes dashes: a run divisible by three
  becomes that many em dashes (U+2014); otherwise a run divisible by two
  becomes that many en dashes (U+2013); otherwise a run of `3k+2` hyphens
  becomes `k` em dashes then one en dash, and a run of `3k+1` hyphens becomes
  `k-1` em dashes then two en dashes. A single hyphen is unchanged.
- Three periods become one ellipsis (U+2026). Two periods are unchanged.
- `'` and `"` are delimiter runs on the shared stack under the inherited
  flanking rules. A matched pair becomes U+2018 and U+2019, or U+201C and
  U+201D. An unmatched `'` becomes U+2019. An unmatched `"` becomes U+201D
  when it could close and U+201C otherwise.

Backslash-escaped characters and characters inside code spans, HTML tokens,
formulas, comments, and every other opaque construct are never substituted.
With the option off, every byte above is ordinary text.

## Required conformance cases

The cmark specification corpus is replayed in full by the cmark gate. The
package fixtures additionally cover `Text` merging across escapes and
character references, `SoftBreak` and `LineBreak` scopes, `info`, `language`,
`fenced`, and `closed` on every code-block form, the `formula` info word,
every HTML block type and inline token including the comment forms, Markdown
between paired HTML tags, every smart-punctuation rule above with the option on
and off, and the four inherited link forms with and without a definition.
