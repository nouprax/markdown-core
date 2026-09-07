package com.nouprax.markdown.core

/**
 * A footnote the document owns: a scoped value outside the markup union,
 * reached through [Document.footnotes] and never an element of any content
 * list. Repeated calls share one footnote, the first definition of an id
 * wins, and a valid definition nobody calls is still a footnote. The walk
 * reports it through [WalkingVisitor.visitFootnote] after the document's
 * content.
 */
public class Footnote internal constructor(
    /** The normalized label without the caret. */
    public val id: String,
    public val content: kotlin.collections.List<Markup>,
    public val scope: Scope,
)
