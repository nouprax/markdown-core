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
does not recognize file metadata. The gate runs `gray-matter@4.0.3` with a
custom `js-yaml@4.1.0` engine configured with `JSON_SCHEMA`. Both packages are
exact- and integrity-pinned. npm's official download API reported 8,940,668
downloads for `gray-matter` and 298,229,346 for `js-yaml` in the week queried
for 2026-08-23 through 2026-08-29. The compared frontmatter alternatives
`remark-frontmatter@5.0.0` and `front-matter@4.0.2` had 5,162,817 and 4,381,501;
the former tokenizes fences but deliberately does not decode values. The
selected releases record commits
`e54a33b394e14a1808b88f939507f374552906e4` and
`2cef47bebf60da141b78b085f3dea3b5733dcc12` in npm metadata.

Popularity chooses an implementation oracle; it does not make that package the
OFM specification. The official Obsidian help snapshot registered in
`deltas.json` remains normative. This oracle is authoritative only for the
intersection it implements: wikilinks/embeds, highlights, comment removal, and
custom task characters. Tag and package-specific math syntax are disabled.
Callouts, block identifiers, inline-footnote recognition, the target
`Cite`/`Citation`/`CitationReferent`/`Footnote` consumer projection, and image
dimensions are absent from the direct parser comparison and therefore stay
under official-example product fixtures. Inherited HTML behavior remains owned
by the cmark oracle; this profile adds no Obsidian-specific HTML suppression.

The Properties page remains the authority for beginning-of-file placement,
the three-hyphen fence form, the supported consumer domain, and the absence of
Markdown and nested Properties values. `gray-matter` is more permissive for
some delimiter spellings, so the harness first applies the target contract's
malformed-envelope boundary and then requires `gray-matter` to agree on the
extracted body.
Package-only syntax never enlarges the target language. `js-yaml` supplies
YAML 1.2 JSON-schema value decoding for the valid intersection; the projection
then requires one top-level mapping with arbitrary non-empty names and scalar
or documented text/number-list values. Its JavaScript-object result has
already converted mapping keys to property strings, so it is evidence for the
decoded names in the successful corpus, not for the source shape of a YAML key.
The executable property-name intersection is restricted further to direct
scalar spellings whose JavaScript property string is identical to the target
textual name and which `js-yaml` does not merge with another authored name.
Numeric or null spellings such as `1.0`, `1e2`, `01`, `-0`, and `~` are outside
that intersection because the binding rewrites them to `"1"`, `"100"`, `"1"`,
`"0"`, and `"null"`. They must not be added to this oracle corpus as name
witnesses; product fixtures own their exact names, source order, coexistence,
and duplicate boundaries.
For an empty, whitespace-only, or comment-only payload, `gray-matter` reports
`isEmpty` and does not invoke its YAML engine; the projection maps each form to
the same non-null empty metadata array. Comments remain presentation bytes and
never become semantic records.

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
coordinate models. The Properties comparison additionally omits record scopes
and limits number witnesses to exactly representable canonical spellings,
because the external libraries expose neither source-faithful record ranges
nor lossless numeric lexemes, and JavaScript object enumeration reorders
integer-index-looking names. Product fixtures own those facts, strict invalid
fallback, key-source validation, exact spelling and order for every valid
name, distinct textual names collapsed by the binding, nested-value rejection,
and resource limits.

For successful Properties inputs, the normalized semantic root contains a
`metadata` field: `null` means absent, while an array (including an empty
array) contains ordered `{name, value}` records using the tagged scalar/list
shape from `docs/specs/metadata.md`. The current implementation's missing
field is deliberately normalized to `null`, so every target gap remains
visible. When `Document.metadata` is implemented, its canonical debug field
must expose the same compact JSON value for this gate; that dump change lands
atomically with the public model and cross-binding fixtures.
