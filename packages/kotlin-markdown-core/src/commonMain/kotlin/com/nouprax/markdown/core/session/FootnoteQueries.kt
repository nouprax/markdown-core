package com.nouprax.markdown.core

/**
 * Session-answered footnote presentation state for one node.
 *
 * The tree is source-faithful: definitions stay at their source position
 * whether referenced or not, and references always carry their label.
 * Numbering, first-use order, resolution state, and back-reference ordinals
 * are queries against the session's committed revision. When a commit
 * changes only these answers, the affected nodes are reported `changed`
 * with a revision bump and identical dump content.
 */
public class FootnoteInfo internal constructor(
    /** The label's winning definition (for a definition: its own id unless
     * an earlier definition shadows it); null while the label is
     * unresolved. */
    public val definition: MarkupID?,
    /** The label's 1-based first-use ordinal; null while the label is
     * unresolved or unreferenced. */
    public val number: Int?,
    /** For a reference: its 1-based position among the label's references
     * in document order. null for definitions. */
    public val referenceOrdinal: Int?,
    /** How many references share the label. */
    public val referenceCount: Int,
) {
    override fun equals(other: Any?): Boolean =
        other is FootnoteInfo &&
            other.definition == definition &&
            other.number == number &&
            other.referenceOrdinal == referenceOrdinal &&
            other.referenceCount == referenceCount

    override fun hashCode(): Int {
        var result = definition.hashCode()
        result = 31 * result + number.hashCode()
        result = 31 * result + referenceOrdinal.hashCode()
        result = 31 * result + referenceCount
        return result
    }
}

/**
 * Answers for the footnote reference or definition with [id] at the
 * committed revision; null when [id] does not name a footnote node of this
 * session.
 */
public fun MarkupSession.footnote(id: MarkupID): FootnoteInfo? {
    if (id.lineage != lineage) {
        return null
    }
    requireOpen()
    return WireDecoder.decodeFootnoteInfo(native.footnoteInfo(id.rawValue), lineage)
}

/**
 * The referenced (winning) definitions in first-use order — the order a
 * renderer lists them in.
 */
public fun MarkupSession.footnotes(): kotlin.collections.List<FootnoteDefinition> {
    requireOpen()
    return WireDecoder.decodeIds(native.footnotes()).immutableMap { raw ->
        val definition = node(MarkupID(lineage, raw))
        check(definition is FootnoteDefinition) { "footnote index names a non-definition node" }
        definition
    }
}

/**
 * The references that resolve to [definition], in document order — the
 * renderer's back-reference targets. Empty unless [definition] is a
 * referenced winning definition of this session.
 */
public fun MarkupSession.references(definition: MarkupID): kotlin.collections.List<FootnoteReference> {
    if (definition.lineage != lineage) {
        return emptyList()
    }
    requireOpen()
    return WireDecoder.decodeIds(native.footnoteReferences(definition.rawValue)).immutableMap { raw ->
        val reference = node(MarkupID(lineage, raw))
        check(reference is FootnoteReference) { "footnote index names a non-reference node" }
        reference
    }
}
