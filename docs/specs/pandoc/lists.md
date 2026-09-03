# Pandoc ordered and example lists

Status: normative target module for `fancy_lists`, `startnum`, and
`example_lists`. Authority: the Pandoc User's Guide sections for
[fancy lists](https://pandoc.org/MANUAL.html#extension-fancy_lists),
[starting numbers](https://pandoc.org/MANUAL.html#extension-startnum), and
[numbered example lists](https://pandoc.org/MANUAL.html#extension-example_lists)
at the snapshot pinned by the [Pandoc extension index](../pandoc.md).

## Ordered-list model

```text
OrderedListStyle =
  default | decimal | lowerAlpha | upperAlpha |
  lowerRoman | upperRoman | example

OrderedListDelimiter = default | period | oneParen | twoParens

ExampleReference(
  label: String,
  scope: Scope
)

ListItem(
  exampleLabel: String?,
  ...
)

List(
  flavor: bullet | ordered,
  start: Int?,
  style: OrderedListStyle?,
  delimiter: OrderedListDelimiter?,
  tight: Bool,
  items: [ListItem],
  scope: Scope
)
```

For a bullet list, `start`, `style`, and `delimiter` are null. For an ordered
list all three are non-null and `start >= 1`. Marker style and delimiter are
consumer-visible authored choices and are not reconstructed from `start`.

## Fancy markers

With `fancy_lists=true`, ordered markers may contain:

- one or more decimal digits;
- one ASCII lowercase or uppercase letter;
- a lowercase or uppercase Roman numeral;
- `#`, selecting `style=default`; or
- `@` under `example_lists`.

The marker may be followed by `.`, by `)`, or enclosed in `(...)`, producing
`period`, `oneParen`, or `twoParens`. `#.` produces both default style and
default delimiter. Same-line item content requires at least one separating
space; the marker may instead end the line. A single capital letter followed
by `.` and same-line content requires at least two spaces, preventing initials
such as `B. Russell` from becoming accidental lists.

The single markers `i` and `I` mean Roman one; other single letters are
alphabetic. Multi-character markers are Roman only when the complete sequence
is a valid numeral. The numeric value of an alphabetic marker is its one-based
ASCII letter position.

Every item in one list must use the same style and delimiter. `#` may continue
any ordered-list style. A style or delimiter change starts a new list. A nested
ordered list must begin with one or its equivalent (`1`, `a`, `i`); this avoids
accidental sublists from dates and parenthetical text.

With `fancy_lists=false`, only the inherited decimal-period form is added by
the base list grammar.

## Starting numbers

With `startnum=true`, `List.start` is the numeric value of the first marker.
Later authored marker numbers do not create per-item state; list numbering
continues from the start. With the option disabled, an otherwise recognized
ordinary ordered list stores `start=1` while retaining its style and delimiter.

Example lists have their own global assignment and always retain the computed
start regardless of `startnum`.

## Example lists

With `example_lists=true`, `(@)` is an ordered marker with `style=example` and
`delimiter=twoParens`. New example occurrences receive monotonically
increasing document-wide numbers, continuing across separated lists:

```markdown
(@) First example.
(@) Second example.

Intervening text.

(@) Third example.
```

`(@label)` assigns the current number to a label consisting of one or more
Unicode alphanumeric, `_`, or `-` characters. The owning `ListItem` stores that
label in `exampleLabel: String?`; it is non-null only for an item in a list with
`style=example`. No item stores its derived example ordinal.

Elsewhere, `(@label)` resolves to `ExampleReference(label)`. Its scope covers
the complete parentheses and marker. A renderer derives the displayed ordinal
by resolving the first item with that label and using its owning list's start
plus item position. The AST retains the referent instead of replacing it with
Pandoc's early-rendered number text; otherwise a consumer could not update or
analyze example references. An unresolved reference remains literal.

A repeated label reuses its first number and does not advance the global
counter. Pandoc only guarantees useful repeated-item numbering when that item
forms a list by itself; a repeated labelled item therefore starts a new
single-item example list at the first number. A repeated marker inside a larger
contiguous list follows ordinary sequential presentation from that list's
`start`.

Pandoc 3.11 also permits `(N@)` and `(N@label)` on the first item of an example
list. This resets the next global number to positive decimal `N`; later markers
in the same contiguous list do not reset it. Every example-list continuation
block is indented four spaces regardless of marker length.

When bibliography citations are enabled, a collected example label wins over
the same `@key` citation candidate even when the example occurs later. This
document-wide resolution rule is defined jointly with
[Pandoc citations](citations.md).

## Fallback and complexity

Invalid Roman numerals, missing marker whitespace, prohibited nested starts,
or incomplete parentheses remain ordinary text. A valid first item commits the
list style/delimiter; a mismatching later marker ends that list and remains
available to start another block.

Example registration and lookup use a document map and remain linear in source
plus output. The map is parser state rather than a second public side table;
the retained `ListItem.exampleLabel` edges are sufficient for consumer
resolution. Counter overflow is a parse error rather than wraparound. No
binding independently reconstructs list style or example state.

## Required conformance cases

Tests must cover decimal, lower/upper alpha, lower/upper Roman, `#.`, all three
delimiters, `i`/`I` ambiguity, capital-period spacing, style/delimiter changes,
nested-start restrictions, `startnum` on/off, ignored later numbers, tight and
loose items, global examples across lists, labels/references, unresolved and
repeated labels, `(N@)` resets, citation conflicts before/after definitions,
four-space continuations, exact scopes, independent options, overflow,
allocation failure, and long numeral/label/list inputs.
