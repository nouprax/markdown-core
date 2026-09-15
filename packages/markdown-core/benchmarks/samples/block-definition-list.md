Source stage
: The block phase, from the first line to the finished block tree.

AST stage
: Inline parsing, consolidation and the postprocess passes.
~ Also the stage the tree walks belong to.

*Ratio* `Ir/B`
: Instructions per input byte, core over the pinned reference.

Corpus digest

: A hash over every document measured, content and all.

  A moved digest means the corpus changed, so two reports are not
  comparable until the cases that moved are named.

Toolchain
: The compiler, the C library, and the flags both engines were built with.
