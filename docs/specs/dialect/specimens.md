# Specimens (numbered examples)

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Start an example definition with `(@label)` and whitespace. Refer to its label
elsewhere in the document.

```markdown
As (@sample) shows, the label can be used before its definition.

(@sample) A numbered example.
```

The definition becomes a `Specimen` in `Document.specimens`, with ID `sample`
and block content. It leaves no implicit call in ordinary document content.
The reference becomes a `Cite` with a specimen referent. The parser stores
identity and authored resets; the application derives and displays numbers.

## Anonymous definitions and resets

```markdown
(5@) Start this group at five.
(@) The next example.

An intervening paragraph.

(@later) Continue the document's sequence.
```

The first definition has `start=5` and no ID. The next has no explicit start
or ID; the last has ID `later`. Numbering these in order yields 5, 6, and 7.
A document counter begins at one and advances for every definition, including
anonymous, duplicate, and unreferenced definitions.

An explicit reset is effective only on the first definition of a source group.
Adjacent definitions form a group; blank lines alone do not split it. An
intervening block outside the preceding body starts a new group. Later resets
within the same group are ignored and stored as null. Reset numbers have one
to nine digits and must be positive.

## Bodies and labels

Continuation content starts four expanded columns after the enclosing
container's start, independent of marker width. Bodies use ordinary block
parsing and can contain multiple blocks. Definition markers never interrupt a
paragraph; they are exempt from nested ordered-list start restrictions.

Labels are runs of Unicode letters/numbers, optionally joined by single `_`
or `-` separators. They retain case. Duplicate IDs retain all definitions, but
references select the first. Specimen and footnote IDs are separate families.

## Reference forms

```markdown
(@example) A definition.

Use (@example) or @example. Keep [@example] for the bibliography.
```

The parenthesized reference and bare key resolve to the specimen. A bracketed
citation or a bare key with a bibliography tail remains a bibliography reference.
Resolution sees the entire document, including definitions in other containers.
An unknown `(@label)` retains its parentheses while the inner key can become an
author-in-text bibliography citation.

Definitions are retained in source order across all containers. Walks visit
them after footnotes. References store IDs, never copied bodies or derived
numbers, and one citation group never mixes referent families.
