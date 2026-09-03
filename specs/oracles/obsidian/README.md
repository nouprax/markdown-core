# Obsidian parser oracle

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

Popularity chooses an implementation oracle; it does not make that package the
OFM specification. The official Obsidian help snapshot registered in
`deltas.json` remains normative. This oracle is authoritative only for the
intersection it implements: wikilinks/embeds, highlights, comment removal, and
custom task characters. Tag and package-specific math syntax are disabled.
Callouts, block identifiers, inline footnotes, image dimensions, and Markdown
suppression inside HTML are absent from the direct parser and therefore stay
under official-example product fixtures.

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
coordinate models.
