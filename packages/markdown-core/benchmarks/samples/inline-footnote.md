The source stage[^source] and the AST stage[^ast] are measured separately,
and the ratio[^ratio] is reported against a pinned reference.

Repeated calls to [^source] and [^ast] resolve through the same map, so
the second call costs a lookup rather than a definition.

[^source]: The block phase, from the first line to the finished block tree.
[^ast]: Inline parsing, consolidation and the postprocess passes.
[^ratio]: Instructions per input byte, core over reference.
