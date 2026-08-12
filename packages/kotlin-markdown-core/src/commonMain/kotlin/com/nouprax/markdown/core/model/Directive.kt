package com.nouprax.markdown.core

/**
 * An inline directive, `:name[label]{attributes}`.
 *
 * The directive grammar is this library's own rather than Markdown's, which
 * is why a `{...}` block that does not parse is the one thing a parse
 * reports; see [DiagnosticCode.DIRECTIVE_ATTRIBUTES].
 */
public class Directive internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    override val scope: Scope,
    /** Always [PlacementMode.EMBEDDED].
     *
     * [DirectiveBlock] is the standalone form. */
    public val mode: PlacementMode,
    /** The name as written, the marker colon excluded.
     *
     * Never empty — a colon with no name after it is text, not a
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
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)
}
