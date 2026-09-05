# Citations

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `citations` (default `false`). Source: Pandoc's citation syntax.
Executable oracle: the Pandoc 3.11 CLI under `specs/oracles/pandoc/`.
Landing: `P7`, the first producer of `CitationReferent.bib`. The `Cite`,
`Citation`, and `CitationReferent` values are defined by the
[footnotes](footnotes.md) module; this module produces the `bib` branch and
never touches footnote recognition.

## Keys

```text
key-char        = unicode-letter / unicode-number / "_"
key-punctuation = ":" / "." / "#" / "$" / "%" / "&" / "-" / "+" / "?" /
                  "<" / ">" / "~" / "/"
bare-key        = key-char *( [ key-punctuation ] key-char )
braced-key      = "{" 1*( braced-key-character / braced-key ) "}"
braced-key-character
                = any non-whitespace scalar except "{" and "}"
citation-key    = "@" ( bare-key / braced-key )
```

In a bare key a punctuation scalar must be followed by a key character, so
`@Foo_bar.baz.` has key `Foo_bar.baz` and `@Foo_bar--baz` has key `Foo_bar`.
A braced key is non-empty, whitespace-free, and balanced before the end of the
same inline container; braces owned by code spans or HTML tokens do not count.
The outer braces are excluded from the stored key. Keys are stored exactly
after delimiter removal and are neither case-folded nor resolved.

A `@`, or the `-` of `-@`, opens a candidate only at the start of the inline
container or when the preceding scalar is not a letter, number, or `_`, so
`foo@bar`, `1@bar`, and `(@bar` open nothing here; this holds independently
of `autolinks`, and a `@` inside an autolink token is that token's byte. An
escaped `\@` is text.

## Bracketed groups

Recognition of a group is alternative 5 of the bracket procedure of the
[links and images](links-and-images.md) module: it is tested after a direct
tail, a resolving reference tail, and an enabled valid span container have
failed, and before the shortcut-reference alternative.

```text
bracketed-group = "[" spacing citation-item *( ";" spacing citation-item )
                  spacing "]"
citation-item   = prefix [ "-" ] citation-key suffix
spacing         = optional whitespace with at most one line ending
```

A group produces one `Cite` with one `Citation` per item in source order.
Semicolons separate items and belong to neither affix. `prefix` is the inline
content before the item's key and optional mode marker; `suffix` is the inline
content after the key up to the next item boundary. Both exclude leading and
trailing whitespace, may be empty, and may contain nested inline markup; a
suffix of only whitespace is empty. `[see @doe99, pp. 3]` has prefix `see` and
suffix `, pp. 3`. An unescaped `-` immediately before `@` is always the mode
marker: it selects `suppressAuthor`, is excluded from the affixes and the key,
and is where the opener precondition is evaluated, so `[Smith-@1990]` has
prefix `Smith` and a suppressed-author key `1990`. Every other item has mode
`normal`.

A group in which any item lacks a key is not a citation, and the bracket pair
continues at the shortcut-reference alternative. A non-resolving reference
tail does not block a group: `[@foo][nope]` is a `Cite` followed by literal
`[nope]`. With `bracketedSpans` on, `[@foo]{.key}` is a `Span` containing an
author-in-text `Cite`; with it off, the container is literal text after the
bracketed `Cite`. Curly braces inside a suffix, such as `[@smith{ii, A, D-Z},
with a suffix]`, are suffix text; their locator meaning belongs to a CSL-aware
consumer and is not represented.

## Author-in-text keys

An unbracketed citation key is inline step A8 and produces a one-item `Cite`
whose referent mode is `authorInText`; `-@key` outside brackets produces
`suppressAuthor`. An immediately following bracketed tail belongs to the sole
item's suffix without its brackets:

```markdown
@smith04 says blah.
@smith04 [p. 33] says blah.
```

Optional spaces or tabs and at most one line ending may separate the key from
the `[`. The tail is not claimed when it begins with `^`, or when its `]` is
immediately followed by `(`, `[`, or a valid attribute container, in which
case the bracket pair is decided by the bracket procedure on its own. A tail
that itself contains items, `@k [s1; @k2, s2]`, produces one `Cite` whose
first item is author-in-text with suffix `s1`, followed by the further items.

A bare `@label` with no bracketed tail whose label is registered as an example
label anywhere in the document under `exampleLists` is an `ExampleReference`
rather than a `Cite`; the [lists](lists.md) module states that rule, and the
choice is finalized document-wide so parser order cannot change it.

## Non-normative notes

Locator recognition, the default page locator, and the meaning of braces in
suffixes are behaviors of a citation processor. The exact heading class
`reset-citation-positions` on a heading whose parent is `Document` asks such a
processor to reset position-sensitive state; the parser stores the class as
written through the [attributes](attributes.md) module and does nothing else.

## Option behavior and fallback

With `citations=false`, `@` has no meaning and every bracket pair follows the
other alternatives; footnote `Cite` nodes are unaffected. A failed candidate
releases its opener and consumes nothing. Inline code, comment bodies, HTML
tokens, formulas, and cross links are opaque; a semicolon inside an opaque
child is not an item separator. A line the inherited grammar accepts as a link
reference definition is one regardless of a leading `@`.

## Scopes

A bracketed `Cite.scope` covers its outer brackets and contents. Each
`Citation.scope` runs from the first non-whitespace byte after `[` or `;` to
the last non-whitespace byte before `;` or `]`. An author-in-text `Cite` and
its item both run from the mode marker or `@` through the tail's closing `]`,
or through the key when no tail is claimed. Affix child scopes cover visible
authored content only.

## Required conformance cases

Tests cover single and multiple items; prefixes, formatted suffixes, and
semicolon boundaries; every key punctuation scalar, terminal and repeated
punctuation, and braced keys with nesting and URLs; the opener precondition
after letters, digits, underscores, and punctuation; all three modes; spacing
after `[` and `;`; items without keys; author-in-text tails with and without
items, with a `^` start, and with a following `(`, `[`, or container;
reference-tail and span precedence with each option on and off; example
labels before and after their definitions; escaped openers, email addresses,
code, comments, HTML, and formulas; definitions with a leading `@`; exact
group, item, and affix scopes; option-off output; allocation failure; and
adversarial runs of `@`, punctuation, braces, brackets, and semicolons.
