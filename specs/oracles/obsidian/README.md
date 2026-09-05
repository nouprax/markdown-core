# Obsidian parser oracles

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
`docs/specs/dialect/` are the rule. This oracle is authoritative only for the
intersection it implements: wikilinks/embeds, highlights, comment removal, and
custom task characters. Tag and package-specific math syntax are disabled.
Callouts, block identifiers, inline-footnote recognition, the target
`Cite`/`Citation`/`CitationReferent`/`Footnote` consumer projection, and image
dimensions are absent from the direct parser comparison and therefore stay
under official-example product fixtures. Inherited HTML behavior remains owned
by the cmark oracle; the dialect adds no Obsidian-specific HTML suppression.

The Properties page is the source for beginning-of-file placement, the
three-hyphen fence form, the supported consumer domain, and the absence of
Markdown and nested Properties values; `docs/specs/dialect/properties.md`
states the rule. The harness therefore owns one exact,
line-oriented envelope scanner and passes only the bytes between a valid pair
of fences to the YAML oracle. A package-specific frontmatter recognizer is
neither an authority nor an intermediate normalization layer.

`yaml` parses one document with JSON scalar resolution plus a plain-string
fallback, duplicate checking disabled at composition time, and source tokens
enabled. The Properties projection then walks the ordered mapping pairs,
decodes each directly authored scalar key as text, checks uniqueness in that
decoded string namespace, retains number payloads from scalar source, resolves
aliases on the node graph, and accepts only the contract's scalar and
text/number-list domain. It never calls `toJS()` or materializes a root
JavaScript object. Empty, whitespace-only, and comment-only payloads all
produce a document with no content node and therefore the same non-null empty
metadata array; comments remain presentation bytes rather than records.

Package-only syntax never enlarges the target language. Parser errors,
unsupported tags or node kinds, duplicate decoded names, unresolved or cyclic
aliases, YAML stream/document indicators (including `...`), and unsupported
values make the tentative Properties candidate fail. Only the exact outer
`---` line terminates Properties.

The corpus contains inputs only. It deliberately has no Markdown Core expected
AST blocks; product goldens belong to the C fixture and shared canonical AST
corpus. `deltas.json` registers every current semantic gap and the parity gate
requires each gap's two semantic-tree digests to continue reproducing. A new or
changed divergence fails, and a gap that starts agreeing also fails until its
registry entry is removed in the same implementation change.

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
record to retain ordered, in-envelope CST range evidence. Its corpus covers
integer-looking keys in non-JavaScript order, exact large/decimal/exponent/
negative-zero number spellings, quoted key decoding, aliases, and strict
projection failures. Product fixtures remain authoritative for canonical
binding-coordinate scopes, allocation failure, and parser-wide resource
limits.

For successful Properties inputs, the normalized semantic root contains a
`metadata` field: `null` means absent, while an array (including an empty
array) contains ordered `{name, value}` records using the tagged scalar/list
shape from `docs/specs/dialect/properties.md`. The current implementation's missing
field is deliberately normalized to `null`, so every target gap remains
visible. When `Document.metadata` is implemented, its canonical debug field
must expose the same compact JSON value for this gate; that dump change lands
atomically with the public model and cross-binding fixtures.
