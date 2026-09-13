package com.nouprax.markdown.core

/** Exhaustive callbacks overloaded by concrete markup type; [Markup.accept] selects the overload. */
public interface Visitor<Result> {
    public fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Result

    public fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): Result
}

/** The phase supplied to a markup visit. */
public enum class MarkupVisitPhase {
    /** During a walk, the node is reached before its owned markup. */
    ENTERING,

    /** During a walk, all of the node's owned markup has been visited. */
    EXITING,
}

/**
 * Walks owned markup depth first, reporting both phases through the same visitor.
 * The explicit stack keeps call-stack depth independent of document depth.
 * Only a visitor without a result can be used for automatic traversal.
 */
public fun Markup.walk(visitor: Visitor<Unit>) {
    val actions = mutableListOf(WalkAction(this, MarkupVisitPhase.ENTERING))
    while (actions.isNotEmpty()) {
        val action = actions.removeAt(actions.lastIndex)
        action.node.accept(visitor, action.phase)
        if (action.phase == MarkupVisitPhase.EXITING) continue
        actions += WalkAction(action.node, MarkupVisitPhase.EXITING)
        scheduleOwnedMarkup(action.node, actions)
    }
}

private data class WalkAction(
    val node: Markup,
    val phase: MarkupVisitPhase,
)

// Typed ownership fields determine traversal order; no flattened children projection is built.
private fun scheduleOwnedMarkup(
    node: Markup,
    actions: MutableList<WalkAction>,
) {
    fun schedule(nodes: kotlin.collections.List<Markup>) {
        for (index in nodes.indices.reversed()) actions += WalkAction(nodes[index], MarkupVisitPhase.ENTERING)
    }
    when (node) {
        is Document -> {
            schedule(node.specimens)
            schedule(node.footnotes)
            schedule(node.content)
            node.metadata?.let { actions += WalkAction(it, MarkupVisitPhase.ENTERING) }
        }

        is Callout -> {
            schedule(node.content)
            node.title?.let(::schedule)
        }

        is Paragraph -> {
            schedule(node.content)
        }

        is Heading -> {
            schedule(node.content)
        }

        is List -> {
            schedule(node.items)
        }

        is ListItem -> {
            schedule(node.content)
        }

        is Table -> {
            schedule(node.foot)
            schedule(node.content)
            schedule(node.head)
            node.caption?.let { actions += WalkAction(it, MarkupVisitPhase.ENTERING) }
        }

        is TableCaption -> {
            schedule(node.content)
        }

        is TableRow -> {
            schedule(node.cells)
        }

        is TableCell -> {
            schedule(node.content)
        }

        is DirectiveBlock -> {
            schedule(node.content)
            node.label?.let { actions += WalkAction(it, MarkupVisitPhase.ENTERING) }
        }

        is DirectiveLabel -> {
            schedule(node.content)
        }

        is Emphasis -> {
            schedule(node.content)
        }

        is Strong -> {
            schedule(node.content)
        }

        is Strikethrough -> {
            schedule(node.content)
        }

        is Mark -> {
            schedule(node.content)
        }

        is Insertion -> {
            schedule(node.content)
        }

        is Span -> {
            schedule(node.content)
        }

        is Superscript -> {
            schedule(node.content)
        }

        is Subscript -> {
            schedule(node.content)
        }

        is Link -> {
            schedule(node.content)
        }

        is Embedded -> {
            schedule(node.content)
        }

        is Directive -> {
            node.label?.let { actions += WalkAction(it, MarkupVisitPhase.ENTERING) }
        }

        is Cite -> {
            schedule(node.citations)
        }

        is DefinitionList -> {
            schedule(node.definitions)
        }

        is Definition -> {
            for (body in node.content.asReversed()) schedule(body)
            schedule(node.term)
        }

        is Citation -> {
            schedule(node.suffix)
            schedule(node.prefix)
        }

        is Footnote -> {
            schedule(node.content)
        }

        is Specimen -> {
            schedule(node.content)
        }

        is ThematicBreak,
        is CodeBlock,
        is HTMLBlock,
        is FormulaBlock,
        is Text,
        is SoftBreak,
        is LineBreak,
        is Code,
        is HTML,
        is CrossLink,
        is CrossEmbedded,
        is Comment,
        is Formula,
        is Metadata,
        -> {}
    }
}
