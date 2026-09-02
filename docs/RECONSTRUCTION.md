# Reconstruction: cmark renamed as a parser-only Markdown Core

This document describes the architecture reconstructed from commit `904c66d`.
It is a statement of the current repository, not a staged implementation plan
and not a record of abandoned designs.

Markdown Core is a renamed cmark-family Markdown parser with repository-owned
syntax extensions and an immutable, typed document model. It parses one owned
source buffer into one owned `Document`. It does not render output and it does
not expose a parser lifecycle, incremental mutation, streaming, CST, or
diagnostic subsystem.

Historical documents under `docs/deprecated/` are non-normative. If they
describe a renderer, feed API, edit/session API, concrete syntax tree,
diagnostic list, or recoverable OOM behavior, those descriptions do not apply
to this repository.

## 1. Lineage and reconstruction boundary

The C engine retains cmark's block parser, inline parser, UTF-8 handling,
reference resolution, node ownership, and extension machinery. The public
product name and symbols are `markdown-core` / `markdown_core_*`; upstream
`cmark_*` product symbols are not a second supported API.

The reconstruction makes four deliberate product changes:

1. Rename the cmark-derived parser as Markdown Core.
2. Attach the repository's syntax extensions to the one parse transaction.
3. Project the parse result into the canonical immutable AST exposed by C,
   Swift, Kotlin, and ECMAScript.
4. Remove cmark's renderers and its caller-driven feed/finish lifecycle.

These are one coherent parser product. There is no baseline parser alongside
an extended parser, and there is no compatibility layer that preserves the
removed lifecycle or render APIs.

## 2. One parsing operation

The semantic operation is:

```text
owned source bytes + parse options -> Document | ParseError
```

The facade owns the complete source for the duration of parsing. Internally it
creates one private parser, attaches the configured syntax extensions, parses
the full buffer, finishes the document, and destroys the parser before it
returns. Parser state never escapes that transaction.

The engine has no writable process-global parser state and no initialization
registry. Its file-scope tables and extension descriptors are immutable.
Distinct parse transactions, traversal, dumps, and frees may run concurrently
without a library lock. A single returned document may be read concurrently;
its owner must synchronize the final free after all readers have finished.

Line endings are recognized as LF, CRLF, or CR. Embedded NUL bytes are parsed
as U+FFFD, matching the engine's source rules. An empty source is a valid
document.

The following interfaces do not exist:

- `parser_new` / `parser_feed` / `parser_finish` / `parser_free`
- a file or `FILE *` parsing wrapper
- edit, append, or incremental reparse operations
- stream, session, snapshot, or delta objects
- callbacks that expose a parser between source fragments

The CLI follows the same rule: it reads the selected input completely and
invokes the same one-shot parse transaction used by library consumers.

## 3. Syntax extensions

Extensions participate only in parser construction. They may register block
or inline syntax, node types, and parser-local state, but they do not create a
second parse path or a caller-visible lifecycle.

The repository-owned language includes the canonical cmark/GFM behavior plus
the enabled extension set represented by the current implementation and
fixtures, including tables, strikethrough, task lists, autolinks, footnotes,
and directives. The extension attach order is part of the grammar and is
audited because it affects delimiter and block precedence.

Each extension must obey the same ownership and failure contract as the core:
an allocation failure terminates the entire parse. An extension may not omit a
feature, switch algorithms, or return a smaller but apparently valid tree in
order to survive OOM.

## 4. Document model, not CST

The public result is a typed, immutable AST. Nodes describe semantic Markdown
constructs and their source scopes. The public visitor/walker surfaces are
exhaustive over the canonical node-kind contract.

`Document.concrete` is not a concrete syntax tree. It is the document's owned
source text plus its line index, used for source lookup and position
calculation. It has no token nodes, trivia nodes, recovery nodes, editable
records, or inverse tree-to-source machinery. The repository must not call it
a CST or grow it into one.

There is no diagnostic collection on `Document`, no diagnostic code enum, and
no parser hook for retaining or emitting diagnostics. Grammar near-matches are
represented only by the resulting AST and source scopes. Transaction failures
are reported by the single parse error value.

The canonical tree dump is a deterministic debug/verification representation
of the AST. It is not a Markdown, HTML, XML, CommonMark, LaTeX, manpage, or
plaintext renderer.

## 5. Rendering is outside the product

The cmark renderer family and renderer extension hooks are removed. The C
library and language bindings do not expose `render`, `markdown_to_html`, or
format-selection APIs. The CLI does not accept output-format or renderer
options such as `--to`, `--width`, `--unsafe`, `--hardbreaks`, or `--nobreaks`.

Consumers that need output generation must build it as a separate operation
over the immutable AST. Reintroducing a renderer into this repository would
change the product boundary and is not a compatibility fix.

## 6. Allocation failure is terminal

OOM has one repository-wide meaning:

```text
any required allocation fails -> no Document + ALLOCATION_FAILED
```

This applies to core nodes, parser buffers, references, extension state,
attributes, indexes, facade projection, and any other allocation required by
the transaction. There is no fallback to sorting, linear scans, reduced
indexing, truncated data, feature omission, alternate parsing, retry, or
lossless-looking success.

The default allocator returns `NULL` to the engine; it does not abort the
process. OOM state is sticky through the transaction. Once observed, the
partially built document is destroyed and the consumer receives no document.

Error reporting must itself remain available during OOM. The facade therefore
uses immutable process-lifetime error values for fixed failures, including
`MARKDOWN_CORE_ERROR_ALLOCATION_FAILED`. Reading or freeing such an error does
not allocate.

The strict OOM runner injects failure at every allocation in a full-featured
parse. Every injected failure must produce the terminal result above; any
successful document after an injected failure is a test failure, even if the
document happens to match the expected AST.

## 7. Public surfaces

The installed C package exposes one facade header,
`include/markdown_core.h`, and an exact export allowlist. Internal parser and
extension headers are not installed.

Swift, Kotlin, and ECMAScript expose the same concepts:

- immutable `Document` and typed markup nodes
- parse options and a one-shot parse entry point
- source text, scopes, and line/position lookup
- exhaustive visitors/walkers
- canonical AST debug dumping
- terminal parse errors

They do not expose native handles, parser ownership, mutation, rendering,
feed/edit/session types, CST nodes, or diagnostic lists.

## 8. Repository invariants and gates

The repository treats the removed APIs as forbidden, not deprecated. Audits
must fail if source, installed artifacts, or binding surfaces reintroduce:

- renderer functions, files, hooks, or CLI flags
- parser construction/feed/finish/free or file parsing
- edit, stream, session, snapshot, or delta APIs
- CST/token/trivia/recovery-tree APIs
- diagnostic records, codes, accessors, hooks, or CLI flags
- OOM fallback algorithms or successful injected-failure parses

Primary validation commands are:

```sh
cmake --preset default
cmake --build --preset default --parallel
ctest --preset correctness
ctest --preset conformance

cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset correctness-asan

cmake --preset ubsan
cmake --build --preset ubsan --parallel
ctest --preset correctness-ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset correctness-tsan

sh scripts/format-c.sh --check
sh scripts/format-cmake.sh --check
bash scripts/audit-public-surface.sh
bash scripts/audit-package-contents.sh
bash scripts/audit-test-topology.sh
node scripts/audit-source-lists.mjs
```

Required CI contains only reproducible quality evidence: correctness,
conformance, sanitizers, coverage, parity, package/consumer contracts, and
release-policy checks. It does not run benchmark jobs, upload PR performance
metrics, compare wall-clock values across hosted runners, or grant a
privileged workflow permission to publish such comparisons. Test artifacts
likewise contain test products only, not benchmark executables or classes;
their producers rebuild CI-owned staging trees so removed targets cannot leak
back in as stale files from an earlier local graph.

The C benchmark remains an explicit local measurement tool. It is opt-in
through the separate `benchmark` configure/build/test preset and never
changes status based on a measured duration or scaling ratio. A performance
observation becomes a gate only after it is expressed as a deterministic
semantic, structural, resource-bound, or operation-count invariant. The
reference-expansion regression follows this rule by bounding AST payload bytes
relative to source bytes without timing the parser.

The Swift, Kotlin, and ECMAScript five-sample benchmark scripts are removed.
They existed to feed the deleted PR metrics pipeline, had no controlled
baseline or trend store, and in Swift's case forced every `swift test` build to
compile the benchmark executable.

Binding conformance and packaging tests remain part of the product boundary;
they must parse through the same one-shot semantics rather than reproduce C
parser behavior independently.

## 9. Change discipline

Future changes must begin from this model. A new syntax construct belongs in
the shared parse transaction and canonical AST. A new consumer operation
belongs outside the parser unless it is required to interpret source into that
AST. Performance work must improve the common algorithm without introducing a
second semantic path, cardinality switch, or allocation-failure fallback.

The architecture is intentionally small: one source, one parse transaction,
one immutable document model, and one terminal failure boundary.
