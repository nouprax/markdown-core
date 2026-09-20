# Attributes

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Attach a braced container to supported syntax to give it an ID, classes, or
key/value metadata.

```markdown
[Read this]{#notice .important role=note}
```

The span has anchor `notice`, class `important`, and a `role="note"` record.
Attributes are inert values: the parser does not apply CSS, execute records,
validate units, or choose a layout.

## Write attribute members

| Member | Meaning |
| --- | --- |
| `#name` or `id=name` | An anchor candidate; the last ID wins. |
| `.name` | Append one class. |
| `class="one two"` | Split the value into classes. |
| `key=value` | Append an ordered name/value record. |
| `key` | A bare name: the record `key="true"`. |
| `-` | Standing alone, append the class `unnumbered`. |
| `{}` | An empty attribute container. |

Names are taken as written, as in an HTML start tag. A name is one or more
bytes other than space, tab, a line ending, `=`, `}`, `"`, `'`, or `{`;
nothing is classified by Unicode category, so `{中文=值}`, `{1a=b}`,
`{_k=v}` and `{a#b=c}` are records named exactly as spelled. A member's first
byte selects its shape: `#` and `.` begin the identifier and class shorthands,
whose names follow the same rule and must be nonempty; a `-` followed by a
separator or the closing brace is the `unnumbered` class; anything else begins
a name. A name followed by `=` is an assignment, and a name followed by a
separator or the closing brace is a bare attribute, recorded as
`name="true"`. Names and values are case-sensitive. The exact lowercase `id`
and `class` names are special, in a bare form too: `{id}` is the anchor `true`
and `{class}` the class `true`; other assignments are records.

```markdown
`value`{#first #last .one class="two one" k=1 k=2 disabled}
```

The code's anchor is `last`, its classes are `one`, `two`, `one`, and its
records retain `k="1"`, `k="2"` and `disabled="true"` in order. Classes and
records are not deduplicated. An empty final `id=` clears the anchor
candidate. Because `#` and `.` are ordinary name bytes after a member's first
byte, `{#a#b}` is the single anchor `a#b` and `{.one.two}` the single class
`one.two`; a second anchor needs a separator, as in `{#a #b}`.

## Values and spacing

Values may be unquoted, single-quoted, or double-quoted, and may be empty.
Quoted values decode ASCII punctuation escapes and character references;
unquoted values decode escapes but retain character references literally.
Permitted line endings inside quoted values become one space.

```markdown
`x`{quoted="a&amp;b" raw=a&amp;b empty=}
```

The records contain `a&b`, `a&amp;b`, and the empty string, respectively.
An unmatched opening quote is treated as part of an unquoted value if the
remaining container is valid. Unquoted values end at space, tab, line ending,
or unescaped `}`; quotes, `<`, `=`, `>`, and backticks can otherwise be content.

Members are separated by spaces/tabs and at most one line ending between
members; independently delimited members can be adjacent. A blank line is not
allowed. Only the owning syntax can grant a multiline extent. Non-ASCII
whitespace does not separate members, although `class=` splits its decoded
value on ASCII whitespace and Unicode Zs. Identifier and class shorthands do
not decode escapes or character references.

## Attachment sites

| Owner | Position |
| --- | --- |
| Inline code | Immediately after the closing backtick run. |
| Heading | At the end of the final heading-content line. |
| Fenced code | At the end of the opening fence's info region. |
| Direct link/image, resolving full/collapsed reference, angle autolink | Immediately after the complete occurrence. |
| Link reference definition | After its destination and optional title. |
| Bracketed span | Immediately after its closing bracket. |
| Named directive | Immediately after its name or label. |
| Nameless container | On its opening colon fence. |

Every AST node has an `attributes` value, but only supported attachment rules
populate it. Other nodes have empty classes and records. A missing container,
`{}`, and a container holding only an ID have the same empty attributes value.
Tables, rows, cells, and captions do not accept attributes; a table can receive
a [block identifier](block-identifiers.md).

### Inline code

```markdown
`printf()`{.c} and `other` {.c}
```

Only the first code span receives a class. A space before the container
prevents attachment. A class on code does not derive a language field.

### Headings

```markdown
## Chapter ## {#chapter .compact}

Underlined {#underlined}
=======================
```

Both headings receive explicit anchors. The valid suffix and preceding
whitespace are removed before applying the ordinary closing-hash rule. For
Setext headings the suffix belongs to the last content line. `# {#empty}` is
an empty heading with an explicit anchor.

### Fenced code

````markdown
```python {.numberLines startFrom="10"}
print("hello")
```
````

The code block has info/language `python`, class `numberLines`, and a
`startFrom="10"` record. No line numbers are generated by parsing. An invalid
suffix or one followed by other text remains ordinary info-string content;
the original fence rule must still accept the complete opening line.

### Links, images, and inherited attributes

```markdown
[guide][r]{#local .new k=2}

[r]: /guide {#shared .base k=1}
```

The link receives anchor `local`, classes `base`, `new`, and records `k="1"`,
`k="2"`. Definition classes/records come first, followed by occurrence values.
A non-null occurrence anchor wins; otherwise the definition's anchor is used.
Nothing is deduplicated and the occurrence retains its own source range.

A definition's container can follow spaces/tabs and at most one line ending;
only whitespace may follow it on its closing line. Invalid syntax falls back
to the ordinary definition grammar. An unresolved reference inherits nothing.

An attribute container after a shortcut-looking bracket makes a
[span](bracketed-spans.md), since spans have earlier precedence. Bare automatic
links accept no container. Image width/height records are independent of typed
[dimensions](links-and-images.md#image-dimensions).

## Invalid containers

An invalid member, missing closer, or invalid placement attaches no partial
attributes. The failed source resumes ordinary parsing. Once an owner such as
inline code or a directive name is complete, its valid portion remains intact.
Attached syntax belongs to the owner's scope, never to visible content.
