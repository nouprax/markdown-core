# Markdown syntax guide

Markdown Core parses a fixed Markdown dialect on every platform. You can use
all the syntax below in the same document without enabling extensions or
choosing a mode.

The pages follow a simple pattern: how to write the syntax, a Markdown example,
what the parser returns, and any boundaries to watch for. A name such as
`Paragraph` or `Link` refers to a node in the [AST](canonical-ast.md). The parser
returns this structure; your application decides how to display it.

## Start here

| Write | Guide |
| --- | --- |
| Paragraphs, blank lines, indentation, literal punctuation | [Basics](dialect/base.md) |
| Titles and sections | [Headings](dialect/headings.md) |
| Italic and bold text | [Emphasis](dialect/emphasis.md) |
| Soft and hard line breaks | [Line breaks](dialect/line-breaks.md) |
| Horizontal separators | [Thematic breaks](dialect/thematic-breaks.md) |
| Inline and fenced code | [Code](dialect/code.md) |
| Raw HTML | [HTML](dialect/html.md) |

## Format text

| Write | Guide |
| --- | --- |
| Deleted text | [Strikethrough](dialect/strikethrough.md) |
| Highlighted text | [Highlights](dialect/marks.md) |
| Inserted text | [Insertions](dialect/insertion.md) |
| Superscripts and subscripts | [Superscript and subscript](dialect/superscript-and-subscript.md) |
| Text with attached classes or other metadata | [Bracketed spans](dialect/bracketed-spans.md) |
| Inline and display mathematics | [Formulas](dialect/formulas.md) |
| Retained notes hidden by a renderer | [Comments](dialect/comments.md) |

## Link and refer

| Write | Guide |
| --- | --- |
| Links, images, reference definitions, automatic URL links, image sizes | [Links and images](dialect/links-and-images.md) |
| Internal links and embedded resources | [Cross links and embeds](dialect/cross-links.md) |
| Heading targets and implicit references | [Anchors](dialect/anchors.md) |
| Targets on other blocks | [Block identifiers](dialect/block-identifiers.md) |
| Referenced and inline notes | [Footnotes](dialect/footnotes.md) |
| Bibliography references and citation groups | [Citations](dialect/citations.md) |
| Numbered examples and references to them | [Specimens](dialect/specimens.md) |

## Organize content

| Write | Guide |
| --- | --- |
| Bullet, numbered, alphabetic, and Roman lists | [Lists](dialect/lists.md) |
| Checkboxes and custom task markers | [Task lists](dialect/task-lists.md) |
| Terms with one or more definitions | [Definition lists](dialect/definition-lists.md) |
| Quotes and titled or collapsible notices | [Quotes and callouts](dialect/callouts.md) |
| Pipe, simple, multiline, and grid tables with captions | [Tables](dialect/tables.md) |
| Named directives and fenced containers | [Directives](dialect/directives.md) |
| Document properties | [Properties](dialect/properties.md) |
| IDs, classes, and key/value annotations | [Attributes](dialect/attributes.md) |

## Ground rules

The base grammar follows CommonMark 0.31.2, with the additions and differences
specified by these pages. Familiar GitHub, Obsidian, Pandoc, and directive
spellings coexist, but Markdown Core does not select a separate upstream mode.
See [compatibility and precedence](dialect/conflicts.md) before assuming another
application's behavior applies unchanged.

Blocks are identified before their inline text is parsed. Code, formula and
comment bodies, raw HTML tokens and blocks, complete cross links, and automatic
URL tokens keep their own contents literal. Two inline HTML tags do not make
the text between them literal.

Incomplete syntax generally falls back to ordinary Markdown at the point it
failed. Some valid open containers, such as fenced code, may end at the end of
the document without a closing fence; each page states that behavior. An
invalid candidate does not justify dropping source text.

Quoted strings, hyphens, periods, and emoji characters stay as written unless
a syntax rule explicitly decodes or transforms them. There is no smart
punctuation or emoji shortcode expansion. Comments are retained. No parse
fetches a URL, opens a workspace file, evaluates code, or formats a bibliography.

## Examples

Fenced `markdown` blocks in this guide are source to copy into a document.
Text immediately below explains the parsed result. A trailing newline is
optional unless a particular example needs another line. Delimiters are part
of the node's source range even when they disappear from its visible content.
For full diagnostic output, see the [debug dump guide](canonical-ast-dump.md).

## Unicode and text

Input to the C API must be valid UTF-8; the other bindings provide valid UTF-8
to the engine. Line endings may be LF, CR, or CRLF. Indentation uses tab stops
four columns apart. Syntax matching uses bundled Unicode 17 tables, rather than
the host operating system's Unicode version.

Unless a rule specifies otherwise, CommonMark whitespace means Unicode Zs
plus tab, LF, form feed, and CR; ASCII whitespace means U+0009 through U+000D
and space. Letters, numbers, combining marks, and punctuation use the Unicode
categories; CommonMark punctuation also includes symbols. Reference-label
normalization uses full case folding, trims ASCII whitespace, and collapses
internal ASCII whitespace runs. Automatic anchors use their own
[lowercase algorithm](dialect/anchors.md#automatic-anchors).

## Limits

| Syntax | Limit | Beyond the limit |
| --- | --- | --- |
| Link or footnote label | 1,000 bytes | The label is not recognized. |
| Code-span backtick run | 80 backticks | The run is literal text. |
| Directive label | 32 nested brackets | The directive has no label; the failed bracket source is parsed normally. |
| Footnote definition nesting | Depth must be below 100 | The line is ordinary content. |
| Decimal list marker or specimen reset | 9 digits | The marker is not recognized. |
| Roman list marker value | 999,999,999 | The marker is not recognized. |
| Image width or height | 2,147,483,647 | The suffix remains label content. |
| Pipe-table row | 65,535 cells | An oversized header is not a table; an oversized body row ends the table. |
| Filled-in pipe-table cells | 524,288 | Once the count exceeds this, no further rows are accepted. |

Properties have a fixed set of ten supported field names. Unknown fields are
ignored. General block and inline delimiter nesting have no fixed syntax-depth
limit. Allocation failure aborts the parse and returns no partial document.

## Scopes

Every node records the source of its own occurrence. Source coordinates use
one-based lines and UTF-8 byte columns, with inclusive ends. A resolved reference
keeps its own range; it does not acquire its definition's range. Generated
values add no fictional source positions. See the
[coordinate contract](canonical-ast.md#coordinates) for empty ranges, line-ending
sentinels, table cells, and the other precise boundary rules.

## Further reading

The presentation of this guide follows the example-first approach of
[GitHub's writing guide](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax)
and [Obsidian's syntax help](https://help.obsidian.md/syntax).
Those sites describe their own products; the pages here describe Markdown Core.
The [conformance guide](../architecture/syntax-conformance.md) records how this
language is checked against pinned external parsers and shared fixtures.
