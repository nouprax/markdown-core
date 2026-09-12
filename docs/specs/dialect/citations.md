# Citations

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use an `@key` inside brackets to cite bibliography entries.

```markdown
See [@doe2024].

Compare [see @doe2024, pp. 3–5; @lee2023, chapter 2].
```

Each bracketed group becomes one `Cite`, containing one `Citation` per item in
source order. Each item has a bibliography key and a mode, with parsed prefix
and suffix content. The second group has two items; the first prefix is `see`
and its suffix is `, pp. 3–5`. Semicolons and surrounding affix whitespace are
not stored as affix content.

The parser does not look up entries, select a citation style, interpret page
locators, or build a bibliography. Keys need no declaration to be recognized.

## Author and suppression modes

```markdown
@doe2024 explains the result.

-@doe2024

[see -@doe2024, p. 8]
```

A bare key uses `authorInText`. An eligible `-@` uses `suppressAuthor` and omits
the mode marker from the key and affixes. Ordinary bracketed items use `normal`.
An opener must be at the start of the inline container or after a scalar other
than a letter, number, or underscore. Thus `foo@bar` is not a citation opener.

For `-@`, that condition is first tested at `-`. In `[Smith-@1990]`, the hyphen
is prefix text and the citation has normal mode; `[Smith -@1990]` suppresses
the author. Escaped `\@` stays text.

## Citation keys

Bare keys begin with a Unicode letter, number, or underscore. Their remaining
runs can contain those characters separated by one of `: . # $ % & - + ? < >
~ /`; punctuation must be followed by a key character. A final period therefore
stays outside the key.

```markdown
@Foo_bar.baz. and @{https://example.com/paper}
```

The keys are `Foo_bar.baz` and `https://example.com/paper`. Braced keys must be
nonempty, whitespace-free, and balanced; the outer braces are removed. Nested
braces are allowed, while braces owned by code or HTML tokens do not count.
Keys retain case and spelling, without normalization or resolution.

## Affixes and tails

Prefix and suffix content can include inline formatting:

```markdown
[see @doe2024, *especially* chapter 2]

@doe2024 [p. 33; @lee2023, p. 8]
```

The second example is one `Cite` combining an author-in-text item with a normal
item. A bare key's bracketed tail can follow spaces/tabs and at most one line
ending. If the first tail section contains no eligible key, it becomes the
external author's suffix. If it contains a key, it becomes another item and
the external author's suffix is empty. Every later semicolon-separated section
must contain a key.

In a group item, the first eligible key is its referent; later keys in its
suffix can parse as nested author-in-text citations. Braces in suffixes are
ordinary suffix text, not a separate locator field. Group spacing permits at
most one line ending at each spacing boundary.

## Boundaries and fallback

A group whose item lacks a key is not a citation group. Other bracket rules
are then tried, and valid inner keys can still parse as ordinary citations.
Semicolons inside opaque children are not item separators.

Direct links, resolving reference tails, and spans take precedence over groups.
A tail beginning with `^`, or followed immediately by `(`, `[`, or a valid
attribute container, is left to its own bracket syntax. Ordinary link
reference definitions are decided before citation parsing.

A bare `@label` with no bibliography tail becomes a
[specimen reference](specimens.md) if that label is declared anywhere in the
document. `[@label]` remains a bibliography citation. Opaque code, comment,
HTML, formula, cross-link, and automatic-link contents keep their own bytes.
