package com.nouprax.markdown.core

/**
 * An inline citation: one or more [citations] in authored order. An inherited
 * `[^label]` call is a one-item cite naming its footnote; the citation
 * syntaxes module, which fills in [CitationReferent.Bib] items, lands with
 * `P7`. Its items are scoped values, not content, so a cite is a leaf.
 */
public class Cite internal constructor(
    /** Never empty: every cite is authored with at least one item. */
    public val citations: kotlin.collections.List<Citation>,
    override val scope: Scope,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visitCite(this)
}

/**
 * One item of a [Cite]: a scoped value the cite owns, outside the markup
 * union. It has no visitor entry; the walk reports it through
 * [WalkingVisitor.visitCitation] between the cite's entering and exiting.
 */
public class Citation internal constructor(
    public val referent: CitationReferent,
    /** The inline content before the referent, owned by the citation; empty for an inherited call. */
    public val prefix: kotlin.collections.List<Markup>,
    /** The inline content after the referent, owned by the citation; empty for an inherited call. */
    public val suffix: kotlin.collections.List<Markup>,
    public val scope: Scope,
)

/**
 * What a [Citation] names: a tagged value, so a branch's fields exist only in
 * that branch.
 */
public sealed interface CitationReferent {
    /** A bibliography key with its [mode]. No parse produces this branch until `P7`. */
    public class Bib internal constructor(
        public val key: String,
        public val mode: BibMode,
    ) : CitationReferent

    /** A footnote named by [id]: the normalized label without the caret, as [com.nouprax.markdown.core.Footnote.id] states it. */
    public class Footnote internal constructor(
        public val id: String,
    ) : CitationReferent
}
