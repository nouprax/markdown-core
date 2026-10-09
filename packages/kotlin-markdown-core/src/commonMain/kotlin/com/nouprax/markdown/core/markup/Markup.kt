package com.nouprax.markdown.core

/**
 * Every node of the AST. A node is a value: two nodes are equal when they
 * have the same kind, [id], scalar fields, [extent], [runs] and pairwise
 * equal children in every relation, and a hash reads the [id] alone, so it is
 * consistent with that equality in O(1).
 */
public sealed class Markup {
    /** Unique within its document, and numbered from 1 by a parse in the order its nodes complete, document last. */
    public abstract val id: MarkupID

    /** Where the node is, relative to the node before it; [Document.scope] turns it into editor coordinates. */
    public abstract val extent: Extent

    /**
     * Its own source ranges, in source order, at least one; touching runs are
     * one run, and the source between two runs is not its own.
     */
    public abstract val runs: kotlin.collections.List<Run>
    public abstract val anchor: String?
    public abstract val attributes: Attributes

    /**
     * Deep value equality, the reference check first. The pairs still to
     * compare sit on an explicit work stack, so the call stack stays flat
     * however deep the trees are, and a pair that is one shared node is equal
     * without a look inside.
     */
    final override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is Markup) return false
        val work = ArrayList<Markup>()
        work += this
        work += other
        while (work.isNotEmpty()) {
            val right = work.removeAt(work.lastIndex)
            val left = work.removeAt(work.lastIndex)
            if (left === right) continue
            if (!left.sameFields(right)) return false
            val lefts = left.relations()
            val rights = right.relations()
            if (lefts.size != rights.size) return false
            for (index in lefts.indices) {
                val nodes = lefts[index].nodes
                val others = rights[index].nodes
                if (lefts[index].group != rights[index].group || nodes.size != others.size) return false
                for (position in nodes.indices) {
                    work += nodes[position]
                    work += others[position]
                }
            }
        }
        return true
    }

    final override fun hashCode(): Int = id.hashCode()

    /** The kind and the id only: a description never follows tree edges. */
    final override fun toString(): String = "${this::class.simpleName}(id=${id.value})"

    /**
     * The node's relations in canonical field order, the one table the walker,
     * the dumper, equality and scope queries share. An absent optional field
     * yields no relation; a group the dump always draws yields one even when it
     * is empty.
     */
    internal fun relations(): kotlin.collections.List<Relation> =
        when (this) {
            is Document -> {
                listOfNotNull(metadata?.let { Relation(null, listOf(it)) }, Relation(null, content))
            }

            is Callout -> {
                listOfNotNull(title?.let { Relation("Title", it) }, Relation(null, content))
            }

            is Table -> {
                listOfNotNull(
                    caption?.let { Relation(null, listOf(it)) },
                    Relation("TableHead", head),
                    Relation("TableBody", content, continues = true),
                    Relation("TableFoot", foot, continues = true),
                )
            }

            is DirectiveBlock -> {
                listOfNotNull(label?.let { Relation(null, listOf(it)) }, Relation(null, content))
            }

            is Directive -> {
                listOfNotNull(label?.let { Relation(null, listOf(it)) })
            }

            is Cite -> {
                listOf(Relation(null, citations))
            }

            is Citation -> {
                val note = ((referent as? CitationReferent.Footnote)?.target as? FootnoteTarget.Note)?.footnote
                listOfNotNull(
                    note?.let { Relation(null, listOf(it)) },
                    Relation("CitationPrefix", prefix),
                    Relation("CitationSuffix", suffix),
                )
            }

            is Definition -> {
                listOf(Relation("DefinitionTerm", term)) + content.map { Relation("DefinitionBody", it) }
            }

            is List -> {
                listOf(Relation(null, items))
            }

            is TableRow -> {
                listOf(Relation(null, cells))
            }

            is DefinitionList -> {
                listOf(Relation(null, definitions))
            }

            is Paragraph -> {
                listOf(Relation(null, content))
            }

            is Heading -> {
                listOf(Relation(null, content))
            }

            is ListItem -> {
                listOf(Relation(null, content))
            }

            is TableCaption -> {
                listOf(Relation(null, content))
            }

            is TableCell -> {
                listOf(Relation(null, content))
            }

            is DirectiveLabel -> {
                listOf(Relation(null, content))
            }

            is Emphasis -> {
                listOf(Relation(null, content))
            }

            is Strong -> {
                listOf(Relation(null, content))
            }

            is Strikethrough -> {
                listOf(Relation(null, content))
            }

            is Mark -> {
                listOf(Relation(null, content))
            }

            is Insertion -> {
                listOf(Relation(null, content))
            }

            is Span -> {
                listOf(Relation(null, content))
            }

            is Superscript -> {
                listOf(Relation(null, content))
            }

            is Subscript -> {
                listOf(Relation(null, content))
            }

            is Link -> {
                listOf(Relation(null, content))
            }

            is Embedded -> {
                listOf(Relation(null, content))
            }

            is Footnote -> {
                listOf(Relation(null, content))
            }

            is Specimen -> {
                listOf(Relation(null, content))
            }

            is ThematicBreak, is CodeBlock, is HTMLBlock, is FormulaBlock, is Text, is SoftBreak, is LineBreak, is Code,
            is HTML, is Comment, is CrossLink, is CrossEmbedded, is Formula, is Metadata, is Reference,
            -> {
                emptyList()
            }
        }

    /** The inherited fields and the kind's scalar fields; relations are compared by [equals]. */
    private fun sameFields(other: Markup): Boolean {
        if (id != other.id || extent != other.extent || runs != other.runs) return false
        if (anchor != other.anchor || attributes != other.attributes) return false
        return when (this) {
            // The unit only chooses how scopes are counted; it is not a field of the contract.
            is Document -> {
                other is Document
            }

            is Callout -> {
                other is Callout && variant == other.variant && collapsed == other.collapsed
            }

            is Paragraph -> {
                other is Paragraph
            }

            is Heading -> {
                other is Heading && level == other.level
            }

            is ThematicBreak -> {
                other is ThematicBreak
            }

            is List -> {
                other is List && flavor == other.flavor && start == other.start &&
                    variant == other.variant && delimiter == other.delimiter && tight == other.tight
            }

            is ListItem -> {
                other is ListItem && marker == other.marker
            }

            is CodeBlock -> {
                other is CodeBlock && info == other.info && language == other.language &&
                    literal == other.literal && fenced == other.fenced && closed == other.closed
            }

            is HTMLBlock -> {
                other is HTMLBlock && literal == other.literal
            }

            is FormulaBlock -> {
                other is FormulaBlock && literal == other.literal
            }

            is Table -> {
                other is Table && columns == other.columns
            }

            is TableCaption -> {
                other is TableCaption
            }

            is TableRow -> {
                other is TableRow
            }

            is TableCell -> {
                other is TableCell && rowspan == other.rowspan && colspan == other.colspan
            }

            is DirectiveBlock -> {
                other is DirectiveBlock && name == other.name
            }

            is DirectiveLabel -> {
                other is DirectiveLabel
            }

            is Text -> {
                other is Text && literal == other.literal
            }

            is SoftBreak -> {
                other is SoftBreak
            }

            is LineBreak -> {
                other is LineBreak
            }

            is Code -> {
                other is Code && literal == other.literal
            }

            is HTML -> {
                other is HTML && literal == other.literal
            }

            is Comment -> {
                other is Comment && literal == other.literal
            }

            is CrossLink -> {
                other is CrossLink && dest == other.dest && label == other.label
            }

            is CrossEmbedded -> {
                other is CrossEmbedded && dest == other.dest && label == other.label &&
                    dimensions == other.dimensions
            }

            is Formula -> {
                other is Formula && mode == other.mode && literal == other.literal
            }

            is Emphasis -> {
                other is Emphasis
            }

            is Strong -> {
                other is Strong
            }

            is Strikethrough -> {
                other is Strikethrough
            }

            is Mark -> {
                other is Mark
            }

            is Insertion -> {
                other is Insertion
            }

            is Span -> {
                other is Span
            }

            is Superscript -> {
                other is Superscript
            }

            is Subscript -> {
                other is Subscript
            }

            is DefinitionList -> {
                other is DefinitionList
            }

            is Definition -> {
                other is Definition && compact == other.compact
            }

            is Link -> {
                other is Link && dest == other.dest && title == other.title
            }

            is Embedded -> {
                other is Embedded && dest == other.dest && title == other.title &&
                    dimensions == other.dimensions
            }

            is Directive -> {
                other is Directive && name == other.name
            }

            is Cite -> {
                other is Cite
            }

            is Citation -> {
                other is Citation && sameReferent(referent, other.referent)
            }

            is Footnote -> {
                other is Footnote && label == other.label
            }

            is Specimen -> {
                other is Specimen && label == other.label && start == other.start
            }

            is Reference -> {
                other is Reference && label == other.label && dest == other.dest && title == other.title
            }

            is Metadata -> {
                other is Metadata && name == other.name && title == other.title &&
                    subtitle == other.subtitle && time == other.time && date == other.date &&
                    authors == other.authors && keywords == other.keywords &&
                    `abstract` == other.`abstract` && state == other.state && comment == other.comment
            }
        }
    }

    /** An inline note's footnote is a relation of its citation, so only the branch is compared here. */
    private fun sameReferent(
        left: CitationReferent,
        right: CitationReferent,
    ): Boolean =
        if (left is CitationReferent.Footnote && left.target is FootnoteTarget.Note) {
            right is CitationReferent.Footnote && right.target is FootnoteTarget.Note
        } else {
            left == right
        }
}
