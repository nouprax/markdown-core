# Block container ownership

Block parsing owns one open-node spine. List items and definition bodies use
one indentation operation: the marker consumes one to four padding columns,
or one column when the excess begins an indented block. Continuation removes
that stored column count, respecting partially consumed tabs. The remaining
line enters the ordinary block-start parser in place. No body is copied into
a second Markdown input and no subtree is reparsed.

A definition term can open only at paragraph fallback, after higher-priority
block starts decline. The shared lookahead carries the same container prefixes
without committing source, and checks one optional blank line and one marker.
A reference-shaped line uses the reference parser's non-registering recognition
operation. Once accepted, the one-line term owns an inline root, separate from
its ordered block-body roots. The shared owned-inline traversal reaches that
root in every phase, including reference resolution, explicit anchor reservation,
inline parsing and postprocessing. Destruction uses the same iterative ownership
walk. Public models expose only DefinitionList, Definition, the term array and
arrays of body content; private body roots never become public Markup.

Blank runs continue an open body only when the next line carries its indentation.
The decision occurs before closing descendants, so code, lists and the body's
scope all agree on the source boundary. The shared lookahead caches scanned
prefixes and blank runs; the body retains the next carried line number until
that run ends. This bounds repeated queries for nested bodies and long blank
runs by source work, without another parsing path. Adversarial tests count the
lookahead and definition work across long runs and 1024 nested bodies.

Directive attributes and the body stack are shared by named and nameless
containers. An empty stored name means an absent block name; inline directives
still require a valid nonempty name. Their opener grammars select that value,
then hand the body to the same container operation.

A directive closer is provisional while matching the open spine. A deeper
carried directive replaces the candidate, even when its own wider fence makes
the line ordinary content. A carried opaque leaf (code, HTML, comment or formula)
owns the line and clears the candidate. Only after the spine is matched or a
prefix fails does the selected container finalize its descendants on the previous
line and itself on the fence line. Pure lookahead applies that same decision,
so a candidate definition cannot claim a marker beyond its enclosing closer.
A fenced code block that loses its parent prefix has no closing fence of its
own; it ends on the preceding line, like every other interrupted block.

Definition scopes are completed after body descendants, as list layout already
is. An empty body ends at its marker; a populated body ends at its last block.
Terms and bodies preserve source order without duplicating source or ownership.
Allocation failure leaves every partially built root on this ownership graph,
so parser failure uses ordinary destruction. Strict OOM sweeps cover term roots,
body roots, nameless attributes, reference probes and nested continuation.
