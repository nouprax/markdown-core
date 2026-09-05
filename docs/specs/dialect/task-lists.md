# Task lists

Status: normative module of the [Markdown Core dialect](../dialect.md).
Options: `taskLists` (default `true`) and `taskMarkers` (default `false`,
effective only while `taskLists` is on). Sources: cmark-gfm's task-list
extension; Obsidian's custom task characters. Executable oracles: cmark-gfm
for the inherited markers, `@quartz-community/remark-obsidian` for custom
markers. Landing: the `marker` field with `M5`, custom markers with `O5`;
until `M5` the current contract's `checked: Bool?` stands.

## Model

```text
ListItem(marker: String?, exampleLabel: String?, content: [Markup])
```

- `marker == null` means the item is not a task.
- `marker == " "` means an incomplete task.
- Every other marker is a completed or custom-state task; the exact scalar is
  preserved, `x` and `X` included, and the parser assigns no meaning to it.

Bindings may expose the derived conveniences `isTask = marker != null` and
`isComplete = marker != null && marker != " "`. No native node, wire payload,
or binding stores a checked boolean beside `marker`.

## Syntax

```text
task-prefix    = "[" task-marker "]" 1*task-separator
task-separator = SP / TAB / VT / FF
```

The prefix is recognized at the first non-space byte of a list item's first
line, after the inherited list marker and its padding, before the item's first
block is decided; the remainder of the line begins that block. It applies to
every list item the inherited list algorithm accepts, bullet or ordered, at
any nesting depth. The separator is structural and is removed with the
prefix. A prefix at the end of its line, with no separator, is not a task
prefix; the line ending is not a separator.

With `taskMarkers=false`, `task-marker` is exactly one of a space, `x`, and
`X`. With `taskMarkers=true`, `task-marker` is exactly one Unicode scalar of
any value; `[]` and `[ab]` are not task prefixes. The scanner decodes at most
the candidate marker before rejecting a malformed prefix.

## Option behavior and fallback

With `taskLists=false`, no task prefix is recognized whatever `taskMarkers`
says, and the brackets are inline text. With `taskLists=true` and
`taskMarkers=false`, the inherited rule stands byte for byte. A malformed
prefix is inline text of the item's first block. Inline code and other opaque
constructs cannot affect recognition, because the prefix is decided before
inline parsing.

## Scopes

`ListItem.scope` covers the prefix; the item's first block and its
descendants begin after the separator.

## Required conformance cases

| Input prefix | `marker` | Derived completion |
| ------------ | -------- | ------------------ |
| none         | `null`   | not a task         |
| `[ ] `       | `" "`    | incomplete         |
| `[x] `       | `"x"`    | complete           |
| `[X] `       | `"X"`    | complete           |
| `[?] `       | `"?"`    | custom, complete   |
| `[-] `       | `"-"`    | custom, complete   |
| `[✓] `       | `"✓"`    | custom, complete   |

Tests also cover ordered and nested lists, every separator, a prefix at the
end of its line, empty and multi-scalar markers, a later `[x]` in the item
text, both options in every combination, scopes, wire round-trips on every
binding, allocation failure, and long malformed bracket runs.
