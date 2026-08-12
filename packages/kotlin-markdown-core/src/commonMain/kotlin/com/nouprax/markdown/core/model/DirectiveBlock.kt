package com.nouprax.markdown.core

/**
 * A directive on its own line, in either written form.
 *
 * - The leaf `::name`, which never has a body.
 * - The container `:::name`, closed by a run of at least as many colons.
 *
 * Both project as this class, and an empty body is an empty [content] in
 * either, so the two are not told apart here.
 *
 * The directive grammar is this library's own rather than Markdown's, which
 * is why a `{...}` block that does not parse is the one thing a parse
 * reports; see [DiagnosticCode.DIRECTIVE_ATTRIBUTES].
 */
public class DirectiveBlock internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Always [PlacementMode.STANDALONE].
     *
     * [Directive] is the embedded form. */
    public val mode: PlacementMode,
    /** The name as written, the marker colons excluded.
     *
     * Never empty — a colon run with no name after it is text, not a
     * directive. */
    public val name: String,
    /** The directive's attribute map in source order, or null when no
     * `{...}` container was written.
     *
     * An empty container is an empty map. */
    public val attributes: Map<String, String>?,
    /** Null when no `[...]` was written.
     *
     * An explicit empty `[]` is a [DirectiveLabel] whose content is
     * empty. */
    public val label: DirectiveLabel?,
    /** The block content between the opening and closing colon runs, in
     * source order.
     *
     * [label] is not among it, and a leaf `::name` has none. */
    public val content: kotlin.collections.List<Markup>,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
