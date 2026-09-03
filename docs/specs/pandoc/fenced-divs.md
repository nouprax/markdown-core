# Pandoc fenced divs

Status: normative target module for `fenced_divs`. Authority:
[Pandoc User's Guide — fenced divs](https://pandoc.org/MANUAL.html#extension-fenced_divs)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

## Syntax and AST

An opening fence is a line containing at least three consecutive `:`, followed
by an attribute-bearing opener defined by the
[Pandoc attribute contract](attributes.md). Optional spaces and another colon
run may follow before the line ending:

```markdown
::: {.warning}
This is a warning.
:::
```

```text
Div(
  closed: Bool,
  content: [Markup],
  scope: Scope
)
```

An explicit `{}` is sufficient to distinguish an opening fence even though its
universal attributes array is empty. Content is arbitrary block content.
`scope` covers both fences when closed and otherwise reaches the last consumed
content.

## Closing and nesting

A closing fence is a line containing at least three consecutive colons and no
attributes. Its length need not equal or exceed the opening length. Because an
opening fence must carry attributes, an attribute-free line always closes the
innermost open `Div`. Different fence lengths are visual guidance only.

Fenced divs may nest without a separate algorithm. Each opener increments the
normal block-container stack; the next eligible closer finalizes its current
owner. A missing closer produces `closed=false` and consumes to the containing
block boundary or end of document, matching Pandoc's recoverable unclosed-div
behavior.

The fenced div should be separated from preceding and following blocks by blank
lines. A colon line inside fenced code, another opaque block, or raw HTML is not
a div fence.

## Fallback and complexity

A colon run with no attributes is never an opener. An invalid braced list or
trailing non-space/non-colon bytes prevents opening and leaves the line to
ordinary block parsing. After a valid opener commits, a malformed inner opener
is content and cannot close its parent accidentally.

The block container stack provides linear nesting and closing. Recognition may
not scan ahead for a matching fence before parsing contents. Allocation failure
unwinds the incomplete container rather than emitting partial content.

## Required conformance cases

Tests must cover braced, empty, and unbraced-class openers; three and longer
fences; optional trailing colons; mismatched closing lengths; nesting; empty
content; missing closers and `closed`; blank-line boundaries; malformed
attributes; code/HTML opacity; block content of every major kind; exact scopes;
option-off behavior; nesting limits; allocation failure; and long colon runs.
