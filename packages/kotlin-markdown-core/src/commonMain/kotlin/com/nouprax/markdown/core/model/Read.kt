package com.nouprax.markdown.core

/**
 * One read of the text, under two total views.
 *
 * [semantic] is the tree with policy applied, which may omit bytes; [concrete]
 * omits nothing. Every byte of the source is in exactly one region of the
 * concrete view and every region has exactly one owner in the tree, so the
 * pair is complete -- and it is CLOSED: every scope in [semantic] is counted
 * against [concrete] and nothing outside this value is needed to resolve one.
 *
 * A read is an immutable value the caller owns outright. It retains nothing
 * native, so it stays readable after every later feed and after the
 * [Document] that produced it is gone. A mid-stream read is the projection of
 * the parse as it stands; the read [Document.seal] returns is the whole text's.
 *
 * In C the two views are siblings on one `markdown_core_document` handle
 * (`markdown_core_document_semantic`, `markdown_core_document_source`); this
 * value is that handle's shape, copied out.
 */
public class Read internal constructor(
    public val semantic: Semantic,
    public val concrete: Concrete,
) {
    /** Returns the canonical diagnostic dump of [semantic]. */
    public fun dump(): String = semantic.dump()
}
