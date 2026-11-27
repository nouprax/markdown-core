# Lists

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Use `-`, `+`, or `*` followed by whitespace to start a bullet item.

```markdown
- Apples
- Pears
  - Conference pears
```

The outer `List` contains two `ListItem` nodes. The second item contains a
nested list. Bullet lists have no start number or delimiter value. Changing the bullet character starts a separate list.

Indent continuation content to the item's content column. Marker padding and
tabs follow CommonMark's four-column tab stops. A blank line can separate
paragraphs or blocks within an item when their indentation keeps them inside it.

## Numbered lists

```markdown
3. First item
4. Next item

1) Another list
```

The first ordered list has `start=3` and a period delimiter. The second uses
a parenthesis delimiter. Only the first marker sets the list's start; later
authored numbers do not reset it.

Markers are one to nine decimal digits, including zero, followed by `.` or
`)`. A missing separator before same-line content or a ten-digit run is not a
marker. A marker at the end of its line can open an empty item.

Pandoc's other markers are text: letters (`a.`, `A)`), Roman numerals
(`iv.`, `(i)`), the automatic `#.`, and parentheses on both sides (`(1)`).

## Blank lines and starts

`tight` describes whether the list is compact under the inherited blank-line
rules. Loose items retain ordinary paragraphs; no separate compact paragraph
kind is introduced. Nested lists are evaluated independently.

Only a marker of value one can interrupt an existing paragraph. A list nested
in an item may start at any number, as in CommonMark.

Use [task prefixes](task-lists.md) for checkboxes and custom item markers.
