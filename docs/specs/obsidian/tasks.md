# OFM task lists

Status: normative target module. Authority:
[Basic formatting syntax — Task lists](https://obsidian.md/help/syntax#Task%20lists)
at the snapshot pinned by the
[OFM contract index](../obsidian-flavored-markdown.md). GFM owns base list and
task-prefix placement; this module generalizes the marker value without adding
another list algorithm.

## Recognition grammar

```text
task-prefix = "[" task-marker "]" 1*task-separator
task-marker = exactly one Unicode scalar value
task-separator = ASCII space / ASCII tab / source line ending
```

The prefix is recognized only at the start of a `ListItem`'s first paragraph,
after inherited list-marker/padding processing. It applies to any list item
accepted by the inherited GFM algorithm. A marker containing zero or multiple
Unicode scalar values is not a task prefix. The separator is structural and is
removed with the prefix; remaining item content follows ordinary paragraph
parsing.

## AST and derived state

The Tasks module contributes exactly one field to `ListItem`:

```text
marker: String?
```

The invariants are:

- `marker == null` means the item is not a task.
- `marker == " "` means an incomplete task.
- Every other marker means a completed/custom-state task.
- The exact Unicode scalar is preserved, including `x` versus `X`; the parser
  does not normalize plugin-defined state meanings.
- The task prefix is absent from visible paragraph content, while the
  `ListItem` scope still covers it.

`content`, `scope`, and block-identifier behavior are owned by the base
`ListItem` and the separate block-identifiers spec. They are not task-list
fields and are not redefined here.

Bindings may derive `isTask` and `checked`/`isComplete` convenience properties:

```text
isTask     = marker != null
isComplete = marker != null && marker != " "
```

No native node, wire payload, or language binding may store both
`marker` and an independent checked boolean. The canonical vNext model
replaces the current stored `checked: Bool?` field.

## Fallback and interactions

`[]`, `[ab]`, `[x]text` without a separator, or a task-looking sequence outside
the first paragraph position remains inline text. Inline code and other opaque
constructs do not affect recognition because the prefix decision occurs before
inline parsing. Nested task items are recognized independently by the same
list-item algorithm.

The scanner decodes at most the candidate marker and must not allocate or scan
the full item before rejecting a malformed prefix. Invalid UTF-8 continues to
follow the repository's existing source-decoding contract.

## Required conformance cases

| Input prefix | `marker` | Derived completion |
| --- | --- | --- |
| no prefix | `null` | not a task |
| `[ ] ` | `" "` | incomplete |
| `[x] ` | `"x"` | complete |
| `[X] ` | `"X"` | complete |
| `[?] ` | `"?"` | complete/custom |
| `[-] ` | `"-"` | complete/custom |
| `[✓] ` | `"✓"` | complete/custom |

Tests must also cover ordered/bullet and nested lists, first-paragraph
placement, tabs/newlines as separators, empty/multiple-scalar markers, missing
separators, scopes, wire round-trip on every binding, allocation failure, and
long malformed bracket runs.
