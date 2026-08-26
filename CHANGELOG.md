# Changelog

All notable release changes are recorded here. Markdown Core follows Semantic
Versioning for source packages and public API behavior; the C binary ABI is not
promised to remain compatible between releases.

## 3.0.0 - unreleased

The engine is reconstructed from the 1.0 baseline. The 2.0.0 line is withdrawn:
its major version was bought with a session and incremental parsing API that no
longer exists. 3.0.0 ships a different streaming surface, described below.

- The bindings' one entry is the living `Document`, fed in pieces and
  answering with `Read` values. Every `feed` returns the read after those
  bytes — a mid-stream projection whose incomplete trailing line is not yet in
  it and whose open constructs are projected as they stand — and `seal` ends
  the stream and releases the native shell, returning the sealed read,
  identical for the same bytes however they were fed. The whole-text parse is
  `Document(markdown).seal()`: a one-chunk stream, so the bindings'
  `Document.parse` one-shot entries are deleted and feed/seal partition
  invariance is the only identity there is. Every read is a plain value that
  retains nothing native and outlives the document. The block is the minimal
  update unit; the model is specified in `docs/STREAMING.md`. This is not the
  withdrawn 2.0.0 surface: there is no edit, no fork, and no snapshot handle —
  the returned read is the only answer there is. In C the surface keeps both
  entries under its own names: `markdown_core_document_parse`, and
  `markdown_core_session_new`, `_feed`, `_finish` and `_free`.
- A `Read` is the parse under its two total views: `semantic`, the tree, and
  `concrete`, the normalized source its scopes are counted against — the pair
  the C handle already publishes as siblings, copied out. The root node type
  is renamed `Document` → `Semantic` in all three bindings, completing the
  `concrete: Concrete` naming symmetry; it is an ordinary `Markup` carrying
  nothing but `content` and `scope`, and the visitor case is `visitSemantic`
  (Swift: `visit(_: Semantic)`). The C node kind and the canonical dump label
  keep `document`/`Document`. Swift's visitor protocol `MarkupVisitor` is
  renamed `Visitor`, matching Kotlin and ECMAScript.
- `Concrete.lineCount` is renamed `lines`, and `lineStart` is renamed
  `offset(line)` (Swift: `offset(of:)`): the answer is a byte OFFSET into
  `source`, which a `Scope` boundary never is, and the old name invited the
  one confusion the two views exist to prevent.
- The ECMAScript `Document` also implements `Symbol.dispose`, so
  `using document = new Document()` releases an abandoned stream at scope
  exit; `dispose()` remains, is idempotent, and is only owed for a stream
  abandoned before `seal`.
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
- The directive grammar is rebuilt against the syntax it claims to implement.
  `#name` and `.name` are `id` and `class`, `class` accumulates across an
  attribute list where every other name is last-wins, and `.` `:` `-` `_` are
  ordinary name characters from the second character on, so `{a:b}` is one
  attribute rather than the start of a nested directive. An attribute name may
  not begin with punctuation, an `=` promises a value, an unquoted value holds
  no `<` `>` `=` or backtick, and a quoted value needs whitespace or `}` after
  it. An attribute list the parser refuses leaves the directive standing and
  its braces beside it as text instead of taking the directive down, and a text
  directive's colon has no colon beside it, so `x ::a y` is text.
- A formula body that begins and ends with a space or a line ending, and is not
  all whitespace, loses one from each end. It is both ends or neither:
  `text $$ mid$$ text` reports `" mid"`, because the space the rule wants at the
  end is not there.
- Report an ordered list of diagnostics beside the parsed document —
  `(severity, code, scope, message)` — covering the six places where a
  construct the author wrote did not become one and neither the tree nor the
  concrete records can say so: a directive's rejected label or attribute list, a
  directive block that did not open or never closed, a table whose delimiter row
  does not match its header, and a label the parser refused as too long. A
  well-formed reference or footnote call that resolves to nothing is not among
  them: CommonMark defines that outcome — the bytes are prose — so nothing
  failed and nothing is reported. Every diagnostic is decidable from the
  construct's own bytes and speaks when the construct completes. Recording them
  changes nothing the parse builds, and an allocation the list cannot make
  abandons the parse rather than reporting a short one.
- Report an allocation loss as `MARKDOWN_CORE_ERROR_ALLOCATION_FAILED` rather
  than as `MARKDOWN_CORE_ERROR_INTERNAL`, and stop the failure reporter needing
  an allocation of its own to say so.
- A node's `scope` is a pair of line/column BOUNDARIES saying which range of the
  source an element occupies — not a byte range, and no substring is taken with
  it. A block closed by a blank line therefore ends at column 0 of that line,
  which is what cmark-gfm reports and what an editor needs.
- `Document.concrete` is the normalized source and its line index: the text a
  scope's coordinates are counted against, which is not the string that was
  passed in wherever it held a NUL.
- `markdown_core_document_root` is renamed `markdown_core_document_semantic`,
  because the parse now has two total views and the old name did not say which
  one it returned.
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
  are a list of name/value pairs rather than a string of normalized JSON.
  `markdown_core_node_directive_attribute_at` reads one pair, and the dump
  prints `attributes=[class="x" k="v"]` where it printed JSON object text.
- `mode` is removed from `Code`, `CodeBlock`, `Directive`, `DirectiveBlock` and
  `FormulaBlock`, where it could only ever hold the one value its kind implies.
  `Formula` keeps it, because a formula is the one kind where it varies.
- `markdown_core_node_footnote_id` is replaced by
  `markdown_core_node_association`, which answers for all five kinds carrying a
  label and reports the label as written beside the normalized identifier it
  matches by. `markdown_core_node_directive_first_label_child` and
  `markdown_core_node_directive_first_content_child` are removed: a directive's
  label is an ordinary node in the child list. `FootnoteDefinition.id` and
  `FootnoteReference.id` become `label` and `identifier` in all three bindings
  for the same reason.
- `ParseOptions.dollarFormulaDelimiters` and
  `ParseOptions.latexFormulaDelimiters` are removed from Swift, Kotlin and
  ECMAScript. Attaching `formula` is the only switch the extension has.
- A parse failure carries no scope. `markdown_core_error_get_scope` and
  `ParseError.scope` are removed from C, Swift, Kotlin and ECMAScript: an input
  the parser could not turn into a document has no extent to point at, and a
  failure the author could act on would have been a diagnostic instead.
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
