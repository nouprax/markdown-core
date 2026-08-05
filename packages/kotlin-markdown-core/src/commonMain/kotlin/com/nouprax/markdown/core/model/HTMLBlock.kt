package com.nouprax.markdown.core

public class HTMLBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val literal: String,
) : Markup {
    /** True when the literal is one complete comment, so consumers without an
     * HTML parser can skip comment material by this bit alone. */
    public val comment: Boolean get() = htmlLiteralIsComment(literal)

    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}

/** The canonical one-complete-comment rule shared by [HTMLBlock] and [HTML]:
 * after surrounding whitespace the literal opens with `<!--` and its first
 * `-->` is the terminal bytes. Comment-prefixed HTML with a same-line tail is
 * not a comment. Derived purely from the literal, matching the C facade's
 * markdown_core_node_html_comment. */
internal fun htmlLiteralIsComment(literal: String): Boolean {
    var end = literal.length
    while (end > 0 && (literal[end - 1] == '\n' || literal[end - 1] == ' ' || literal[end - 1] == '\t')) end--
    var start = 0
    while (start < end && (literal[start] == ' ' || literal[start] == '\t')) start++
    if (end - start < 4 || !literal.startsWith("<!--", start)) return false
    var index = start + 1
    while (index + 3 <= end) {
        if (literal[index] == '-' && literal[index + 1] == '-' && literal[index + 2] == '>') {
            return index + 3 == end
        }
        index++
    }
    return false
}
