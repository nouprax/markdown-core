package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads

/**
 * The immutable semantic root returned by a parse. Every footnote and
 * specimen stays in the tree where it was written; the document lists them
 * in source order and answers lookups by label.
 */
public class Document internal constructor(
    public val content: kotlin.collections.List<Markup>,
    public val metadata: Metadata?,
    /** How the document's scope queries count columns. */
    public val unit: TextUnit,
    /** Every footnote of the document, inline notes included, in source order. */
    public val footnotes: kotlin.collections.List<Footnote>,
    /** Every specimen of the document, in source order. */
    public val specimens: kotlin.collections.List<Specimen>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup() {
    private val footnoteLabels = firstByLabel(footnotes) { it.label }
    private val specimenLabels = firstByLabel(specimens) { it.label }

    /** The first definition of each label, in source order; a definition without a label is never found. */
    private fun <T : Markup> firstByLabel(
        definitions: kotlin.collections.List<T>,
        label: (T) -> String?,
    ): Map<String, T> {
        val labels = HashMap<String, T>()
        for (definition in definitions) {
            val key = label(definition) ?: continue
            if (key !in labels) labels[key] = definition
        }
        return labels
    }

    /** The first footnote in source order whose label is [label]; an inline note has none. */
    public fun footnote(label: String): Footnote? = footnoteLabels[label]

    /** The first specimen in source order whose label is [label]. */
    public fun specimen(label: String): Specimen? = specimenLabels[label]

    /**
     * The editor coordinates of [node], computed from the extents and
     * [source], the text this document was parsed from, with columns in the
     * document's [unit]. [node] is a node of this document.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source]
     *   ends before [node] does.
     */
    public fun scope(
        node: Markup,
        source: String,
    ): Scope {
        val place = place(node)
        val lines = SourceLines(source)
        if (place.end > lines.bytes.size) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
        return lines.scope(place.start.toInt(), place.end.toInt(), unit)
    }

    /**
     * The last node in canonical walk order whose source range holds the
     * scalar that starts at [position] of [source], the text this document was
     * parsed from, with the column in the document's [unit]. Null when [source]
     * has no scalar there or no node holds it.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when the line or
     *   the column of [position] is below 1.
     */
    public fun node(
        position: Position,
        source: String,
    ): Markup? {
        if (position.line < 1 || position.column < 1) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
        val offset = SourceLines(source).offset(position, unit) ?: return null
        var found: Markup? = null
        val traversal = MarkupTraversal(this, 0)
        while (traversal.next()) {
            if (traversal.step == MarkupTraversal.Step.ENTER && traversal.start <= offset && offset < traversal.end) {
                found = traversal.node
            }
        }
        return found
    }

    /**
     * The absolute byte range of [target], a node of this document, found by
     * one canonical walk. A node is found by reference.
     */
    internal fun place(target: Markup): Place {
        val traversal = MarkupTraversal(this, 0)
        while (traversal.next() && !(traversal.step == MarkupTraversal.Step.ENTER && traversal.node === target)) {
            continue
        }
        return Place(traversal.start, traversal.end)
    }

    /**
     * The canonical debug dump of this document, with scopes computed from [source] in UTF-8 columns.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source] ends before this document does.
     */
    public fun dump(source: String): String = MarkupDumper.dump(this, this, source)

    /**
     * The canonical debug dump of [node], a node of this document, with scopes computed from [source].
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source] ends before [node] does.
     */
    public fun dump(
        node: Markup,
        source: String,
    ): String = MarkupDumper.dump(this, node, source)

    public companion object {
        /**
         * Parses [source] as the one Markdown Core dialect. There is nothing to
         * configure: every feature is recognized on every call. [unit] is how
         * the document's scope queries count columns.
         *
         * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the
         *   engine cannot allocate, or [source] exceeds 1 GiB of UTF-8 or its
         *   tree a byte array's capacity.
         */
        @JvmOverloads
        public fun parse(
            source: String,
            unit: TextUnit = TextUnit.UTF16,
        ): Document = parsePlatformDocument(source.encodeToByteArray(), unit)
    }
}
