package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads

/**
 * The immutable semantic root returned by a parse. Every footnote, specimen
 * and reference stays in the tree where it was written; the document lists
 * them in source order and answers lookups by label.
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
    /** Every link reference definition of the document, in source order. */
    public val references: kotlin.collections.List<Reference>,
    /** Each normalized label a reference occurrence resolves, with the node it resolves to. */
    private val labels: Map<String, Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
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
     * The node a [Destination.Reference] naming [label], a normalized label,
     * resolves to: the first [Reference] in source order whose label is
     * [label], or, when none is, the first [Heading] in source order whose
     * text declares it; null when neither does.
     */
    public fun reference(label: String): Markup? = labels[label]

    /**
     * The editor coordinates of [node]'s source ranges, one scope per run in
     * source order, computed from the runs and [source], the text this
     * document was parsed from, with columns in the document's [unit]. [node]
     * is a node of this document, found by reference.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source]
     *   ends before [node] does.
     */
    public fun scope(
        node: Markup,
        source: String,
    ): kotlin.collections.List<Scope> {
        val traversal = MarkupTraversal(this)
        while (traversal.next() && !(traversal.step == MarkupTraversal.Step.ENTER && traversal.node === node)) {
            continue
        }
        val places = traversal.places()
        val lines = SourceLines(source)
        if (places.end(places.count - 1) > lines.bytes.size) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
        return kotlin.collections.List(places.count) {
            lines.scope(places.start(it).toInt(), places.end(it).toInt(), unit)
        }
    }

    /**
     * The last node in canonical walk order one of whose source ranges holds
     * the scalar that starts at [position] of [source], the text this document
     * was parsed from, with the column in the document's [unit]. Null when
     * [source] has no scalar there or no node holds it.
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
        val traversal = MarkupTraversal(this)
        while (traversal.next()) {
            if (traversal.step != MarkupTraversal.Step.ENTER) continue
            val places = traversal.places()
            for (index in 0 until places.count) {
                if (places.start(index) <= offset && offset < places.end(index)) {
                    found = traversal.node
                    break
                }
            }
        }
        return found
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
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source] ends before a node of its tree does.
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
