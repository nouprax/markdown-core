# Inline delimiter ownership

The inline parser has one delimiter stack and one forward pairing algorithm.
`*`, `_`, `==`, `++`, `^`, single `~` and `~~` share parsed-body construction,
source placement, containment checks and failure handling. Formula pairs use
that matcher too; their hook decodes an opaque literal instead of transferring
parsed children. The hook cannot traverse or mutate the delimiter stack.

## Stack entries and lifetime

The private stack contains three source-ordered entry kinds:

| Entry | Meaning | Completion |
| --- | --- | --- |
| Marker | Borrowed Text, rule, width, opening/closing eligibility, extension owner | Paired by the shared matcher or left as authored text |
| Boundary | Ordinary raw whitespace has occurred at this source position | Advances the standard opener-search floor for word bodies |
| Field | A consumed token owns inline fields that must finish before the next token | Parses those fields once, then becomes a boundary or is removed |

Every entry uses the same allocation, linking and removal operations. Fields
borrow their token owner; the AST owns the field trees. A field event is always
the last entry when token scanning pauses. Completing it cannot change the
parent stack because each field has its own inline subject. No pending-token
pointer, script cursor boundary or per-marker boundary snapshot is retained.

A heading can suspend with a field event on its ordinary stack. Its label
contains live brackets, so it cannot declare an implicit reference; its
remaining content resumes after the shared reference map is ready. Completing
fields before scanning the next token preserves source order for references
and inline footnotes. See [heading resolution](heading-resolution.md).

## Rule grammar and pairing

The rule table declares minimum/maximum consumed width, lexical run limit,
rule-of-three ambiguity and body grammar. Inline bodies use inherited flanking;
word bodies use non-empty content without ordinary raw whitespace. The table
selects the same shared constructor for every parsed body. The maximal tilde
lexer retains its distinct spelling rule: one tilde is Subscript, exactly two
are Strikethrough, and longer runs are text. The inherited strikethrough
flanking classifier and `~` transparency remain unchanged.

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
inline-footnote ownership, including tail precedence and attribute attachment.
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
