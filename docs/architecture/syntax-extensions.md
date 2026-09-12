# Syntax extension ownership

The changes after `2d9dc333e2bbd7007e205c64a6e280333aef3584` are
organized by the syntax element they produce. Development milestones and
upstream dialects do not define implementation modules. One element owns all
of its spellings, including spellings shared by several dialects.

## Element inventory

All implementation paths below are under `packages/markdown-core/extensions/`.
Commit abbreviations identify the reviewed history, not implementation layers.

| Element or syntax | Implementation | Reviewed feature commits |
| --- | --- | --- |
| Comment: HTML comments and `%%` inline/block comments | `comment.c` | `ab01af37`, `b7dc8daf` |
| Link: direct destinations, shared reference resources, reference definitions, attribute attachment | `link.c`, using `attributes.c` | `18602b2e`, `1c5c7a39`, `8d9177fa` |
| Media: image destinations and authored label dimensions | `link.c` for the shared link grammar; `media.c` for dimensions | `18602b2e`, `1c5c7a39`, `9ab6dfee` |
| CrossLink and CrossEmbedded: `[[...]]` and `![[...]]` | `cross_link.c`, using the same `media.c` dimension parser | `4853f2ad`, `9ab6dfee` |
| Callout: quote container, variant/fold metadata and inline title | `callout.c` | `b6a11e50`, `d53b6f53` |
| Citation: bibliography groups, author forms, affixes and specimen references | `citation.c` | `078cf4cf`, `012cb4e1`, `5f5a516c` |
| Footnote: named definitions, named calls and `^[...]` bodies | `footnote.c`, using Citation's shared referent construction | `078cf4cf`, `fbe1d5e5` |
| Specimen: authored examples, labels, numbering and definition registry | `specimen.c` | `012cb4e1`, `5f5a516c` |
| List: authored marker facts, decimal/alpha/Roman and automatic numbering | `list.c` | `012cb4e1`, `5f5a516c` |
| Task marker: one Unicode marker at list-item creation | `tasklist.c`, called by `list.c` | `9f5e630c` |
| DefinitionList: terms, bodies, compactness, indentation and lookahead | `definition_list.c` | `5f5a516c` |
| Table: groups, columns, spans, pipe/grid/simple/multiline forms and captions | `table.c` | `798717b7`, `a540b69e` |
| Attributes: one brace grammar and attachment operations | `attributes.c` | `1ad40aed`, `8d9177fa` |
| Heading: explicit suffix ownership, generated anchors and implicit heading references | `heading.c` | `25282384`, `8d9177fa` |
| Block identifier: `#anchor-id#` suffix and separate-line attachment | `block_identifier.c`, writing the shared anchor value | `d6abfbd0` |
| Properties: document metadata envelope, fixed fields and literal prose | `properties.c` | `2640f0da` |
| Mark: `==...==` | `mark.c` | `8a2b1462` |
| Insertion: `++...++` | `insertion.c` | `d4aca4cf` |
| Span: bracketed content with attributes | `span.c` | `0c68bd1d` |
| Superscript: `^...^` word body | `superscript.c` | `0c68bd1d` |
| Subscript: single-tilde word body | `subscript.c` | `0c68bd1d` |
| DirectiveBlock: named and nameless fenced containers | `directive.c` | `1ad40aed`, `5f5a516c` |
| Formula: non-nesting opaque pairs | `formula.c` | `ab01af37` |

The other six commits introduce no separate syntax element: `ee8200fc`
updates the specification; `1d9d3bde` removes parse options; `9547dc63`
changes node storage; `d4128456` completes conformance evidence; `2419bc44`
unifies delimiter events and ownership; `d0f8cdd5` updates the insertion
oracle dependency. Their constraints continue to apply to every extension.
The evidence portions of `5f5a516c` and `a540b69e` likewise remain shared
validation, rather than new syntax modules.

## Parser boundary

`core/inlines.c` owns the cursor, text and source projection, bracket stack,
delimiter events, pairing, range reduction and inline-field continuation.
`core/blocks.c` owns the open-container spine, indentation advancement,
streaming and mapped inputs, lookahead, source marks and document-phase order.
Inherited CommonMark leaf recognition remains in the engine. Canonical node
storage, resource identity and owned-field traversal remain shared model
operations, independent of a syntax's spelling.

Extensions own lexical rules, recognition results, element construction,
attachment and element resolution. `inline_internal.h` and `block_internal.h`
expose the shared parser services and typed borrowed cursors; they do not
create another parser or transfer AST ownership. Citation's token records are
declared in `citation_state.h`, alongside their implementation owner.

There are two kinds of integration, chosen by lifecycle:

- Scanner extensions have immutable descriptors in the single
  `CORE_EXTENSIONS` table. Mark, Insertion, Superscript, Subscript and
  Strikethrough declare their delimiter semantics there. Citation and
  Footnote own their inline prefixes. Callout, List, Footnote, Specimen and
  DefinitionList register non-consuming block recognition.
- Operations on an existing owner use explicit lifecycle services. Examples
  are a Span alternative at a shared bracket close, an attribute suffix on
  an already recognized owner, dimensions on a media label, a task marker
  when a list item opens, and heading resolution after definitions are known.
  These operations do not acquire fake standalone scanner descriptors or
  postprocess the tree to rediscover syntax.

Block recognition returns a typed candidate and its committed-open callback.
Container prefixes precede leaf recognition; definition/list markers follow
the inherited heading, fence, HTML, setext and thematic-break rules. These are
grammar precedence boundaries, independent of project history. Streaming
parsing and lookahead ask the same recognition operation. Definition terms
have a paragraph-fallback hook after ordinary extensions and tables decline.

## Shared algorithms and failure behavior

Parsed delimiters declare consumed widths, lexical run limit, body grammar,
ambiguity rule and resulting node kind. The engine projects these declarations
by rule and default byte once when attaching the dialect. Two syntax owners
cannot overwrite the same rule or default byte. Shared dispatch is distinct:
the double-tilde scanner selects Strikethrough before Subscript can select a
single tilde; Footnote claims `^[` before Superscript. All parsed delimiters,
including Strikethrough, use the same flanking classifier and constructor.

Citation's non-consuming text predicate is also projected by byte. If multiple
owners disagree on a predicate, the byte reaches ordinary extension dispatch.
There is no input-size threshold or second text-scanning algorithm. The shared
text scanner retains its cached maximal delimiter run, so literal `=` and `+`
runs do not create unnecessary Text nodes or get rescanned at dispatch.

The shared bracket owner arbitrates explicit Link/Media tails, Span, citation
tails/groups, shortcut links and named footnotes. Each alternative consumes
the existing parsed range; none reparses bracket contents. Heading suspension,
field completion, ordinary whitespace boundaries and source positions use
the same services as ordinary inline parsing.

The parser owns descriptor lists and temporary indices; the AST owns nodes,
resources and independent field roots. Allocation failures remain sticky on
the one parse transaction. Disposal releases every descriptor projection even
if attachment failed partway through. Extension code uses the same source
mapping, indentation and lookahead services, preserving their linear-work
invariants.

## Verification

The C correctness suites cover source positions, overlapping syntax, OOM,
deep containers, shared delimiter work, source-order resolution and mapped
tables. The extension inventory additionally rejects conflicting delimiter
projections and block scanners without an explicit precedence. The source-list
audit compares CMake, both Swift manifests, Android CMake and the ES/Wasm
build, so moving an implementation cannot leave a binding on an old source.
