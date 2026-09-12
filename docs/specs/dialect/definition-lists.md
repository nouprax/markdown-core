# Definition lists

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Write a term on one line and begin its definition with `:` followed by a space.

```markdown
Markdown
: A lightweight markup language.
```

The result is a `DefinitionList` containing one `Definition`. Its term is
inline text, its body contains a paragraph, and `compact=true` because no blank
line separates the term from its first body. A `~` marker can replace `:`.

## Multiple definitions and terms

```markdown
Guide
: A person who shows the way.
: A document that explains a subject.

Reference
: Material you consult as needed.
```

The first term owns two distinct body collections; the second owns one.
A blank line is required before a new term. Without it, an apparent next term
can be lazy paragraph continuation within the previous body.

Insert one blank line between a term and its first body for `compact=false`:

```markdown
*Term* with `code`

: A definition with formatted term text.
```

The term contains emphasis and code. Its first gap determines compactness for
all bodies of that term, regardless of later gaps.

## Block content

Indent continuation blocks to the body content column:

```markdown
Term
: First paragraph.

  Second paragraph.

  - An item within the definition
```

All three blocks belong to one body. Bodies can contain lists, quotes, tables,
code, and nested definition lists. Same-depth definition markers start another
body for the term; an indented marker can belong to a nested definition.

## Recognition boundaries

A term is a single nonblank line that could otherwise start a paragraph, with
no more than three leading columns. A heading, reference definition, footnote
definition, or other earlier block start cannot become the term. At most one
blank line may precede the first body. Terms trim surrounding whitespace;
trailing spaces or a backslash do not create a hard break in the term.

A marker allows zero to three leading columns and requires either a line ending
or at least one padding column. One through four padding columns are consumed
under the inherited list rules; extra indentation can produce code. A marker-only
line gets its content from continuation lines.

A complete table candidate, including an eligible caption, takes precedence.
Definition lists do not interrupt an existing multiline paragraph. An invalid
prefix leaves normal Markdown parsing active: `:word` without marker spacing
can still be an inline [directive](directives.md).

Scopes include the term, markers, and bodies through the last nonblank body
line. Each body remains a separate ordered collection; parsing never flattens
the distinction between several definitions of one term.
