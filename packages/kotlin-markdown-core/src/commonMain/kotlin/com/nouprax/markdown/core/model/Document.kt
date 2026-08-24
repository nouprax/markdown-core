package com.nouprax.markdown.core

/**
 * A parse, under two total views.
 *
 * The document IS the semantic view -- the tree with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are in
 * no literal anywhere. [concrete] omits nothing. Every byte of the source is in
 * exactly one region of the concrete view and every region has exactly one
 * owner in this tree, so the pair is complete.
 *
 * In C the two are siblings, because a `markdown_core_document` is a handle and
 * the root is a node it lends out. Here they are not: the handle is gone by the
 * time [parse] returns, the tree is a value, and the concrete view hangs off the
 * root it names into.
 */
public class Document internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
    public val concrete: Concrete,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDocument(this)

    public companion object {
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document = WireDecoder.decodeDocument(nativeParse(source.encodeToByteArray(), options))
    }
}
