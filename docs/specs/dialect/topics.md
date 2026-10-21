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

A segment character is a letter, a combining mark, an emoji, `_`, `-`, a
digit, or a joiner. Letter, mark, and digit mean the Unicode categories L, M,
and N; an emoji is a character with the Unicode Emoji property outside ASCII.
The joiners are those twitter-text accepts in a hashtag: U+200C, U+200D,
U+A67E, U+05BE, U+05F3, U+05F4, U+FF5E, U+301C, U+309B, U+309C, U+30A0,
U+30FB, U+3003, U+0F0B, U+0F0C, and U+00B7. A name is one or more runs of
segment characters separated by single `/` signs. A `/` belongs to the name
only when a segment character follows it, so a name never starts or ends with
`/`. The name is the longest such sequence after the `#`, and it must contain
at least one segment character that is neither a digit nor a joiner. Names
retain case and spelling, without normalization.

```markdown
#定价 #हिन्दी #🎉 #v2 #a_b-c/d #- #2026 #1/2 #1·2 #/a #a/
```

The first six are topics named `定价`, `हिन्दी`, `🎉`, `v2`, `a_b-c/d`, and
`-`. `#2026`, `#1/2`, `#1·2`, and `#/a` stay text. In `#a/`, the topic is `a`
and the `/` is text.

## Where a topic starts and ends

A `#` opens a topic at the start of an inline container or after a character
that is none of a letter, a mark, a decimal digit (Nd), `_`, a joiner, and
`&`, which begins a character reference with it, as twitter-text bounds a
hashtag. Thus `page#pricing`, `C#`, `a_#b`, and `&#abc;` stay text, while
`(#pricing)`, `a/#b`, and `**bold**#pricing` contain a topic. Escaped `\#`
stays text.

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
