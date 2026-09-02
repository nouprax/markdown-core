package com.nouprax.markdown.core

/** The immutable semantic root returned by a parse. */
public class Document internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitDocument(this)

    public companion object {
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document = parsePlatformDocument(source.encodeToByteArray(), options)
    }
}
