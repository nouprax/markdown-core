# Topics

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Write `#` followed by a name to mark a topic, as on social platforms.

```markdown
Revisit #pricing before the #2026/launch review.
```

This produces two `Topic` nodes, named `pricing` and `2026/launch`. A name
excludes the `#`. Topics carry no meaning beyond their name; applications
decide how to display, collect, or link them.

## Topic names

A segment character is a letter, a digit, `_`, or `-`, with letter and digit
meaning the Unicode categories, as for citation keys. A name is one or more
runs of segment characters separated by single `/` signs. A `/` belongs to
the name only when a segment character follows it, so a name never starts or
ends with `/`. The name is the longest such sequence after the `#`, and it
must contain at least one segment character that is not a digit. Names retain
case and spelling, without normalization.

```markdown
#定价 #v2 #a_b-c/d #- #2026 #1/2 #/a #a/
```

The first four are topics named `定价`, `v2`, `a_b-c/d`, and `-`. `#2026`,
`#1/2`, and `#/a` stay text. In `#a/`, the topic is `a` and the `/` is text.

## Where a topic starts and ends

The name characters are the segment characters and `/`. A `#` opens a topic
at the start of an inline container or after a character that is neither a
name character nor `&`, which begins a character reference with it. Thus
`page#pricing`, `C#`, and `&#abc;` stay text, while `(#pricing)` and
`**bold**#pricing` contain a topic. Escaped `\#` stays text.

The name ends at the first character that is not part of it, so `#pricing.`
and `#定价，` are topics followed by punctuation. A name followed immediately
by `#` or by `://` is not a topic: `#a#b` stays text, and in
`#https://example.com` the `#` is text before an automatic link.

A heading needs whitespace after its opening signs, so `#pricing` at the start
of a line is a paragraph containing a topic, not a heading. Inside heading
content, `# Plan #pricing` contains a topic; it is not a closing sequence.

## Boundaries

A [block identifier](block-identifiers.md) is decided before inline content
is parsed: a final `#summary#` gives the paragraph an anchor and creates no
topic. A `#` elsewhere follows the topic rules, including one in text that a
failed construct released, such as an unclosed `{#id`.

Opaque code, comment, HTML, formula, cross-link, link-destination, and
automatic-link contents keep their own bytes, so `https://example.com/#pricing`
and `[Plans](#pricing)` contain no topic. A topic claims its name before
[email recognition](links-and-images.md#automatic-links) reads the remaining
text: `#team@example.com` is the topic `team` followed by text.

A topic has no attribute container; a following `{...}` is ordinary text or
belongs to its own site.
