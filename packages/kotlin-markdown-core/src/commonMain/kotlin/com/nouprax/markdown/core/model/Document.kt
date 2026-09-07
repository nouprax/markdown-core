package com.nouprax.markdown.core

/** The immutable semantic root returned by a parse. */
public class Document internal constructor(
    public val content: kotlin.collections.List<Markup>,
    /** The footnotes the document owns, ordered by scope start; never part of [content]. */
    public val footnotes: kotlin.collections.List<Footnote>,
    /** Specimen definitions, ordered by scope start and visited after footnotes. */
    public val specimens: kotlin.collections.List<Specimen>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDocument(this)

    public companion object {
        /**
         * Parses [source] as the one Markdown Core dialect. There is nothing to
         * configure: every feature is recognized on every call.
         */
        public fun parse(source: String): Document = parsePlatformDocument(source.encodeToByteArray())
    }
}
