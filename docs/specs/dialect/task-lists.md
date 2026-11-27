# Task lists

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Start a list item's opening content with a bracketed marker followed by a space.

```markdown
- [ ] Write the draft
- [x] Review the draft
- [?] Ask about the deadline
- [-] Drop the appendix
```

Each line is a `ListItem` with a `status`, taken from the character inside the
brackets:

| Marker | `status` |
| --- | --- |
| `[ ]` | `open` |
| `[x]`, `[X]` | `done` |
| `[?]` | `question` |
| `[-]` | `cancelled` |

An ordinary item's `status` is null. The marker and following separator are
removed from the item's content. Any other character between the brackets,
such as `[!]` or `[✓]`, is not a task marker: the line stays ordinary text.

## Nesting

```markdown
1. [?] Needs a decision
2. [x] Ready to ship
   - [ ] Follow up
```

Task prefixes work in ordered and nested lists.

## Required separator

A space or tab must follow `]`; vertical tab, form feed, and a line ending are
not separators. These are ordinary list-item text, not tasks:

```markdown
- [x]
- [x]missing separator
- [] empty marker
- [ab] two characters
- [!] not a task marker
```

Only the opening line is checked, at the first non-space position after list
padding. A later `[x]` is ordinary content. The prefix is removed before the
first block is chosen, so the body can begin with a heading or other block.

An item whose task prefix leaves no content has no paragraph to accept lazy
continuation; an unindented following line belongs outside that item.
See [lists](lists.md) for indentation and [block identifiers](block-identifiers.md)
for attaching an anchor to a task item.
