package com.nouprax.markdown.core

/** An inline citation cluster owning one or more [Citation] nodes in source order. */
public class Cite internal constructor(
    /** Never empty: every cite is authored with at least one item. */
    public val citations: kotlin.collections.List<Citation>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()

/**
 * A Markup node owned by [Cite.citations], with inline prefix and suffix
 * content. A [FootnoteTarget.Note] referent owns its inline note's footnote.
 */
public class Citation internal constructor(
    public val referent: CitationReferent,
    /** The inline content before the referent, owned by the citation; empty for an inherited call. */
    public val prefix: kotlin.collections.List<Markup>,
    /** The inline content after the referent, owned by the citation; empty for an inherited call. */
    public val suffix: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()

/**
 * What a [Citation] names: a tagged value, so a branch's fields exist only in
 * that branch.
 */
public sealed interface CitationReferent {
    /** A bibliography key with its [mode]. */
    @ConsistentCopyVisibility
    public data class Bib internal constructor(
        public val key: String,
        public val mode: BibMode,
    ) : CitationReferent

    /** A footnote call: a label the document looks up, or the inline note the citation owns. */
    @ConsistentCopyVisibility
    public data class Footnote internal constructor(
        public val target: FootnoteTarget,
    ) : CitationReferent

    /** A specimen call: [Document.specimen] answers the first specimen whose label is [label]. */
    @ConsistentCopyVisibility
    public data class Specimen internal constructor(
        public val label: String,
    ) : CitationReferent
}

/** The footnote a [CitationReferent.Footnote] names. */
public sealed interface FootnoteTarget {
    /**
     * A referenced footnote: the normalized label without the caret, as
     * [com.nouprax.markdown.core.Footnote.label] states it. [Document.footnote]
     * answers the first footnote with that label.
     */
    @ConsistentCopyVisibility
    public data class Label internal constructor(
        public val value: String,
    ) : FootnoteTarget

    /**
     * An inline note: the citation owns its [footnote], which the walk visits
     * before the citation's prefix and suffix.
     */
    @ConsistentCopyVisibility
    public data class Note internal constructor(
        public val footnote: com.nouprax.markdown.core.Footnote,
    ) : FootnoteTarget
}
