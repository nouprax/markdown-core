# Lists

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use `-`, `+`, or `*` followed by whitespace to start a bullet item.

```markdown
- Apples
- Pears
  - Conference pears
```

The outer `List` contains two `ListItem` nodes. The second item contains a
nested list. Bullet lists have no start number, ordered variant, or delimiter
value. Changing the bullet character starts a separate list.

Indent continuation content to the item's content column. Marker padding and
tabs follow CommonMark's four-column tab stops. A blank line can separate
paragraphs or blocks within an item when their indentation keeps them inside it.

## Numbered lists

```markdown
3. First item
4. Next item

1) Another list
```

The first ordered list has `start=3`, decimal variant, and a period delimiter.
The second uses a one-sided parenthesis delimiter. Only the first marker sets
the list's start; later authored numbers do not reset it.

Decimal markers allow one to nine digits, including zero. A missing separator
before same-line content or a ten-digit run is not a marker. A marker at the
end of its line can open an empty item.

## Alphabetic and Roman markers

```markdown
a. First
b. Second
```

```markdown
(i) First Roman item
(ii) Second Roman item
```

```markdown
A) Capital-letter item
```

These are ordered lists too. The AST retains whether the variant is alphabetic
or Roman, its case, and whether parentheses are closed on both sides. In a new list with no established variant, a single
`i` or `I` starts Roman one. After a list establishes its variant, subsequent
markers are read in that variant first; an incompatible variant or delimiter
ends it and can start another list. An open alphabetic list can therefore read
`i` as letter nine even after a blank line; a blank alone does not reset the
list context. The examples above are independent inputs.

Alphabetic markers are single ASCII letters. Roman numerals use one case and
the accepted Roman component grammar, with value at most 999,999,999. In grammar
notation that component order is `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX]
[V] [IV] I*`, with at least one character. Invalid or oversized numerals are
ordinary text.

A capital letter followed by `.` and same-line content needs at least two
columns of whitespace: `B. Russell` is text, while `B.  Russell` starts a list.

## Automatic markers

```markdown
#. First
#. Second
```

The list stores `start=1`, default variant, and default delimiter. `#)` and
`(#)` are also accepted, with their respective parenthesis delimiters.
A `#` marker can continue an established ordered variant.

## Blank lines and starts

`tight` describes whether the list is compact under the inherited blank-line
rules. Loose items retain ordinary paragraphs; no separate compact paragraph
kind is introduced. Nested lists are evaluated independently.

Only a marker of value one can interrupt an existing paragraph. An ordered
list newly opened inside a list item or definition body must also start at
one (`1`, `a`, `A`, `i`, `I`, or `#`); other starts remain paragraph text.
[Specimens](specimens.md) follow their own definition rules instead.

Use [task prefixes](task-lists.md) for checkboxes and custom item markers.
