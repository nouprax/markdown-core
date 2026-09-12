# Task lists

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Start a list item's opening content with a bracketed marker followed by a space.

```markdown
- [ ] Write the draft
- [x] Review the draft
- [✓] Publish it
```

Each line is a `ListItem`. Its `marker` stores exactly the scalar inside the
brackets: a space, `x`, or `✓`. The marker and following separator are removed
from the item's content. A space means incomplete; any other marker is complete
for the completion convenience value. Applications can give custom markers
more specific meanings without losing the authored character.

## Custom markers and nesting

```markdown
1. [?] Needs a decision
2. [🚀] Ready to ship
   - [ ] Follow up
```

Task prefixes work in ordered and nested lists. A marker must be exactly one
Unicode scalar. Combining sequences and multi-scalar emoji do not qualify,
even when they appear as one displayed character.

## Required separator

A space, tab, vertical tab, or form feed must follow `]`. A line ending is not
a separator. These are ordinary list-item text, not tasks:

```markdown
- [x]
- [x]missing separator
- [] empty marker
- [ab] two characters
```

Only the opening line is checked, at the first non-space position after list
padding. A later `[x]` is ordinary content. The prefix is removed before the
first block is chosen, so the body can begin with a heading or other block.

An item whose task prefix leaves no content has no paragraph to accept lazy
continuation; an unindented following line belongs outside that item.
See [lists](lists.md) for indentation and [block identifiers](block-identifiers.md)
for attaching an anchor to a task item.
