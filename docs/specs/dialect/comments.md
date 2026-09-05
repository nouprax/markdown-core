# Comments

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the `Comment` kind, the HTML-comment rule of the inherited grammar, and
the `%%` comment syntax. Option: `comments` (default `false`) for `%%`; HTML
comments have no option. Sources: CommonMark's HTML comment token and block;
Obsidian's `%%` comments. Executable oracles: cmark for the HTML token and
block boundaries; `@quartz-community/remark-obsidian` for `%%`, whose
stripping is a registered projection. Landing: `M0` for the kind and the HTML
rule, `O3` for `%%`.

## Model

```text
Comment(literal: String)
```

`Comment` is a leaf and the one kind that is valid in both block content and
inline content; its parent edge records which, and the node stores no
placement field. `literal` excludes the delimiters and preserves every byte
between them, line endings and indentation included, exactly as written after
container-prefix removal.

Nothing strips a comment. Every recognized comment of either grammar is a
`Comment` node; a consumer that does not want comments drops the nodes. There
is no retention option, and `stripHTMLComments` is removed.

## HTML comments

Under the inherited grammar, with no option:

- An inline HTML comment token `<!-- ... -->` is an inline `Comment` whose
  literal is the bytes between `<!--` and `-->`; the tokens `<!-->` and
  `<!--->` are comments with an empty literal.
- An HTML block that opens with `<!--` and whose end line holds only
  whitespace after the first `-->` is a block `Comment` whose literal is the
  bytes between `<!--` and that `-->`.
- Every other HTML block, including one whose end line carries non-whitespace
  after `-->`, stays `HTMLBlock` as written, and every other HTML token stays
  `HTML`.

The token and block boundaries are the inherited ones; this module changes
only the kind produced.

## `%%` comments

With `comments=true`:

- The opener is the first two `%` of a run of percent signs that is not
  preceded by an unescaped backslash. The body ends at the first later `%%`.
  Backslashes inside the body are ordinary bytes. `%%%%` is an empty comment;
  `%%%a%%%` is `Comment("%a")` followed by text `%`; `\%%` is text.
- Block form, step 4 of the block-start order: after container prefixes, a
  line whose content is `%%` at indentation zero to three followed only by
  spaces or tabs opens a candidate, which may interrupt a paragraph. The
  candidate scans forward, interpreting no block syntax, for the first later
  line whose content is exactly `%%` under the same prefixes and indentation
  bound; intervening lines carry the prefixes and may be blank. If found, the
  candidate commits as a block `Comment` whose literal is the intervening
  lines after prefix removal; otherwise the opener line is paragraph text and
  the inline rule applies to it.
- Every other `%%` is an inline comment, inline step A5. Its body may span the
  lines of one inline container but not a block boundary: in `%%`, `# h`,
  `end %% x`, the heading is a heading and both `%%` are text. An unmatched
  opener is text and hides nothing.
- A comment suppresses all recognition, inherited and extension, until its
  closer. Code spans, HTML tokens, and formulas are earlier class-A steps, so
  a `%%` inside them is their byte.
- Table boundary scanning does not recognize comments: a `|` inside `%%...%%`
  on a table row splits the cell and the unmatched `%%` bytes are text, and a
  `\|` inside a comment in a table cell becomes `|` in the literal.

## Option behavior

With `comments=false`, `%%` is ordinary text everywhere, and HTML comments are
still `Comment` nodes. Every module that says "comment" means a `Comment` node
of either grammar.

## Scopes

An inline `Comment.scope` covers both delimiters and the body. A block
`Comment.scope` covers the opener line through the closer line. An HTML
comment's scope is the inherited token or block range.

## Required conformance cases

Tests cover inline, block, multi-line inline, empty, adjacent, escaped, and
unmatched `%%` comments; runs of three or more percent signs; block candidates
with and without a closer, inside containers, and interrupting a paragraph;
Markdown-looking and extension-looking bodies for every merged feature; a
comment inside code, HTML, and formulas; comments in table rows and cells;
`<!-->`, `<!--->`, inline and block HTML comments, and a block whose end line
carries content after `-->`; an HTML comment beside a `%%` comment; the
callout-title and mark-content compositions named by those modules; exact
literals and scopes; option-off output; allocation failure; and size-doubling
percent runs.
