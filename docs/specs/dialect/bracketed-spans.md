# Bracketed spans

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Put text in square brackets and attach an attribute container immediately
after the closing bracket.

```markdown
A [small **notice**]{#notice .muted role=note} here.
```

The result contains a `Span` with an anchor of `notice`, a `muted` class, and a
`role="note"` record. Its body contains text and strong emphasis. The brackets
and attributes are excluded from visible content and included in its scope.

Spans group inline content without assigning a built-in visual style. Use the
shared [attribute grammar](attributes.md) for IDs, classes, and records.

## Empty spans and nested content

```markdown
[]{} [visit [the guide](/guide)]{.reference}
```

The first span has empty content and empty attributes. The second contains a
link. The closing bracket must balance the span's opening bracket; ordinary
bracket nesting and escapes apply.

## Links and spans

A valid direct link or resolving full/collapsed reference takes precedence:

```markdown
[text](/target){.external}

[text]{.label}

[text]: /target
```

The first occurrence is a `Link` with a class. The second is a `Span` even
though `text` has a reference definition: the span rule precedes shortcut
references. A valid citation group or footnote-like body can likewise become
a span when an attribute container follows it.

Whitespace before `{`, malformed attributes, or an unclosed container prevent
span recognition. Other bracket alternatives can still apply. `![text]{.label}`
leaves a literal `!` before a span when no image tail succeeds. A complete
[cross link](cross-links.md) ends at `]]`; a following container does not make
it a span.
