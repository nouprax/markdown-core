# Callouts

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the `Callout` kind, which every `>` container produces, and the option
`callouts` (default `false`) that recognizes `[!type]` metadata. Source:
Obsidian's callouts. Executable oracle: none; the Obsidian package does not
parse callouts, so product fixtures are the oracle of record. Landing: the
kind with `M3`, metadata with `O8`; until `M3` the current contract's
`BlockQuote` stands.

## Model

```text
CalloutFold = none | expanded | collapsed

Callout(variant: String?, fold: CalloutFold, title: [Markup]?,
        content: [Markup])
```

Every source container introduced by `>` produces a `Callout`. A plain quoted
block is a callout without metadata; the AST has no `BlockQuote` kind, alias,
or wrapper. The inherited algorithm owns the `>` prefix, laziness,
continuation, blank-line, and nesting rules; this module changes the kind and
adds optional metadata without a second container parser. `title` is a
node-valued field visited before `content` and is not counted in `children`.

- `variant == null` means no valid metadata line; then `title == null`,
  `fold == none`, and every quoted byte is represented by `content`.
- A non-null `variant` is the authored type as written, case preserved;
  matching against a type list is consumer policy.
- `fold == none` means no `+` or `-` was authored; `+` is `expanded` and `-`
  is `collapsed`.
- `title == null` means no title bytes were authored; otherwise `title` is
  the parsed inline content of the title, which may consist of one `Comment`.

Default titles, built-in aliases, icons, colors, custom CSS types, and the
current fold state are renderer data. An unknown type stays its `variant`;
the parser substitutes nothing.

## Metadata grammar

With `callouts=true`, the candidate is the first line of the container after
the `>` prefix and its optional space are removed; up to three further spaces
may precede `[!`. A blank first line, four or more spaces, or any other
leading byte means no metadata.

```text
metadata-line = "[!" type "]" [ fold-marker ] ( 1*sep title / *WSP EOL )
type          = 1*( ASCII-letter / DIGIT / "_" / "-" )
fold-marker   = "+" / "-"
sep           = SP / TAB
```

The `]`, or the fold marker when present, must be followed by a space, a tab,
or the end of the line; otherwise the line is not a metadata line and the
callout is metadata-free, so `[!note]Title` and `[!faq]+Title` are content.
Trailing spaces and tabs are removed before the title is parsed, and a title
never contains `SoftBreak` or `LineBreak`. Metadata is decided when the first
line is consumed, before Setext resolution, so a following underline belongs
to the body. The paragraph that began on the metadata line is split: its
remaining lines, lazy lines included, form a `Paragraph` whose scope starts at
the first byte of the second line, and with no remaining lines the body is
empty. Metadata is evaluated independently for every nested container. A
block identifier candidate on a metadata line is title text; the
[block identifiers](block-identifiers.md) module attaches after metadata is
extracted.

## Option behavior and fallback

With `callouts=false`, `[!type]` is paragraph text inside a metadata-free
`Callout`. With the option on, an invalid type, a marker after the first
line, or a marker separated from the first position by other content is
ordinary content of a metadata-free `Callout`; no second kind exists.
GitHub's alerts spell a subset of this grammar and are recorded in the
[conflicts](conflicts.md) register.

## Scopes

`Callout.scope` covers every quote marker, the metadata line, the title, and
the body. `title` child scopes cover the title bytes only.

## Required conformance cases

| Source shape       | Required state                                    |
| ------------------ | ------------------------------------------------- |
| `> quote`          | `variant=null`, `title=null`, `fold=none`         |
| `> [!info]`        | `variant="info"`, empty body                      |
| `> [!TIP] Title`   | `variant="TIP"`, inline title, empty body         |
| `> [!faq]+ Title`  | `fold=expanded`                                   |
| `> [!faq]- Title`  | `fold=collapsed`                                  |
| `> [!custom-type]` | custom type retained as `variant`                 |
| `> [!note]Title`   | metadata-free, content `[!note]Title`             |
| `> [!note] %%t%%`  | `title` holds one `Comment`                       |

Tests also cover formatted titles, body blocks, title-only callouts, every
built-in alias spelling stored as written, invalid type characters, misplaced
markers, leading spaces before `[!`, a blank first line, nested combinations,
a Setext underline after the metadata line, attached block identifiers,
lazy continuation, exact scopes, option-off output, allocation failure, and
adversarial quote depth.
