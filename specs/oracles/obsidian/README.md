# Obsidian parser oracles

Metadata status (2026-09-08): the [Properties module](../../../docs/specs/dialect/properties.md)
owns ten fixed fields, selected scalar/list/JSON forms, and bare literal `|`
for `abstract` and `comment`. Unknown names and unsupported input are ignored.
Metadata directly stores ordered scoped records. This is a user-defined format
borrowing Obsidian/Pandoc notation, not full compatibility with either application
or YAML. The pinned package remains comparison tooling for the valid intersection.

This oracle runs `@quartz-community/remark-obsidian@0.2.4` through the same
unified/remark parser family already used by the repository. The package is
exact-pinned in `package.json` and integrity-pinned in `pnpm-lock.yaml`.

The choice is usage-based and reproducible as a dated decision. npm's official
download API reported 43,766 downloads for the direct parser package during
2026-08-23 through 2026-08-29. Its full
`@quartz-community/obsidian-flavored-markdown` integration had 44,174 and
delegates Markdown recognition to this parser. The comparison candidates
`remark-obsidian`, `@thecae/remark-obsidian`, and `markdown-it-obsidian` had
135, 11, and 35 downloads respectively during the same window. The selected
release records git commit
`bb36c5db9f343dd82af2ffe47b5ec271a15c080d` in npm metadata.

Properties use a separate executable oracle because that Markdown package
does not recognize file metadata. The gate runs `yaml@2.9.0` through its
Document/node API with CST source tokens retained. The package is exact- and
integrity-pinned and its npm release records git commit
`ddb21b04cb889722cec8f89dc1b67f19d62d7f7d`. npm's official download API
reported 202,359,392 downloads for `yaml` during 2026-08-23 through 2026-08-29.
`js-yaml` had 298,229,346 downloads, but its public load API projects mappings
to JavaScript objects and thereby erases source order, key shape, colliding
scalar spellings, and numeric lexemes. Raw download rank cannot make an
implementation an oracle for information it does not expose. `yaml` is the
broadly used candidate whose public model can witness the required facts.

Popularity chooses an implementation oracle; it does not make that package the
specification. The official Obsidian help snapshot registered in `deltas.json`
is the source of the feature definitions, and the dialect modules under
`docs/specs/dialect/` are the rule. This oracle is evidence only for the
intersection it implements: wikilinks/embeds, highlights, comment removal, and
custom task characters; where it and a module differ, the module decides and
the difference is a registered delta. Tag and package-specific math syntax
are disabled.
Callouts, block identifiers, inline-footnote recognition, the target
`Cite`/`Citation`/`CitationReferent`/`Footnote` consumer projection, and image
dimensions are absent from the direct parser comparison and therefore stay
under official-example product fixtures. Inherited HTML behavior remains owned
by the cmark oracle; the dialect adds no Obsidian-specific HTML suppression.

The Properties module owns the exact leading `---` envelope and the bounded
member grammar. Only a complete first envelope attaches, even when every member
is ignored. The body is parsed after its closing fence; `...` does not close it.
The oracle scanner recognizes that envelope and submits its payload to the
pinned Document/CST parser. It checks only supported names and source forms:
single-line direct scalars, flat text/number lists, JSON roots, and bare literal
prose on the two designated fields. Source checks exclude anchors, aliases,
tags, nested values, general flow mappings, and folded/multiline scalars.

Oracle inputs outside that intersection fail the comparison precondition;
product fixtures separately verify whole-member skipping and valid-neighbor
recovery. The gate has no Obsidian runtime and does not claim which parser the
application uses. Pandoc requires valid YAML and interprets strings as Markdown;
this product's ignored-input rule and atomic text model are local decisions.
No general YAML dependency is added to the shared core or bindings.

The corpus contains inputs only. It deliberately has no Markdown Core expected
AST blocks; product goldens belong to the C fixture and shared canonical AST
corpus. `deltas.json` separates unfinished features in `baselineGaps` from
exact deliberate syntax differences in `expectedDivergences`. One comparison
checks both registries: each input must remain in the corpus and both semantic
digests must reproduce; agreement, changed digests, duplicate entries, or an
unregistered difference fail. O5 retires `custom-task-character`; its seven
remaining task witnesses record the oracle's UTF-16 marker matching, excluded
closing bracket, whitespace handling, paragraph-first recognition, decoded
escapes, and later-line recognition.

Set up and run the gate offline after dependency installation:

```sh
pnpm install --frozen-lockfile
pnpm build:c
pnpm check:obsidian-parity
```

The gate performs no network access. It verifies the installed package version,
runs oracle canaries before comparison, parses the same corpus with both
implementations, and compares a scope-free semantic tree. Scope correctness
remains owned by product fixtures because the two parsers use different
coordinate models. The Properties canaries additionally require every emitted
record to retain ordered, in-envelope CST range evidence. The corpus covers
all recognized names, exact large/decimal/exponent/negative-zero numeric text,
quoted name decoding, single strings and both list spellings, literal prose,
and empty metadata. Product fixtures own binding-coordinate scopes, ignored
syntax, malformed recovery, bounded record state and allocation failures.

For successful Properties inputs, the normalized semantic root contains a
`metadata` field: `null` means absent, while an array (including an empty
array) contains ordered `{name, value}` records using the tagged scalar/list
shape from `docs/specs/dialect/properties.md`. The gate reads the real Metadata
field from canonical dumps and compares its ordered records directly. There
are no retained-source cases to filter and no alias/tag expansion canaries.
