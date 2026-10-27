# Context query corpus

`queries.json` is what the C context, spelling, path, field and marker
queries answer for every example of the context fixture,
`packages/markdown-core/tests/context/context.txt`, in both text units, asked
of the document of a finalized session of the example's source, as the
language engine holds one per version of a text. It is a reviewed record of
those answers: a change to any query shows in its diff. The queries are C's
alone; the platform bindings expose the language engine, not the queries
(docs/plans/2026-10-23-lsp-completion-engine.md, R6).

The C runner writes the file and the CTest `elements_queries` fails when it is
stale, so the corpus is always the C queries' own answer:

```sh
build/cmake/packages/markdown-core/tests/queries_runner --emit \
    packages/markdown-core/tests/context/context.txt specs/context/queries.json
```

Each case holds its fixture line, its source without the cursor mark, and for
each unit, `utf8` and `utf16`, the cursor as `[line, column]` in that unit
and the expected rendering, one line per entry:

```text
block KIND START..END
inline KIND START..END
slot SLOT OWNER open|closed before=START..END after=START..END [prefix="TEXT"]...
spell INDEX "VALUE" "TEXT" CURSOR
spell INDEX "VALUE" none
path ID...
node ID [SLOT=SCOPES]... [none=SCOPES] [markers=SCOPES]
```

- `block` and `inline` are the context's openers, with the canonical AST's
  kind name, `None` for an inline opener whose kind is still open.
- `slot` is each slot, with its kind's name in the contract and its owner's
  kind name; its prefixes follow in order.
- `spell` is the spelling of each of the corpus's `values` in the slot with
  that index: the insertion's text and its caret, in columns of the unit, or
  `none` when the slot holds the value in no spelling.
- `path` is the identifiers of the nodes that hold the byte at the cursor,
  root first; none when the cursor names no byte.
- `node` is every node of the document in walk order by identifier, with the
  scopes of its fields of each slot kind that has any, in the contract's slot
  order, then of its fields no slot kind names (`none`), then of its
  markers, each list joined by `,`.

A scope is `LINE:COLUMN..LINE:COLUMN` as the queries answer it. Strings are
written as they are, between quotes.
