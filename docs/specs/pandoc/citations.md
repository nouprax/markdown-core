# Pandoc bibliography citations

Status: normative target module for the optional `citations` parser extension.
Authority:
[Pandoc User's Guide — Citation syntax](https://pandoc.org/MANUAL.html#citation-syntax)
at the Pandoc 3.11 snapshot pinned by the
[Pandoc extension index](../pandoc.md). The shared consumer types are defined
by the [citation model](../citation-model.md).

This module recognizes only bibliography source syntax and produces
`CitationReferent.bib`. Footnote syntax may produce the other referent branch
without enabling `citations`.

## Citation keys

A citation key begins after `@`:

```text
bare-key-start = Unicode-letter | Unicode-digit | "_"
key-punctuation = ":" | "." | "#" | "$" | "%" | "&" | "-" |
                  "+" | "?" | "<" | ">" | "~" | "/"
bare-key = bare-key-start *( Unicode-letter | Unicode-digit |
                             "_" | key-punctuation )
braced-key-character = non-whitespace character other than "{" or "}"
braced-key = "{" 1*( braced-key-character | braced-key ) "}"
citation-key = "@" ( bare-key | braced-key )
```

In a bare key, punctuation is internal only when it is single and does not end
the key. Adjacent punctuation terminates the key before that run. Thus
`@Foo_bar.baz.` has key `Foo_bar.baz`, while `@Foo_bar--baz` has key
`Foo_bar`. Braces permit otherwise invalid keys, including balanced nested
braces and URL-shaped values; the outer pair is excluded from `key`.

A braced key must be non-empty, whitespace-free, and balanced within the
current inline parsing subject. Malformed input creates no cite and cannot
consume bytes needed by later inline constructs. Keys are stored exactly after
delimiter removal; they are not case-folded or resolved during Markdown parse.

## Bracketed cite groups

```text
bracketed-group = "[" citation-item *( ";" citation-item ) "]"
citation-item = prefix [ "-" ] citation-key suffix
```

A group produces one `Cite` and one or more `Citation` items in source order.
Semicolons separate items and are not affix content. Prefix is the inline
content before that item's optional mode marker and key; suffix is the inline
content after its key and before the next item boundary. Both arrays may be
empty and may contain ordinary nested inline markup.

The referent mode is `normal`, except that `-` immediately before `@` selects
`suppressAuthor` and is excluded from visible affixes and key. For example,
`[-@smith04]` has one suppressed-author item.

The Markdown parser preserves the complete authored post-key sequence in
`suffix`; it does not split a locale-dependent locator field. In
`[see @doe99, pp. 33-35 and *passim*; @smith04, chap. 1]`, the first suffix is
`, pp. 33-35 and *passim*` and the second is `, chap. 1`. A CSL-aware consumer
may later classify locator terms.

## Author-in-text cites

An unbracketed `@key` produces a one-item `Cite` whose referent mode is
`authorInText`. `-@key` produces `suppressAuthor`, not author-in-text. An
immediately following bracketed tail belongs to the sole item's suffix:

```markdown
@smith04 says blah.
@smith04 [p. 33] says blah.
```

The second suffix contains `p. 33` without its structural square brackets. A
citation processor decides final punctuation and author/year layout.

## Locator controls

Locator recognition depends on CSL locale terms and heuristics, including
page, chapter, section, volume, `¶`, and `§`; an unlabelled locator defaults to
page in Pandoc's citation processor. The Markdown AST therefore retains the
whole suffix rather than guessing.

Curly braces in these suffix forms are zero-width citation-processing controls
and remain suffix text until a CSL-aware consumer interprets them:

```markdown
[@smith{ii, A, D-Z}, with a suffix]
[@smith, {pp. iv, vi-xi, (xv)-(xvii)} with suffix here]
[@smith{}, 99 years later]
```

Non-empty braces force their contents to be treated as a locator; empty braces
prevent following material from being classified as a locator. The braces are
not rendered after citation processing.

## Bracket and reference precedence

Citation recognition participates in the shared inline bracket algorithm. A
bracketed candidate immediately followed by a direct-link destination,
reference tail, or enabled bracketed-span attribute list belongs to that outer
construct and is not a citation group. Without such a tail, a complete
bracketed cite takes precedence over shortcut-reference lookup. A failed cite
releases its opener to ordinary bracket parsing.

Inline code, comment bodies, and bytes owned by an HTML token are opaque. A
semicolon inside an opaque child is not an item separator. Paired inline HTML
tags do not suppress citation recognition in intervening text. An escaped `@`
or an `@` continuing an email/autolink token is not a cite opener.

When `example_lists` is also enabled, a label registered by a numbered example
is an example referent, not a bibliography key, even if its definition occurs
later in the document. This resolution occurs after the complete document has
collected example labels; it prevents parser order from changing `(@label)`
between citation and example semantics.

## Scopes and option behavior

A bracketed `Cite.scope` covers its outer brackets and complete contents. Each
item scope covers its contiguous prefix, mode marker, key, and suffix, excluding
the group's outer brackets and adjacent semicolon. Affix child scopes cover
only visible authored content.

An unbracketed group's and sole item's scopes cover the optional mode marker,
key, and optional bracketed suffix. The group has no invisible scope beyond
the citation spelling.

With `citations=false`, Pandoc citation syntax has no special meaning; footnote
cites remain controlled by their own extension. Recognition is linear in
source plus AST output and performs no bibliography lookup or CSL rendering.

## Citation-position reset

The exact heading class `reset-citation-positions` instructs a downstream
citation processor to reset position-sensitive style state at a top-level
heading. A nested heading does not reset it. Heading attribute syntax belongs
to the [Pandoc attribute contract](attributes.md); the citation parser merely
defines this class's processor-facing meaning.

## Required conformance cases

Tests must cover single and multiple items; prefixes, formatted suffixes, and
semicolon boundaries; every key punctuation character; braced URLs/nesting;
terminal and repeated punctuation; all three `BibMode` values;
author-in-text tails; implicit, explicit, forced, and suppressed locators;
reference-link/bracketed-span precedence; example-label conflicts before and
after definitions; malformed groups/keys; escaped openers, email addresses,
code, comments, and HTML; exact group/item/affix scopes; option-off behavior;
top-level/nested reset headings; allocation failure; and adversarial runs of
`@`, punctuation, braces, brackets, and semicolons.
