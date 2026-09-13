# Line breaks

[Syntax guide](../dialect.md) · [Documentation](../README.md)

A single source line ending inside a paragraph is a soft break:

```markdown
First line
second line
```

The paragraph contains `Text`, `SoftBreak`, and `Text`. The application decides
whether to display a soft break as a space or a line break.

## Hard breaks

End a line with a backslash to request a hard break:

```markdown
First line\
second line
```

This produces `LineBreak` between the two text nodes. Two or more spaces before
the line ending have the same effect. Use the backslash form when trailing
spaces would be difficult to see or preserve in an editor.

A hard break does not create another paragraph. A blank line does. A backslash
at the end of a block, without another inline line to follow, is ordinary text.

## Source positions

LF, CR, and CRLF are all accepted. `SoftBreak` and `LineBreak` report the
parser's editor coordinates for the authored break. A backslash hard break
includes its backslash; for a spaces-based hard break, trailing spaces remain
in the preceding text node's scope but not its literal. As with every
[scope](../canonical-ast.md#coordinates), these positions do not promise a
retrievable string slice, and the bindings do not convert them.
