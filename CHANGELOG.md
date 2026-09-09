# Changelog

All notable release changes are recorded here. Markdown Core follows Semantic
Versioning for source packages and public API behavior; the C binary ABI is not
promised to remain compatible between releases.

## 3.0.0 - unreleased

- Generate heading anchors with Unicode simple lowercase, global explicit-anchor
  reservation and deterministic duplicate suffixes. Resolve full, collapsed and
  shortcut references to authored heading labels, including forward references;
  explicit reference definitions win. Share each final target through the
  existing resource model in C, Swift, Kotlin and ES. Preserve source scopes,
  cover allocation failure and linear work, and retire the P3/P4 Pandoc gaps.

- Attach normalized attributes to inline code, ATX/Setext headings, fenced code,
  links and media. Reference occurrences inherit definition attributes while
  retaining local anchors, duplicate declarations, typed dimensions and exact
  source scopes. Keep each binding's native collection and ownership model;
  encode and decode inherited definition values once. Fix Setext heading ends
  to stop at the underline. Add the pinned Pandoc 3.11 oracle gate and exact
  evidence for the dialect's deliberate attribute-merge difference.

- Add always-on `++inserted text++` as `Insertion(content)` across C, Swift, Kotlin,
  and ES, including typed visitors, JNI/Wasm transports and canonical dumps.
  Pin the insertion oracle and replay upstream, composition, source-scope,
  strict OOM and linear-work cases.

- Close the Obsidian evidence track across C, Swift, Kotlin and ES with shared
  composition fixtures, fixed fuzz inputs, HTML/comment boundary work probes
  and a mixed-ownership allocation-failure sweep. The supported subset covers
  cross links and embeds, marks, comments, inline/referenced footnotes, authored
  task markers, fixed-field Properties, block identifiers, callouts and media
  dimensions. Parsing and source ownership stay in the core; rendering, vault
  resolution and execution stay with consumers. The source snapshot is
  `obsidianmd/obsidian-help@d780d6b48a92ee6a150304b40ee888f322bf43bf`
  (read 2026-09-03); the dialect modules own the documented adaptations.

- Rename the canonical `Image` node to `Media` across C, Swift, Kotlin, ES,
  visitors and dumps. ES uses `kind: "media"`; C uses
  `MARKDOWN_CORE_KIND_MEDIA` and `markdown_core_node_dimensions`.
  The parser preserves the authored target without inferring its media type.

- Parse external image dimensions from complete `W`, `WxH`, `alt|W` and
  `alt|WxH` labels on C, Swift, Kotlin and ES. Direct and resolved images use
  the same rule; dimensions belong to each occurrence, preserve parsed alt
  content and scopes, and accept positive 32-bit values without leading zeros.
  Split workspace transclusions into `CrossEmbedded(dest, label, dimensions)`
  and ordinary cross links into `CrossLink(dest, label)`, removing the embedded
  flag. CrossEmbedded labels use the same size grammar,
  retaining only the raw label prefix. Ordinary cross-link labels and malformed
  suffixes remain unchanged. C exposes the shared `markdown_core_node_dimensions`.
  Replace separate image width/height fields with `Media.dimensions`, an optional
  node-independent `Dimensions(width, height?)` value across every public surface.

- Recognize opening callout metadata on C, Swift, Kotlin and ES. Preserve the
  authored type in `variant`, map `+`/`-` to `collapsed`, and parse the optional
  inline title before body content. Keep inherited nesting and lazy continuation;
  metadata never becomes a body heading, table header or block identifier.
  Titles participate in reference resolution, owned-field traversal and cleanup.

- Land O6 as document-owned metadata with ten direct optional fields: `name`,
  `title`, `subtitle`, `time`, `date`, `authors`, `keywords`, `abstract`, `state`,
  and `comment`. Ignore unknown names, unnamed text, comments, invalid values
  and later duplicates while preserving valid neighboring fields. Authors and
  keywords accept single strings, bracketed arrays and block lists; abstract
  and comment accept single-line text and `: |` literal prose. Preserve exact
  numeric text and the complete envelope scope across C, Swift, Kotlin and ES.
  Metadata stays outside Markup and visitors. Remove the metadata collection,
  record wrappers, per-field scopes, JSON root parser and source indexes.

- Recognize one authored Unicode scalar as a task marker on C, Swift, Kotlin,
  and ES, including `?`, `✓` and `🚀`. Consume the entire SP/TAB/VT/FF separator
  run before deciding the first block, and inspect only an item's opening
  prefix. Preserve the exact scalar in `ListItem.marker`; empty or multi-scalar
  candidates and prefixes without a separator retain their literal fallback.

- Recognize `^[inline note]` on C, Swift, Kotlin, and ES through the shared
  bracket algorithm. Store parsed inline bodies directly in document-owned
  Footnote values, merge both footnote forms by source position, and assign
  deterministic `inline-N` ids after reserving every authored id. Nested
  citations remain id edges, so traversal has no object cycles.

- Recognize `%%...%%` comments as `Comment` on C, Swift, Kotlin, and ES, the
  kind an HTML comment already produces. Inline, the body ends at the first
  later `%%` and is opaque to every other construct; on fence lines of their
  own, a block comment commits only when a closer line follows under the same
  container prefixes, and its literal keeps indentation and line endings as
  written. Nothing is stripped: a consumer that does not want comments drops
  the nodes.

- Recognize `==highlight==` as `Mark(content)` on C, Swift, Kotlin, and ES.
  Parse nested inline content through the shared delimiter stack, consume two
  equals signs per match, and retain unmatched single signs as text. Scopes
  cover the matched delimiters; code, formulas, HTML tokens, and cross links
  keep their bodies opaque.

- Recognize `[[...]]` and `![[...]]` as `CrossLink` on C, Swift, Kotlin and ES.
  Preserve raw paths, destination anchors and nullable labels, including escaped
  table pipes and authored empty labels. Formula bodies claim their bytes before
  later inline syntax, including a cross-link candidate containing a formula closer.

- Add universal `anchor` and ordered `Attributes(classes, records)` to every
  node on C, Swift, Kotlin, and ES. Directives now use the one shared Pandoc
  attribute operation: the last ID wins, an empty final ID clears it, classes
  append, and records preserve duplicates. Empty and absent containers have
  the same empty value. Directive names begin with a Unicode letter.
- Add document-owned Metadata values and optional image width/height across
  the facade and transports. Properties syntax and image dimension syntax
  follow in O6 and O9; O6 now produces metadata, while dimensions await O9.

- Unify tables as columns plus head/content/foot row groups on C, Swift, Kotlin,
  and ES. Remove row header flags, expose cell spans, and retain inline or block
  cell content directly. Escaped pipes now retain authored content coordinates.

Reconstruct the cmark-derived engine as the renamed, parser-only Markdown Core
product. This line adds the repository parser extensions and immutable AST
facade while removing renderer support and the caller-driven feed lifecycle.

- Remove the parse option surface: `Document.parse(source)` is the only entry
  point on every surface, the C facade's `markdown_core_parse_options` and
  `markdown_core_parse_options_init` are gone, and the Swift, Kotlin, and
  ECMAScript `ParseOptions` types with them. Every feature of the Markdown
  Core dialect is always recognized. Smart punctuation is removed with the
  options: quotation marks, hyphen runs, and periods are stored as written.
  The installed `markdown-core` CLI takes no `--profile`, `-e`, or `--smart`,
  and the test tree keeps no layer selection either: every fixture, oracle
  gate, and audit parses the one language, and each place the dialect
  deliberately leaves an oracle's language is registered against its exact
  inputs. Running the shipped language through the gates also showed that a
  `\\]` or `\\)` with nothing to close swallowed the bracket after it, so
  `[bar\\]` stopped being a reference, and that the text directive claimed
  the colon of `mailto:x@y.z` before the autolink pass ran. The formula
  scanner now leaves an unopened closer to the base language, and the
  autolink scanner claims a colon that an address follows, as cmark-gfm
  links it.
- Replace the lossy nullable task-list completion boolean with the authored
  `ListItem.marker` as an owned UTF-8 string and expose completion
  only as a derived binding convenience. Ordered lists now report their
  authored decimal `variant` and `period` or `parenthesis(closed=false)` delimiter on every
  public surface (M5).
- Unify specimen definitions with the citation model: `Document.specimens`
  owns `Specimen(id?, start?, content, scope)` values, and references use
  `CitationReferent.specimen(id)`. All bindings, transports, dumps and walks
  preserve the values; specimen syntax remains scheduled for P9b. Remove the
  reserved example list variant and item label field. Alphabetic and Roman
  variants retain `alpha(lowercased:)` and `roman(lowercased:)`.
- Add the `Comment` kind on every surface (M0). An inline HTML comment token
  is a `Comment` whose literal is the bytes between `<!--` and `-->`, empty
  for `<!-->` and `<!--->`, and an HTML block that opens with `<!--` and whose
  end line holds only whitespace after the first `-->` is a block `Comment`
  whose literal keeps its line endings; every other HTML block stays
  `HTMLBlock` as written. Nothing strips a comment any more: the C option bit
  and the tree pass that removed them are gone, and a consumer drops the nodes
  instead. Two scopes move with it. An HTML block that its own end condition
  closes now ends on that line rather than the line before, so `<pre>`,
  `<?...?>`, `<![CDATA[`, and comment blocks that span lines end at their
  terminator, and an ATX heading covers its whole line, closing sequence and
  trailing spaces included, as a paragraph does. The cmark and cmark-gfm gates
  project upstream's comment nodes to `Comment` with the delimiters removed,
  the remark gate does the same for mdast, the position ledger records where
  cmark ends those blocks early, and the comments module's HTML examples are a
  package fixture.
- Replace `Link.destination` and `Image.source` with `dest`, a tagged
  `Destination` value with the branches `url(String)` and
  `cross(path: String, anchor: String?)` (M1). Every link and image the
  grammar produces today carries the `url` branch, the complete decoded
  destination, empty for `[a]()` and `[a](<>)`; the `cross` branch is declared
  on every surface and produced by nothing until cross links land. The C
  facade's `markdown_core_node_destination` and `markdown_core_node_title`
  replace `markdown_core_node_link_properties` and
  `markdown_core_node_image_properties`, the dump prints the value as
  `dest=url("...")`, the Swift enum, Kotlin sealed interface, and ECMAScript
  union state both branches, and the cmark, cmark-gfm, remark, and Obsidian
  gates compare the real tagged value instead of wrapping a string. The
  links-and-images module's destination and autolink examples are a package
  fixture.
- Resolve every reference link and image (M2). A link reference definition
  produces no node: the parser consumes it into its reference map, which owns
  the definition's destination and title once, and every successful full,
  collapsed, shortcut, and autolink form is the `Link` or `Image` it names,
  with the definition's destination and title and its own occurrence scope.
  Every occurrence of one definition shares that one resource, so a long
  destination referenced many times is stored once in the C tree, sent once
  over the JNI and Wasm transports, and materialized once by every binding;
  `markdown_core_node_resource` answers the identity, and the expansion bound
  is re-derived around it. Only the parser writes a resource: the engine's
  `markdown_core_node_get_url`, `set_url`, `get_title`, and `set_title` are
  removed, a destination and title are read through the facade, a link built
  through the engine API or converted with `markdown_core_node_set_kind` is
  the link `[a]()` is, and an extension's per-node data lives beside the
  type-specific arm rather than in it. `LinkReference`, `ImageReference`,
  `ReferenceDefinition`, and `ReferenceForm` leave every surface with their
  accessors, `markdown_core_node_association` answers for the two footnote
  kinds only, and an unresolved reference or an invalid definition keeps the
  inherited literal text. The cmark and cmark-gfm gates no longer project a
  reference model, the remark gate resolves mdast's definitions inside its
  projection, and the order-independence audit compares the resolved links.
  The links-and-images module's resolved-reference examples join the package
  fixture.
- `Callout` replaces `BlockQuote` (M3). Every `>` container is a `Callout`
  with `variant`, `collapsed`, and `title`, which the callouts module's
  metadata rule fills in with `O8`; until then every callout is metadata-free,
  with a null variant, `collapsed=null`, and no title, and the inherited
  prefix, laziness, continuation, blank-line, and nesting rules are unchanged.
  `collapsed` is the fold marker as an optional boolean, like a list item's
  `checked`: null without a marker, false for `+`, true for `-`. A present
  title holds at least one node, so its first node is its presence on the C
  facade and its count is its presence on the wire. The kind is renamed on
  the C engine and facade, which gain `markdown_core_node_callout_properties`
  and `markdown_core_node_callout_title`; the dump prints `variant` and
  `collapsed` and nests a non-null title as a `Title` group before the
  content; every binding's model, visitors, walkers, and dumpers replace
  `BlockQuote` with `Callout` and visit the title before the content; the JNI
  payload and the Wasm result carry the three fields; and every fixture and
  golden containing a quote regenerates. The cmark, cmark-gfm, and remark
  projections state the metadata-free callout, the Obsidian
  `universal-callout-container` delta stays as the general projection of
  mdast `blockquote`, the containment ledger names `Callout`, the manifest
  gains `callout.variant.null`, `callout.collapsed.null`, and
  `callout.title.null`, and the base and callouts modules' metadata-free
  examples join the package fixture `dialect-callouts.txt`.
- Replace `FootnoteReference` and `FootnoteDefinition` with the citation model
  (M4). A footnote call is an inline `Cite` whose items are `Citation` values,
  and a footnote definition is a `Footnote` value the document owns through
  `Document.footnotes`, ordered by scope start and visited after the content;
  neither value is a `Markup` kind, a cite is a leaf, and the document's
  children count its content alone. An inherited `[^label]` call is a one-item
  cite whose `Citation` carries the `footnote` referent, the normalized label
  without the caret as its id, and empty prefix and suffix; the `bib` referent
  and `BibMode` are declared on every surface and produced by nothing until
  citations land with `P7`. Repeated calls share one footnote: the first
  definition of an id is the one they resolve to, a later definition of the
  same id is a footnote after it, a definition nobody calls is a footnote
  too, and the inherited block and bracket grammars are unchanged.
  `markdown_core_node_association` is removed; the C facade gains
  `markdown_core_node_cite_citations`, `markdown_core_citation_next`,
  `markdown_core_citation_scope`, `markdown_core_citation_referent`,
  `markdown_core_citation_prefix`, `markdown_core_citation_suffix`,
  `markdown_core_node_document_footnotes`, `markdown_core_footnote_next`,
  `markdown_core_footnote_scope`, `markdown_core_footnote_id`, and
  `markdown_core_footnote_content`. The dump prints a cite's items as
  `Citation` value lines with `CitationPrefix` and `CitationSuffix` groups and
  the document's footnotes as `Footnote` value lines after its content; every
  binding's model, visitors, walkers, and dumpers replace the two kinds with
  `Cite`, `Citation`, and `Footnote`, the walking visitors gain value callbacks
  for the two, and the JNI payload and the Wasm result carry the values as
  records. The cmark-gfm and remark projections lower upstream's footnote
  nodes to the same model and lift both sides' footnotes into one order for
  comparison, the manifest gains `citation.referent.footnote`,
  `citation.affix.empty`, `document.footnotes.empty`,
  `document.footnotes.populated`, `document.content-before-footnotes`, and
  `cite.items-in-order`, and the footnotes module's referenced-form examples
  join the package fixture `dialect-footnotes.txt`.
- Open one formula of a form at a time. A formula opener is a delimiter only
  while no opener of its form is unmatched, and a closer only while one is, so
  a body runs from its opener to the first closer of its form and an opener
  inside it is the body's own bytes: `\\(a \\(b\\) c\\)` is the formula
  `a \\(b` followed by text. Before, the inner pair formed first and the outer
  closer then took the outer opener across it, so every level of a nest built
  a literal the next level threw away, and a paragraph of thousands of nested
  `\\(` and `\\)` pairs took seconds; a pathological case pins twenty
  thousand.
- Raise the Swift package contract to Swift tools 6.3 and iOS 26/macOS 26,
  refresh Gradle, AGP, Kotlin, Node.js, pnpm, Emscripten, and SwiftLint
  pins, and audit every duplicated toolchain declaration for exact agreement.
  The Gradle 9.7.1 distribution checksum and rotated release-signing subkey are
  both pinned for wrapper and IDE-source dependency verification.
- Keep the C and Kotlin packages independent in JetBrains workspaces. Gradle
  IDE sync no longer projects the cross-package JNI graph through
  `android-runtime/cpp`; normal Android builds still compile that graph, while
  the canonical C project remains owned by CMake. Kotlin/Native IDE import
  generates declarations from the public header without building C archives;
  product and test KLIBs still build and embed those archives.
- Replace the cross-runtime hosted-runner metrics jobs with one non-blocking C
  parser comparison. PR comments omit the meaningless boundary column, reuse
  an exact-base-SHA artifact when available, and otherwise build and publish
  that baseline before measuring the head.
- Remove the obsolete line/branch coverage ratchet and its instrumented CI
  builds. Canonical AST cases, parser corpora, parity oracles, strict OOM and
  lifecycle tests remain the required semantic evidence instead of treating
  compiler, loader, and defensive wire branches as one quality score.
- Synchronize the CommonMark parser from cmark 0.29 through stable cmark 0.31.2,
  including published syntax, complexity/security, numeric-entity, Unicode 17,
  case-folding, entity-table, and scanner changes. Current cmark is the
  CommonMark authority; dormant cmark-gfm now judges only GFM extensions, with
  remark/mdast used for correction and supplementation.
- Keep the bytes of a footnote call whose label crosses a line ending, and read
  a label spelled with a character reference out of the source rather than out
  of a released buffer.
- Resolve a repeated footnote label to the definition that opens first, and
  keep the definition that does not win where it was written, instead of
  destroying it and everything inside it.
- Give an unresolved footnote call a source position instead of line zero.
- Stop the formula and directive extensions from changing what CommonMark
  emphasis means when they are attached.
- Test the flanking scan's bound before reading it, and stop the directive
  extension registering a byte its inline matcher cannot consume.
- Invalid directive attributes leave the directive standing and the braces
  available to ordinary inline parsing. Attribute recognition shares one
  index per input extent, bounding overlapping malformed and unclosed
  candidates linearly; allocation failures remain terminal.
- A formula body that begins and ends with a space or a line ending, and is not
  all whitespace, loses one from each end. It is both ends or neither:
  `text $$ mid$$ text` reports `" mid"`, because the space the rule wants at the
  end is not there.
- Remove the incremental parser lifecycle and file wrapper. Parsing is one
  synchronous source-to-document transaction; there is no feed, stream, edit,
  session, snapshot, or delta API.
- Make every allocation failure terminal. No hash-index failure may switch to
  sorting or a linear scan, and no allocation refusal may still return a
  document. The facade reports `MARKDOWN_CORE_ERROR_ALLOCATION_FAILED` through
  an immutable error value that allocates nothing itself.
- Remove the diagnostic list and its CLI, C ABI, test census, and recording
  hooks. Parse errors are the only failure channel.
- Guarantee concurrent use by independent parser instances: the engine has no
  writable process-global parser state or initialization registry, and parser,
  option, extension, allocation, and failure state is transaction-local.
- A node's `scope` is a pair of line/column BOUNDARIES saying which range of the
  source an element occupies — not a byte range, and no substring is taken with
  it. A block closed by a blank line therefore ends at column 0 of that line,
  which is what the cmark-family parser reports and what an editor needs.
- Remove source retention and the `Concrete`/line-index API. A parse returns
  only the immutable AST; consumers that need the Markdown text retain their
  own input.
- A link reference definition is a node. `ReferenceDefinition` sits at the byte
  where its `[` was written, in the container it was written in, carrying
  `label`, `identifier`, `destination` and `title`, which
  `markdown_core_node_definition_resource` reads. The baseline harvested the
  definition and discarded it, so nothing in the tree said the source had one.
- A reference is a node too. `LinkReference` and `ImageReference` carry the
  label as written, the identifier it matches by, and the `form` it was spelled
  in — `full`, `collapsed` or `shortcut` — instead of being flattened into a
  `Link` or an `Image` with the definition's destination copied into them.
  `markdown_core_node_reference_form` reads the form.
- A directive's label is a node of its own, `DirectiveLabel`, and its attributes
  are a list of name/value pairs rather than a string of normalized JSON. The
  list preserves each name's first-occurrence source order; duplicate values
  update or accumulate in that original slot instead of triggering a sort.
  `markdown_core_node_directive_attribute_at` reads one pair, and the dump
  prints `attributes=[k="v" class="x"]` where it printed JSON object text.
- `mode` is removed from `Code`, `CodeBlock`, `Directive`, `DirectiveBlock` and
  `FormulaBlock`, where it could only ever hold the one value its kind implies.
  `Formula` keeps it, because a formula is the one kind where it varies.
- `markdown_core_node_footnote_id` is replaced by
  `markdown_core_node_association`, which answers for all five kinds carrying a
  label and reports the label as written beside the normalized identifier it
  matches by. `markdown_core_node_directive_first_label_child` and
  `markdown_core_node_directive_first_content_child` are replaced by
  `markdown_core_node_directive_label`: a directive label remains `Markup`, but
  is a typed field rather than an element of the directive child/content list.
  `FootnoteDefinition.id` and
  `FootnoteReference.id` become `label` and `identifier` in all three bindings
  for the same reason.
- `ParseOptions.dollarFormulaDelimiters` and
  `ParseOptions.latexFormulaDelimiters` are removed from Swift, Kotlin and
  ECMAScript. Attaching `formula` is the only switch the extension has.
- A parse failure carries no scope. `markdown_core_error_get_scope` and
  `ParseError.scope` are removed from C, Swift, Kotlin and ECMAScript: an input
  the parser could not turn into a document has no extent to point at.
- `null` and `""` are different answers everywhere, and nothing folds one into
  the other. `null` means the source did not write the field; `""` means it
  wrote it and it was empty. An optional string is reported as
  `markdown_core_optional_string` in C — a value beside a presence flag,
  matching the optional Int and optional Bool the header already had — so
  `CodeBlock.info`, `CodeBlock.language`, `Link.title`, `Image.title` and
  `ReferenceDefinition.title` state which of the two they are.
- `Link.destination` and `Image.source` are no longer optional. `[a]()` and
  `[a](<>)` wrote a destination and wrote nothing in it, so both report `""`;
  a link with no destination at all is a `LinkReference`. The dump prints
  `destination=""` where it printed `destination=null`.
- `markdown_core_string_view` is renamed `markdown_core_string`. The `_view`
  suffix named a C++ type this is not; that the bytes are lent by the document
  rather than copied out of it is said in the header instead of in the name.
- The Swift binding says what the canonical AST contract says. `children` is
  `content` on every kind that has it, `List.isTight`, `ListItem.isChecked`,
  `CodeBlock.isFenced` and `CodeBlock.isClosed` drop an `is` the contract never
  had, and `List` reaches its `items` through the typed `[ListItem]` edge the
  contract names rather than through a generic child list. Kotlin and
  ECMAScript already agreed; the Swift model had drifted.

## 1.0.3 - 2026-07-15

- Add a single environment setup and validation entry point for local
  development, CI, IDE import, and release preparation.
- Refresh supported build runners and toolchains while keeping workflow policy
  audits focused on security and quality outcomes rather than Action versions.
- Harden PR concurrency, release staging, publication recovery, package audits,
  and cross-platform consumer validation.
- Add a reusable repository setup template covering platform-native bindings,
  stable quality gates, tag releases, and lessons learned from deployment.

## 1.0.2 - 2026-07-15

- Fix the Kotlin/JVM native loader so clean application shutdowns remove both
  the extracted JNI library and its temporary directory.
- Use JVM platform library-name mapping and non-overwriting extraction while
  preserving zero-configuration native loading from the published JAR.
- Fix Kotlin Multiplatform project import in Android Studio and IntelliJ IDEA so
  source sets remain visible after Gradle sync.
- Add a Gradle-backed `All Kotlin tests` IDE entry that runs every Kotlin test
  supported by the current host, including Android managed-device coverage.
- Expand consumer-facing package documentation and release guidance.

## 1.0.0 - 2026-07-15

- Establish the standalone Markdown Core C parser and read-only canonical AST
  facade without renderer APIs.
- Add coordinated SwiftPM, Kotlin Multiplatform/Maven Central, and
  ECMAScript/WASM packages backed by the same parser and canonical AST contract.
- Add cross-platform correctness, conformance, consumer, security, package
  content, performance, and release-support validation.
