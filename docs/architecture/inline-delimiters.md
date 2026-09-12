# Inline delimiter ownership

The inline parser has one delimiter stack and one forward pairing algorithm.
`*`, `_`, `==`, `++`, `^`, single `~` and `~~` share parsed-body construction,
source placement, containment checks and failure handling. Formula pairs use
that matcher too; their hook decodes an opaque literal instead of transferring
parsed children. The hook cannot traverse or mutate the delimiter stack.

## Stack entries and lifetime

The private stack contains five source-ordered entry kinds:

| Entry | Meaning | Completion |
| --- | --- | --- |
| Marker | Borrowed Text, rule, width, opening/closing eligibility, extension owner | Paired by the shared matcher or left as authored text |
| Boundary | Ordinary raw whitespace has occurred at this source position | Advances the standard opener-search floor for word bodies |
| Citation token | A raw key or semicolon awaiting its bracket owner, or a suspended range endpoint | Becomes an affix boundary or is removed when ownership is decided |
| Affix boundary | A committed item splits prefix, key and suffix inline fields | Advances every rule's opener-search floor |
| Field | A consumed token owns inline fields that must finish before the next token | Parses those fields once, then becomes a boundary or is removed |

Every entry uses the same allocation, linking and removal operations. Fields
borrow their token owner; the AST owns the field trees. A field event is always
the last entry when token scanning pauses. Completing it cannot change the
parent stack because each field has its own inline subject. No script cursor boundary or per-marker boundary snapshot is retained.

A heading can suspend with a field event on its ordinary stack. Its label
contains live brackets, so it cannot declare an implicit reference; its
remaining content resumes after the shared reference map is ready. Completing
fields before scanning the next token preserves source order for references
and inline footnotes. See [heading resolution](heading-resolution.md).

## Rule grammar and pairing

Each element's extension descriptor declares minimum/maximum consumed width,
lexical run limit, rule-of-three ambiguity and body grammar. The engine retains
the two inherited emphasis rules and projects attached declarations by rule.
See [syntax extension ownership](syntax-extensions.md) for the element inventory
and parser boundary. Inline bodies use inherited flanking;
word bodies use non-empty content without ordinary raw whitespace. The table
selects the same shared constructor for every parsed body. The maximal tilde
lexer retains its distinct spelling rule: one tilde is Subscript, exactly two
are Strikethrough, and longer runs are text. The inherited strikethrough
flanking semantics and `~` transparency remain unchanged; Strikethrough now
uses the same classifier and constructor as the other parsed delimiters.

The matcher walks forward and searches backward using the existing
`openers_bottom[length % 3][rule]` memo. A boundary advances those same memo
slots for word-body rules. No script-specific search or second matching pass
exists. Empty word pairs consume both markers as text. An unmatched marker
retains its authored Text, so later valid pairs remain available.

Ordinary text is classified from disjoint UTF-8 source slices. Newline tokens
also emit boundaries. Escaped ASCII spaces, decoded entities and opaque token
bodies do not. An owned field reports whether its own stack retained a boundary;
its source is not rescanned by the enclosing parser.

## Range reduction and scope

Pairing a parsed container and closing a bracket scope both reduce a stack
range through one operation: remove its unresolved markers and retain only its
last boundary. The AST's bracket grammar still determines Link, Media, Span and
inline-footnote and bibliography ownership, including tail precedence and attribute attachment.
Those constructs are bracket scopes, not interchangeable emphasis markers.

Retaining the boundary is necessary for inputs such as `^a[**b c**]{}z^`:
the Strong and Span are valid, but their raw space prevents the enclosing
Superscript. Retaining *all* boundaries would repeatedly scan them at every
nested Span. A completed range therefore exports exactly one boundary summary.
For `^a[**b&#32;c**]{}z^`, no raw boundary exists and Superscript can form.

During range reduction each removed entry is charged once; a retained summary
is charged to the pair or bracket scope that reduces it. Adjacent source boundaries coalesce.
With a fixed rule inventory, this adds O(tokens + closed scopes) work and
O(tokens) storage to the shared matcher, including deeply nested brackets.
A prose-only inline buffer retains at most one boundary, rather than allocating
one entry per word. Allocation tests compare identical whitespace layouts;
doubling tests count matching and reduction work through 8192 units.

## Final content and failure

The shared constructor checks the parent's containment policy and allocates
before changing marker Text or child ownership. A rejected container leaves
its authored text intact. Bracket alternatives perform the same parent-policy
check before child transfer or suffix consumption. Allocation failure sets the
existing transaction's sticky OOM state. The engine owns range and endpoint cleanup even when an
opaque constructor fails; disposal also clears suspended heading stacks.

A `\ ` token remains a flagged Text until the final ownership walk. Only then
can its enclosing completed script be known: a later whitespace boundary may
invalidate an outer candidate, and bracket reduction may discard a temporary
node. That existing walk decodes flagged escapes, traverses owned fields with
inherited script context, and reserves final anchors. Document-owned footnotes
start an independent context. This is content completion, not delimiter
recognition or reparsing Text values for syntax.

## Citation ranges and affixes

A key and a semicolon are raw source tokens until the enclosing bracket chooses
its owner. Direct links, resolving reference tails and attribute-bearing Spans
complete those keys as ordinary inlines. A complete citation group instead
marks its first key and separators as affix boundaries, then runs the same
bounded delimiter reduction. Failed groups preserve unclaimed tokens as Text.
No citation item is constructed by reparsing a Text node or a copied body.

An author key's following bracket can depend on an outer bracket's decision:
`[@a [x]]` gives a normal item whose suffix contains `[x]`, whereas
`[@a [x]](u)` gives a Link containing an author item with suffix `x`.
The inner bracket suspends as a parser-owned continuation with source position,
raw closing Text and a delimiter endpoint. Its already parsed nodes remain
owned by the live AST. Once the outer owner is known, an explicit postorder
stack resumes that same bracket procedure over its bounded range. Each
continuation resolves once; sibling and nested keys share this operation.
Allocation failure frees continuations independently of the AST they borrow.

Each populated affix owns a private inline root, exposed through the public
Citation's prefix/suffix collections. Source trimming only changes raw edge
whitespace; nested markup keeps its authored scope. Completion, consolidation,
validation and extension postprocessing traverse all owned inline roots using
one explicit stack. Field order and inherited script depth are retained, and a
phase may replace its root only after its nested fields finish. Definition
families start independent contexts. Disposal splices the same owned roots into
the existing iterative node release path.

Bare keys and balanced braced keys use a single lexical operation. Braced
candidates share a lazy source index with the ordinary code and HTML token
scanners, so malformed nested candidates cannot repeatedly scan suffixes.
Token scans, range reductions and continuation resolution are linear in source
bytes plus emitted nodes. Doubling tests include successful and failed nested
keys, long braced keys, semicolon groups and author-tail chains through 8192
levels; allocation sweeps include pending and finalized affix owners.
