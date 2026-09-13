package com.nouprax.markdown.core

/** An inline citation cluster owning one or more [Citation] nodes in source order. */
public class Cite internal constructor(
    /** Never empty: every cite is authored with at least one item. */
    public val citations: kotlin.collections.List<Citation>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup

/** A Markup node owned by [Cite.citations], with inline prefix and suffix content. */
public class Citation internal constructor(
    public val referent: CitationReferent,
    /** The inline content before the referent, owned by the citation; empty for an inherited call. */
    public val prefix: kotlin.collections.List<Markup>,
    /** The inline content after the referent, owned by the citation; empty for an inherited call. */
    public val suffix: kotlin.collections.List<Markup>,
    override val scope: Scope,
    override val anchor: String? = null,
    override val attributes: Attributes = Attributes.empty,
) : Markup

/**
 * What a [Citation] names: a tagged value, so a branch's fields exist only in
 * that branch.
 */
public sealed interface CitationReferent {
    /** A bibliography key with its [mode]. */
    public class Bib internal constructor(
        public val key: String,
        public val mode: BibMode,
    ) : CitationReferent

    /** A footnote named by [id]: the normalized label without the caret, as [com.nouprax.markdown.core.Footnote.id] states it. */
    public class Footnote internal constructor(
        public val id: String,
    ) : CitationReferent

    /** A specimen definition named by its authored [id]; first produced with P9b. */
    public class Specimen internal constructor(
        public val id: String,
    ) : CitationReferent
}
