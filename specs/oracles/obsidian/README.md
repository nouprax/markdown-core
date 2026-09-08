# Obsidian parser oracles

Properties status (2026-09-08): O6 supports only the specified Obsidian
Properties fields and forms. Metadata is not a complete YAML document. The [landing plan](../../../docs/plans/2026-09-04-canonical-vnext-landing-plan.md)
reopens implementation and oracle migration. The existing runner/corpus still
exercise the superseded alias/tag projection; green checks and closed old gaps
do not prove the revised target. The general YAML parser selection/vendoring
task is withdrawn. The pinned package remains test tooling for inputs already
inside the supported Properties grammar; YAML acceptance cannot define that
grammar or justify additional syntax.

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

The Properties page is the source for beginning-of-file placement, the
three-hyphen fence form, the supported consumer domain, and the Properties
editor's limits on nested values and Markdown rendering; `docs/specs/dialect/properties.md`
states the rule. The harness therefore owns one exact,
line-oriented envelope scanner and passes only the bytes between a valid pair
of fences to the YAML oracle. A package-specific frontmatter recognizer is
neither an authority nor an intermediate normalization layer.

For the corrected O6 target, input selection follows the Properties module
before `yaml` compares ordered pairs and CST tokens. Single-line property
values, flat lists, and the documented JSON alternative form that intersection. The Properties projection keeps
direct scalar names, exact numeric spellings, atomic scalar values, and flat
text/number lists. It does not construct a generic YAML object graph or resolve
aliases/tags. The runner's old alias, tagged-empty-null, multiline-folding and
general YAML mapping success canaries and corpus entries need migration with
the bounded Properties producer. JSON objects keep JSON key/value syntax;
YAML key-only flow pairs are outside this task.

The official help documents property types and source forms. Its nested-value
UI limitation does not establish that Obsidian's underlying YAML parser rejects
nested input, aliases, or tags. This repository excludes those constructs from
its data projection and preserves their source according to the user's explicit
retention rule. Obsidian's [public API](https://github.com/obsidianmd/obsidian-api/blob/master/obsidian.d.ts)
declares `parseYaml`; this offline gate has no Obsidian runtime and makes no claim about which library the app uses.

Only the exact outer `---` line closes the envelope. Complete envelopes always
produce metadata; unsupported or malformed members become `comment` values,
while valid neighboring members remain data. The YAML data comparison removes
those comment cases, including source-only payloads, only for semantic parity.
Product fixtures must verify exact comment bytes, ordering, source-independent
ownership, member recovery, scopes, resource bounds, and allocation failure.
Whole-document YAML success/failure cannot substitute for those tests.

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
record to retain ordered, in-envelope CST range evidence. Its corpus covers
integer-looking keys in non-JavaScript order, exact large/decimal/exponent/
negative-zero number spellings, quoted key decoding, and projection failures.
Legacy alias success coverage remains pending migration, as noted above. Product fixtures remain the rule for canonical
binding-coordinate scopes, allocation failure, and parser-wide resource
limits.

For successful Properties inputs, the normalized semantic root contains a
`metadata` field: `null` means absent, while an array (including an empty
array) contains ordered `{name, value}` records using the tagged scalar/list
shape from `docs/specs/dialect/properties.md`. The gate reads the real Metadata
field from canonical dumps, projects comment cases away, and compares its data
records directly. The eight original gap closures belong to the superseded
implementation; the corrected O6 acceptance criteria require fresh evidence.
