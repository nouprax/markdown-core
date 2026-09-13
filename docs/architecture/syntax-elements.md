# Syntax element ownership

All syntax, including inherited CommonMark, belongs to an element-owned
implementation unit. Development milestones and upstream dialects do not define
implementation modules. One element owns all of its spellings, including
spellings shared by several dialects. The inventory also accounts for every
feature commit after `2d9dc333e2bbd7007e205c64a6e280333aef3584`.

## Element inventory

All implementation paths below are under `packages/markdown-core/elements/`.
Commit abbreviations identify the reviewed history, not implementation layers.

| Element or syntax | Implementation | Reviewed feature commits |
| --- | --- | --- |
| Document: source envelope, definition dependencies and resolution order | `document.c`, invoking the participating element services | Inherited and shared document grammar |
| Paragraph: default prose, reference-only cleanup and lazy continuation | `paragraph.c` | Inherited |
| Text: literals, entities, escapes and contextual escaped spaces | `text.c` | Inherited; script-space semantics from `0c68bd1d` |
| SoftBreak and LineBreak: authored line endings and hard breaks | `line_break.c`, with escaped breaks recognized by Text | Inherited |
| Emphasis and Strong: asterisk and underscore rules | `emphasis.c`, declaring both spellings to the shared delimiter engine | Inherited |
| Strikethrough: double-tilde delimiters | `strikethrough.c` | Inherited |
| Code: backtick spans and normalized literal bodies | `code.c` | Inherited |
| CodeBlock: indented/fenced openers, continuation, info and literal finalization | `code_block.c` | Inherited |
| HTML: inline tokens and block start/end conditions | `html.c`, `html_block.c`; Comment owns comment values | Inherited |
| ThematicBreak: marker runs and failed-suffix cache | `thematic_break.c` | Inherited |
| Autolink: angle-delimited URI/email and bare links | `autolink.c` | Inherited |
| Comment: HTML comments and `%%` inline/block comments | `comment.c` | `ab01af37`, `b7dc8daf` |
| Link: direct destinations, shared reference resources, reference definitions, attribute attachment | `link.c`, using `attributes.c` | `18602b2e`, `1c5c7a39`, `8d9177fa` |
| Embedded: image prefix, destinations and authored label dimensions | `embedded.c`, using Link's shared destination and bracket grammar | `18602b2e`, `1c5c7a39`, `9ab6dfee` |
| CrossLink and CrossEmbedded: `[[...]]` and `![[...]]` | `cross_link.c`, using the same `embedded.c` dimension parser | `4853f2ad`, `9ab6dfee` |
| Callout: quote container, variant/fold metadata and inline title | `callout.c` | `b6a11e50`, `d53b6f53` |
| Citation: bibliography groups, author forms, affixes and specimen references | `citation.c` | `078cf4cf`, `012cb4e1`, `5f5a516c` |
| Footnote: named definitions, named calls and `^[...]` bodies | `footnote.c`, using Citation's shared referent construction | `078cf4cf`, `fbe1d5e5` |
| Specimen: authored examples, labels, numbering and definition registry | `specimen.c` | `012cb4e1`, `5f5a516c` |
| List: authored marker facts, decimal/alpha/Roman and automatic numbering | `list.c` | `012cb4e1`, `5f5a516c` |
| Task marker: one Unicode marker at list-item creation | `tasklist.c`, called by `list.c` | `9f5e630c` |
| DefinitionList: terms, bodies, compactness, indentation and lookahead | `definition_list.c` | `5f5a516c` |
| Table: groups, columns, spans, pipe/grid/simple/multiline forms and captions | `table.c` | `798717b7`, `a540b69e` |
| Attributes: one brace grammar and attachment operations | `attributes.c` | `1ad40aed`, `8d9177fa` |
| Heading: ATX/setext openers, suffix ownership, generated anchors and implicit references | `heading.c` | Inherited; `25282384`, `8d9177fa` |
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
oracle dependency. Their constraints continue to apply to every element.
The evidence portions of `5f5a516c` and `a540b69e` likewise remain shared
validation, rather than new syntax modules.

## Parser boundary

`core/inlines.c` owns the cursor, source projection, token dispatch,
delimiter events, pairing, range reduction and inline-field continuation.
`core/blocks.c` owns the open-container spine, indentation advancement,
streaming and mapped inputs, lookahead, source marks and lifecycle dispatch.
Neither driver recognizes element spellings or reads element-specific AST
payloads. Canonical node storage, resource identity and owned-field traversal
remain shared model operations, independent of a syntax's spelling.

Elements own lexical rules, recognition results, element construction,
attachment and element resolution. `inline_internal.h` and `block_internal.h`
expose the shared parser services and the single `markdown_core_inline_state`
type, whose parameters are named `inline_state`; they do not
create another parser or transfer AST ownership. Citation, bracket and heading
state records live beside their grammar owners. Inline state resources are released
through their owners' disposal hooks, including on allocation failure.

The immutable registry projects node kinds to element structure descriptors separately
from scanner precedence. Structure follows a node's current kind; its existing
`element` pointer continues to own opaque payload and containment callbacks,
including across kind conversion. A containment policy cannot suppress the
underlying paragraph's parsing or Text's completion rules.

There are two kinds of integration, chosen by lifecycle:

- Scanner elements have immutable descriptors in the single
  `CORE_ELEMENTS` table. Emphasis, Strong, Mark, Insertion, Superscript,
  Subscript and Strikethrough use declared delimiter semantics. Basic block
  openers and newer container grammars use the same non-consuming recognition
  and committed-open contract, with explicit indentation bounds.
- Operations on an existing owner use explicit lifecycle services. Examples
  are a Span alternative at a shared bracket close, an attribute suffix on
  an already recognized owner, dimensions on a media label, a task marker
  when a list item opens, and heading resolution after definitions are known.
  A lifecycle-only descriptor does not pretend to scan source, and a service
  invoked by its existing owner does not rediscover syntax in a tree pass.

Block recognition returns a typed candidate and its committed-open callback.
The registry orders all openers; the engine has no reserved recognition slot
for inherited grammar. Streaming parsing and lookahead ask the same operation.
Definition terms have a fallback hook after ordinary elements and tables
decline. Table owns its dash-led interruption rule. Elements also declare
literal/prose content, continuation, blank-line propagation and closing hooks.
The same continuation and speculative-state contracts serve real input and
cached lookahead.

Inline descriptors declare protected-token, ordinary-alternative or literal-
fallback precedence. One ordered dispatch loop handles all three. Its byte index
is built once per parse, preserving candidate order and set membership without
walking unrelated descriptors for each token. Inline state lifecycle and block
alternative lists likewise include only participating descriptors, in registry
order. A successful
alternative may consume input without emitting a node, as bracket commitment
does. Ordinary elements, including test probes, still run before the literal
`!`, `[` and backslash fallbacks. Every text-terminating byte comes from a
descriptor; the engine has no built-in byte table.

## Shared algorithms and failure behavior

Parsed delimiters declare consumed widths, lexical run limit, body grammar,
ambiguity rule and resulting node kind. The engine projects these declarations
by rule and default byte once when attaching the dialect. Two syntax owners
cannot overwrite the same rule or default byte. Shared dispatch is distinct:
the double-tilde scanner selects Strikethrough before Subscript can select a
single tilde; Footnote claims `^[` before Superscript. All parsed delimiters,
including Emphasis and Strikethrough, use the same flanking classifier and
constructor. Underscore's punctuation-bound flanking is a rule property.

Citation's non-consuming text predicate is also projected by byte. If multiple
owners disagree on a predicate, the byte reaches ordinary element dispatch.
There is no input-size threshold or second text-scanning algorithm. The shared
text scanner retains its cached maximal delimiter run, so literal `=` and `+`
runs do not create unnecessary Text nodes or get rescanned at dispatch.

Link's shared bracket owner arbitrates explicit Link/Embedded tails, Span, citation
tails/groups, shortcut links and named footnotes. Each alternative consumes
the existing parsed range; none reparses bracket contents. Heading suspension,
field completion, ordinary whitespace boundaries and source positions use
the same services as ordinary inline parsing.

The parser owns descriptor lists and temporary indices; the AST owns nodes,
resources and independent field roots. Allocation failures remain sticky on
the one parse transaction. Disposal releases every descriptor projection even
if attachment failed partway through. Element code uses the same source
mapping, indentation and lookahead services, preserving their linear-work
invariants.

## Generated lexical rules

The handwritten grammar and its generated lexer share the same element owner.
Autolink, CodeBlock, Comment, Footnote, Formula, Heading, HTML, Link, Table and
Text each have an `*_scanners.re` source, private header and committed generated
`*_scanners.c` in `elements/`. Each scanner includes `scanner_common.re`, which
contains only shared byte-cursor configuration and whitespace, newline and
escape primitives. It contains no scanner functions, so including it cannot
import another element's implementation.

Each element exposes its scanners directly, accepting data, length and offset.
The entry rejects invalid or empty ranges before forming any pointers, then
recognizes only that borrowed slice without allocation, sentinel writes or
padding. The generated functions own this boundary; there is no shared scanner
wrapper, callback dispatch or forwarding macro. Table's cursor-based dash
scanner additionally reports matched spans for its geometry pass.

The generator retains each grammar's encoding, and the Makefile and
reproducibility check use the same pinned re2c command. Ordinary builds consume
committed C files.

## Verification

The C correctness suites cover source positions, overlapping syntax, OOM,
deep containers, shared delimiter work, source-order resolution and mapped
tables. The element inventory additionally rejects conflicting delimiter
projections and block scanners without an explicit indentation bound. The
parser-boundary audit rejects concrete element kinds, payload access, scanner
calls and spelling dispatch in either engine. The source-list
audit compares CMake, both Swift manifests, Android CMake and the ES/Wasm
build, so moving an implementation cannot leave a binding on an old source.

### Registry lifetime

Every parse borrows the same immutable, contiguous core descriptor table.
Hook presence on each descriptor is the authority for participation; there
are no separately owned block, inline, or lifecycle membership lists. The
byte dispatch index is the one runtime projection, stably ordered by inline
precedence within each byte. It is built once before inline parsing.
Private setup extensions acquire a contiguous snapshot before replacing the
borrowed table. They use the same readers and dispatch construction as the
fixed dialect; failed allocation preserves the previous registry. No global
initialization cache or lock is needed.
