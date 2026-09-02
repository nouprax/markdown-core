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
- A `Read` is the parse: `semantic`, the tree. Scopes are counted against
  the normalized source — UTF-8 as fed, every NUL replaced by U+FFFD, every
  line ending a single `\n` and every line having one — which the library
  does not hand back: a caller whose input can differ from it applies the
  same normalization to its own copy before resolving a scope. The root node
  type is renamed `Document` → `Semantic` in all three bindings; it is an
  ordinary `Markup` carrying
  nothing but `content` and `scope`, and the visitor case is `visitSemantic`
  (Swift: `visit(_: Semantic)`). The C node kind and the canonical dump label
  keep `document`/`Document`. Swift's visitor protocol `MarkupVisitor` is
  renamed `Visitor`, matching Kotlin and ECMAScript.
- Every `Markup` carries **`id: Identity`** — the pair `(block, ordinal)`,
  the name a consumer tracks an element by across a stream's feeds: the
  render key. `block` is the owning block's document-unique mint — the block
  is the minimal update unit, so it alone names the region an incremental
  consumer re-renders — and `ordinal` is the node's pre-order ordinal among
  that block's inline descendants, 0 for the block itself. A block keeps its
  identity across feeds however the bytes arrive; the halves are opaque
  values. The C facade answers it whole (`markdown_core_node_identifier`), and
  the canonical dump leads every line with `id=block:ordinal`.
- A reference names the definition it resolved to: `LinkReference`,
  `ImageReference` and `FootnoteReference` carry **`definition: Identity`**,
  the identity of the FIRST definition of their label in document order,
  while every later definition of the same label stays in the tree where it
  was written. The reference kinds stop carrying `identifier`: their match
  key equals the winning definition's by construction. The definitions
  rename `identifier` to **`norm`** — the match key their label folds to,
  still compared by bytes, the footnote kinds still keeping the leading `^`.
  In C, `markdown_core_node_reference_definition` reads the edge and
  `markdown_core_node_association` keeps its five-kind contract under its
  own names.
- The whole read crosses a binding boundary as ONE buffer, in ONE
  allocation. `markdown_core_document_wire` serializes the document —
  the canonical BYTES beside the canonical dump's TEXT, the
  two changing together or not at all — with caller-reserved prefix room, so
  a transport's versioned envelope is stamped into the payload's own
  allocation rather than copied into a second one; both managed bridges wrap
  it in that MKC8 envelope. The Kotlin transport keeps its single JNI
  crossing, and the ECMAScript runtime stops walking per-field wasm
  accessors — thousands of boundary crossings per document become one, and
  the large-document feed-seal-copy drops from 45.7ms to 27.7ms while now
  carrying the identity data. A read the caller's contract DISCARDS — the
  `Document(markdown)` constructor's initial feed — stops being built at
  all: `markdown_core_session_advance` takes the bytes without projecting or
  serializing a document nothing would decode, which is what takes the
  Kotlin large-document number from 26.6ms to 18.1ms. The
  ECMAScript coverage ledger's unpinned surface shrinks with the walk it
  covered: the retired decoder's 22-line allowance becomes the wire
  decoder's 4-line, statically unreachable remainder.
- A feed's payload is a DELTA against the payload before it (#162). Every
  wire payload leads with a frame byte — `markdown_core_wire_frame`, FULL
  or DELTA — and a session asked for DELTA answers the tree by its
  differences from the last payload it wrote: the blocks the engine
  retained across the two derivations are named by position and reused,
  and only the open spine and the changed blocks cross. The bindings hand
  the previous read's values into the new read wherever the delta says
  nothing moved — a reused subtree is the same object, still an immutable
  value — and ask for FULL whenever they hold no previous read. What a
  reader still pays per feed is one reference copied per reused child into
  the new read's child list, bounded in docs/STREAMING.md §6. In C,
  `markdown_core_session_feed_wire` takes the frame request, and
  `markdown_core_session_finish_wire` seals the stream on the wire in the
  frame asked for; `markdown_core_document_wire` always writes FULL. The
  ECMAScript feed-loop benchmark on the 158 KB document falls from 986.6 ms
  to 27.8 ms at 256 chunks, the Kotlin one from 453.9 ms to 20.5 ms with a
  third of the resident memory, and the feed-loop caps in both bindings
  tighten from 32 to 4 per 16× step. The decoders' coverage ledgers shrink
  with the change: the ECMAScript wire decoder's unpinned branches 7 → 6 and
  the Kotlin markup decoder's 28 → 21, the delta's refusals and the table's
  header-first rule now pinned by payload.
- The C facade's child navigation is a BY-VALUE cursor:
  `markdown_core_node_children` opens one and `markdown_core_children_next`
  steps it, each step O(1), no allocation. The pair replaces
  `markdown_core_node_get_first_child`/`markdown_core_node_get_next_sibling`
  (D9): sibling order is the parent's fact, so the cursor carries the parent
  and the position, and asking a bare node what follows it — the one
  question a node shared between two reads of a stream cannot answer — is no
  longer asked. The bindings' surfaces are unchanged: ECMAScript and Kotlin
  read the wire, and Swift's `Markup` builders moved to the cursor
  internally.
- The ECMAScript `Document` also implements `Symbol.dispose`, so
  `using document = new Document()` releases an abandoned stream at scope
  exit; `dispose()` remains, is idempotent, and is only owed for a stream
  abandoned before `seal`. Because sealing releases the shell, no public call
  can reach a native session error any more, and the coverage ledgers'
  unpinned defensive surface moved with it: in ECMAScript, `session.ts`'s
  allowance is retired and `document.ts` and `parser.ts` carry the
  unreachable allocation-failure and error-release arms; in Swift,
  `NativeValues.swift`'s native-error constructors join for the same reason,
  and the root's precondition arms move from `Document.swift` to
  `Semantic.swift` beside `Read.swift`'s copy-in guard.
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
- Report an allocation loss as `MARKDOWN_CORE_ERROR_ALLOCATION_FAILED` rather
  than as `MARKDOWN_CORE_ERROR_INTERNAL`, and stop the failure reporter needing
  an allocation of its own to say so.
- A node's `scope` is a pair of line/column BOUNDARIES saying which range of the
  source an element occupies — not a byte range, and no substring is taken with
  it. A block closed by a blank line therefore ends at column 0 of that line,
  which is what cmark-gfm reports and what an editor needs.
- `markdown_core_document_root` is renamed `markdown_core_document_semantic`,
  naming the view it returns.
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
