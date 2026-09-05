# Lists

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the ordered-list facts of every list and two options: `fancyLists` and
`exampleLists` (each default `false`). Source: Pandoc's `fancy_lists`,
`startnum`, and `example_lists`. Executable oracle: the Pandoc 3.11 CLI under
`specs/oracles/pandoc/`. Landing: the list facts with `M5`, fancy markers
with `P9a`, example lists with `P9b`.

## Model

```text
OrderedListStyle     = decimal | lowerAlpha | upperAlpha | lowerRoman |
                       upperRoman | example | default
OrderedListDelimiter = period | oneParen | twoParens | default

List(flavor: bullet | ordered, start: Int?, style: OrderedListStyle?,
     delimiter: OrderedListDelimiter?, tight: Bool, items: [ListItem])
ListItem(marker: String?, exampleLabel: String?, content: [Markup])
ExampleReference(label: String)
```

For a bullet list `start`, `style`, and `delimiter` are `null`. For an
ordered list all three are non-null and `start >= 0`. `start` is always the
numeric value of the first marker; there is no option that changes it. Style
and delimiter are authored facts and are never reconstructed from `start`.
`ExampleReference` is an inline leaf. `ListItem.marker` belongs to the
[task lists](task-lists.md) module.

## Inherited ordered lists

With `fancyLists=false`, the inherited markers `N.` and `N)` are the only
ordered markers: `N` is one to nine ASCII digits, `style` is `decimal`,
`delimiter` is `period` or `oneParen`, and `start` is the value of the first
marker, so `0.` starts at zero and a ten-digit run is not a marker. Every
inherited rule about padding, continuation, tightness, and which markers may
interrupt a paragraph is unchanged.

## Fancy markers

With `fancyLists=true`, an ordered marker is one of:

- one to nine decimal digits, `style=decimal`;
- one ASCII letter, `lowerAlpha` or `upperAlpha`, with value its one-based
  position in the alphabet; `i` and `I` alone are Roman one;
- a Roman numeral `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`
  of at least one character in one case, `lowerRoman` or `upperRoman`, with
  the usual value, the whole marker consumed; or
- `#`, `style=default`, with value 1.

The marker is followed by `.`, by `)`, or is enclosed in `(...)`, giving
`period`, `oneParen`, or `twoParens`; `#.` stores `delimiter=default`, while
`#)` and `(#)` store `oneParen` and `twoParens`. Padding follows the inherited
rule of one to four columns of spaces or tabs, or the marker ends the line. A
single capital letter followed by `.` and same-line content requires at least
two columns of whitespace after the `.`, so `B. Russell` is text; no exception
for `p.` exists.

After the first item commits a style and delimiter, each later marker is
read in that style first; `#` continues any style; a marker unreadable in the
committed style, or with another delimiter, ends the list and may start
another. With `fancyLists` on, an ordered list whose first item lies inside a
list item or a definition body must have value 1 (`1`, `a`, `A`, `i`, `I`,
or `#`), or the line is paragraph text; example lists are exempt. Only a
marker with value 1 may interrupt a paragraph, and example markers never do.

## Example lists

With `exampleLists=true`, `@` is a marker character only inside parentheses:
`(@)`, `(@label)`, `(N@)`, and `(N@label)`, with `style=example` and
`delimiter=twoParens`. Items receive monotonically increasing document-wide
numbers starting at 1, assigned in ascending order of item `scope.start`
across content and footnotes, continuing across separated lists:

```markdown
(@) First example.
(@) Second example.

Intervening text.

(@) Third example.
```

`label` is `alnum-run *( ("_" / "-") alnum-run )` over the dialect's letters
and numbers; the item stores it in `exampleLabel`, non-null only in an example
list. No item stores its derived number. A repeated label never splits a
list: the item is an ordinary item, `exampleLabel` records the label, and the
counter does not advance; a list whose first item repeats a label has `start`
equal to that label's first number. `N` is one to nine decimal digits with
value at least 1 (`(0@)` and longer runs are not markers): on the first item
of a list it sets the counter before that item is numbered, and on a later
item it is ignored. The continuation column of an example item is the
container start plus four columns after tab expansion, whatever the marker
width.

In inline content outside opaque constructs, the exact spelling `(@label)`
with no internal whitespace is an `ExampleReference` when the label is
registered anywhere in the document, before or after the occurrence; where it
is a valid list marker, the marker rule wins. With `citations` on, a bare
`@label` that is not followed by bracketed material and names a registered
label is also an `ExampleReference`, while `[@label]` and a bare key with a
bracketed tail are citations. An unregistered `(@label)` is `(` followed by
an author-in-text `Cite` and `)` with `citations` on, and text otherwise.
Registration and lookup are one document-wide operation, so parser order
never changes a result. A consumer derives the displayed number by resolving
the first item with that label and using its list's `start` plus the item's
position.

## Option behavior and fallback

With both options off, output is the inherited grammar's. Invalid numerals,
missing marker whitespace, prohibited nested starts, incomplete parentheses,
and ten-digit runs are ordinary text. Counters cannot overflow because
markers and `N` are limited to nine digits. The example map is parser state,
not a public side table.

## Scopes

`List` and `ListItem` scopes are the inherited ones; `ExampleReference.scope`
covers the parentheses and marker.

## Required conformance cases

Tests cover decimal, alphabetic, and Roman markers in both cases, `#`, all
delimiters, `i` and `I`, the capital-period rule, `0.`, nine- and ten-digit
markers, style and delimiter changes, committed-style reading, nested starts,
paragraph interruption, tight and loose items, global examples across lists
and footnotes, labels and references before and after definitions, unresolved
and repeated labels, resets on first and later items, `(0@)`, four-column
continuations, the bare-label rule with `citations` on and off, exact scopes,
each option independently, allocation failure, and long numeral, label, and
list inputs.
