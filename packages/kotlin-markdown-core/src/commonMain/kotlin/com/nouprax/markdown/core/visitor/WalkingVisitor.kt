package com.nouprax.markdown.core

/** The phase of a depth-first markup walk. */
public enum class WalkPhase {
    /** The node has been reached, before any of its owned markup is visited. */
    ENTERING,

    /** Every owned markup relation of the node has been visited. */
    EXITING,
}

/**
 * An exhaustive observer with [visit] overloads for every concrete type in a depth-first markup walk.
 *
 * There is no untyped callback or default implementation. Adding a [Markup]
 * kind therefore breaks every walking visitor until it handles the new kind.
 * Markup-valued fields are not projected into a generic children collection:
 * each node-kind traversal branch schedules its own typed relations.
 *
 * The scoped values outside the markup union have entries of their own:
 * a [Citation] is reported between its cite's phases, before its prefix and
 * suffix content, and a [Footnote] after the document's content, before the
 * footnote's own content.
 */
public interface WalkingVisitor {
    public fun visit(
        document: Document,
        phase: WalkPhase,
    )

    public fun visit(
        callout: Callout,
        phase: WalkPhase,
    )

    public fun visit(
        paragraph: Paragraph,
        phase: WalkPhase,
    )

    public fun visit(
        heading: Heading,
        phase: WalkPhase,
    )

    public fun visit(
        thematicBreak: ThematicBreak,
        phase: WalkPhase,
    )

    public fun visit(
        list: List,
        phase: WalkPhase,
    )

    public fun visit(
        listItem: ListItem,
        phase: WalkPhase,
    )

    public fun visit(
        codeBlock: CodeBlock,
        phase: WalkPhase,
    )

    public fun visit(
        htmlBlock: HTMLBlock,
        phase: WalkPhase,
    )

    public fun visit(
        formulaBlock: FormulaBlock,
        phase: WalkPhase,
    )

    public fun visit(
        table: Table,
        phase: WalkPhase,
    )

    public fun visit(
        tableCaption: TableCaption,
        phase: WalkPhase,
    )

    public fun visit(
        tableRow: TableRow,
        phase: WalkPhase,
    )

    public fun visit(
        tableCell: TableCell,
        phase: WalkPhase,
    )

    public fun visit(
        directiveBlock: DirectiveBlock,
        phase: WalkPhase,
    )

    public fun visit(
        directiveLabel: DirectiveLabel,
        phase: WalkPhase,
    )

    public fun visit(
        text: Text,
        phase: WalkPhase,
    )

    public fun visit(
        softBreak: SoftBreak,
        phase: WalkPhase,
    )

    public fun visit(
        lineBreak: LineBreak,
        phase: WalkPhase,
    )

    public fun visit(
        code: Code,
        phase: WalkPhase,
    )

    public fun visit(
        html: HTML,
        phase: WalkPhase,
    )

    public fun visit(
        crossLink: CrossLink,
        phase: WalkPhase,
    )

    public fun visit(
        crossEmbedded: CrossEmbedded,
        phase: WalkPhase,
    )

    public fun visit(
        comment: Comment,
        phase: WalkPhase,
    )

    public fun visit(
        formula: Formula,
        phase: WalkPhase,
    )

    public fun visit(
        emphasis: Emphasis,
        phase: WalkPhase,
    )

    public fun visit(
        strong: Strong,
        phase: WalkPhase,
    )

    public fun visit(
        strikethrough: Strikethrough,
        phase: WalkPhase,
    )

    public fun visit(
        mark: Mark,
        phase: WalkPhase,
    )

    public fun visit(
        insertion: Insertion,
        phase: WalkPhase,
    )

    public fun visit(
        span: Span,
        phase: WalkPhase,
    )

    public fun visit(
        superscript: Superscript,
        phase: WalkPhase,
    )

    public fun visit(
        definitionList: DefinitionList,
        phase: WalkPhase,
    )

    public fun visit(
        definition: Definition,
        phase: WalkPhase,
    )

    public fun visit(
        subscript: Subscript,
        phase: WalkPhase,
    )

    public fun visit(
        link: Link,
        phase: WalkPhase,
    )

    public fun visit(
        embedded: Embedded,
        phase: WalkPhase,
    )

    public fun visit(
        directive: Directive,
        phase: WalkPhase,
    )

    public fun visit(
        cite: Cite,
        phase: WalkPhase,
    )

    public fun visit(
        citation: Citation,
        phase: WalkPhase,
    )

    public fun visit(
        footnote: Footnote,
        phase: WalkPhase,
    )

    public fun visit(
        specimen: Specimen,
        phase: WalkPhase,
    )
}

/**
 * Walks this markup and all of its owned markup depth first.
 *
 * An explicit action stack keeps the native call-stack depth independent of
 * document depth. Every node receives [WalkPhase.ENTERING] before its typed
 * relations and [WalkPhase.EXITING] after them.
 */
public fun Markup.walk(visitor: WalkingVisitor) {
    WalkingDriver(visitor).walk(this)
}

private enum class ActionPhase {
    ENTER,
    EXIT,
}

/** One pending step: a markup node or one of the scoped values, with the phase to report. */
private sealed interface WalkAction {
    val phase: ActionPhase

    data class Node(
        val node: Markup,
        override val phase: ActionPhase,
    ) : WalkAction

    data class CitationValue(
        val value: Citation,
        override val phase: ActionPhase,
    ) : WalkAction

    data class FootnoteValue(
        val value: Footnote,
        override val phase: ActionPhase,
    ) : WalkAction

    data class SpecimenValue(
        val value: Specimen,
        override val phase: ActionPhase,
    ) : WalkAction
}

/**
 * Node-kind callbacks own the relation schedule. The action stack is only a
 * traversal mechanism; it is not a public iterator or child projection.
 */
private class WalkingDriver(
    private val visitor: WalkingVisitor,
) : Visitor<Unit> {
    private val actions = mutableListOf<WalkAction>()
    private var phase: WalkPhase = WalkPhase.ENTERING

    fun walk(root: Markup) {
        actions += WalkAction.Node(root, ActionPhase.ENTER)
        while (actions.isNotEmpty()) {
            val action = actions.removeAt(actions.lastIndex)
            phase =
                when (action.phase) {
                    ActionPhase.ENTER -> WalkPhase.ENTERING
                    ActionPhase.EXIT -> WalkPhase.EXITING
                }
            when (action) {
                is WalkAction.Node -> action.node.accept(this)
                is WalkAction.CitationValue -> visit(action.value)
                is WalkAction.FootnoteValue -> visit(action.value)
                is WalkAction.SpecimenValue -> visit(action.value)
            }
        }
    }

    private fun scheduleExit(node: Markup) {
        if (phase == WalkPhase.ENTERING) actions += WalkAction.Node(node, ActionPhase.EXIT)
    }

    private fun schedule(nodes: kotlin.collections.List<Markup>) {
        for (index in nodes.indices.reversed()) {
            actions += WalkAction.Node(nodes[index], ActionPhase.ENTER)
        }
    }

    override fun visit(document: Document) {
        visitor.visit(document, phase)
        scheduleExit(document)
        if (phase == WalkPhase.ENTERING) {
            // The footnotes are visited after the content, in their order.
            for (index in document.specimens.indices.reversed()) {
                actions += WalkAction.SpecimenValue(document.specimens[index], ActionPhase.ENTER)
            }
            for (index in document.footnotes.indices.reversed()) {
                actions += WalkAction.FootnoteValue(document.footnotes[index], ActionPhase.ENTER)
            }
            schedule(document.content)
        }
    }

    /** A citation's prefix is visited before its suffix, between its phases. */
    private fun visit(citation: Citation) {
        visitor.visit(citation, phase)
        if (phase == WalkPhase.ENTERING) {
            actions += WalkAction.CitationValue(citation, ActionPhase.EXIT)
            schedule(citation.suffix)
            schedule(citation.prefix)
        }
    }

    private fun visit(footnote: Footnote) {
        visitor.visit(footnote, phase)
        if (phase == WalkPhase.ENTERING) {
            actions += WalkAction.FootnoteValue(footnote, ActionPhase.EXIT)
            schedule(footnote.content)
        }
    }

    private fun visit(specimen: Specimen) {
        visitor.visit(specimen, phase)
        if (phase == WalkPhase.ENTERING) {
            actions += WalkAction.SpecimenValue(specimen, ActionPhase.EXIT)
            schedule(specimen.content)
        }
    }

    override fun visit(callout: Callout) {
        visitor.visit(callout, phase)
        scheduleExit(callout)
        if (phase == WalkPhase.ENTERING) {
            schedule(callout.content)
            // The title is a callout-valued field, visited before the content.
            callout.title?.let(::schedule)
        }
    }

    override fun visit(paragraph: Paragraph) {
        visitor.visit(paragraph, phase)
        scheduleExit(paragraph)
        if (phase == WalkPhase.ENTERING) schedule(paragraph.content)
    }

    override fun visit(heading: Heading) {
        visitor.visit(heading, phase)
        scheduleExit(heading)
        if (phase == WalkPhase.ENTERING) schedule(heading.content)
    }

    override fun visit(thematicBreak: ThematicBreak) {
        visitor.visit(thematicBreak, phase)
        scheduleExit(thematicBreak)
    }

    override fun visit(list: List) {
        visitor.visit(list, phase)
        scheduleExit(list)
        if (phase == WalkPhase.ENTERING) schedule(list.items)
    }

    override fun visit(listItem: ListItem) {
        visitor.visit(listItem, phase)
        scheduleExit(listItem)
        if (phase == WalkPhase.ENTERING) schedule(listItem.content)
    }

    override fun visit(codeBlock: CodeBlock) {
        visitor.visit(codeBlock, phase)
        scheduleExit(codeBlock)
    }

    override fun visit(htmlBlock: HTMLBlock) {
        visitor.visit(htmlBlock, phase)
        scheduleExit(htmlBlock)
    }

    override fun visit(formulaBlock: FormulaBlock) {
        visitor.visit(formulaBlock, phase)
        scheduleExit(formulaBlock)
    }

    override fun visit(table: Table) {
        visitor.visit(table, phase)
        scheduleExit(table)
        if (phase == WalkPhase.ENTERING) {
            schedule(table.foot)
            schedule(table.content)
            schedule(table.head)
            table.caption?.let { schedule(listOf(it)) }
        }
    }

    override fun visit(tableCaption: TableCaption) {
        visitor.visit(tableCaption, phase)
        scheduleExit(tableCaption)
        if (phase == WalkPhase.ENTERING) schedule(tableCaption.content)
    }

    override fun visit(tableRow: TableRow) {
        visitor.visit(tableRow, phase)
        scheduleExit(tableRow)
        if (phase == WalkPhase.ENTERING) schedule(tableRow.cells)
    }

    override fun visit(tableCell: TableCell) {
        visitor.visit(tableCell, phase)
        scheduleExit(tableCell)
        if (phase == WalkPhase.ENTERING) schedule(tableCell.content)
    }

    override fun visit(directiveBlock: DirectiveBlock) {
        visitor.visit(directiveBlock, phase)
        scheduleExit(directiveBlock)
        if (phase == WalkPhase.ENTERING) {
            schedule(directiveBlock.content)
            directiveBlock.label?.let { actions += WalkAction.Node(it, ActionPhase.ENTER) }
        }
    }

    override fun visit(directiveLabel: DirectiveLabel) {
        visitor.visit(directiveLabel, phase)
        scheduleExit(directiveLabel)
        if (phase == WalkPhase.ENTERING) schedule(directiveLabel.content)
    }

    override fun visit(text: Text) {
        visitor.visit(text, phase)
        scheduleExit(text)
    }

    override fun visit(softBreak: SoftBreak) {
        visitor.visit(softBreak, phase)
        scheduleExit(softBreak)
    }

    override fun visit(lineBreak: LineBreak) {
        visitor.visit(lineBreak, phase)
        scheduleExit(lineBreak)
    }

    override fun visit(code: Code) {
        visitor.visit(code, phase)
        scheduleExit(code)
    }

    override fun visit(html: HTML) {
        visitor.visit(html, phase)
        scheduleExit(html)
    }

    override fun visit(crossLink: CrossLink) {
        visitor.visit(crossLink, phase)
        scheduleExit(crossLink)
    }

    override fun visit(crossEmbedded: CrossEmbedded) {
        visitor.visit(crossEmbedded, phase)
        scheduleExit(crossEmbedded)
    }

    override fun visit(comment: Comment) {
        visitor.visit(comment, phase)
        scheduleExit(comment)
    }

    override fun visit(formula: Formula) {
        visitor.visit(formula, phase)
        scheduleExit(formula)
    }

    override fun visit(emphasis: Emphasis) {
        visitor.visit(emphasis, phase)
        scheduleExit(emphasis)
        if (phase == WalkPhase.ENTERING) schedule(emphasis.content)
    }

    override fun visit(strong: Strong) {
        visitor.visit(strong, phase)
        scheduleExit(strong)
        if (phase == WalkPhase.ENTERING) schedule(strong.content)
    }

    override fun visit(strikethrough: Strikethrough) {
        visitor.visit(strikethrough, phase)
        scheduleExit(strikethrough)
        if (phase == WalkPhase.ENTERING) schedule(strikethrough.content)
    }

    override fun visit(mark: Mark) {
        visitor.visit(mark, phase)
        scheduleExit(mark)
        if (phase == WalkPhase.ENTERING) schedule(mark.content)
    }

    override fun visit(insertion: Insertion) {
        visitor.visit(insertion, phase)
        scheduleExit(insertion)
        if (phase == WalkPhase.ENTERING) schedule(insertion.content)
    }

    override fun visit(span: Span) {
        visitor.visit(span, phase)
        scheduleExit(span)
        if (phase == WalkPhase.ENTERING) schedule(span.content)
    }

    override fun visit(superscript: Superscript) {
        visitor.visit(superscript, phase)
        scheduleExit(superscript)
        if (phase == WalkPhase.ENTERING) schedule(superscript.content)
    }

    override fun visit(definitionList: DefinitionList) {
        visitor.visit(definitionList, phase)
        scheduleExit(definitionList)
        if (phase == WalkPhase.ENTERING) schedule(definitionList.definitions)
    }

    override fun visit(definition: Definition) {
        visitor.visit(definition, phase)
        scheduleExit(definition)
        if (phase == WalkPhase.ENTERING) {
            for (body in definition.content.asReversed()) schedule(body)
            schedule(definition.term)
        }
    }

    override fun visit(subscript: Subscript) {
        visitor.visit(subscript, phase)
        scheduleExit(subscript)
        if (phase == WalkPhase.ENTERING) schedule(subscript.content)
    }

    override fun visit(link: Link) {
        visitor.visit(link, phase)
        scheduleExit(link)
        if (phase == WalkPhase.ENTERING) schedule(link.content)
    }

    override fun visit(embedded: Embedded) {
        visitor.visit(embedded, phase)
        scheduleExit(embedded)
        if (phase == WalkPhase.ENTERING) schedule(embedded.content)
    }

    override fun visit(directive: Directive) {
        visitor.visit(directive, phase)
        scheduleExit(directive)
        if (phase == WalkPhase.ENTERING) {
            directive.label?.let { actions += WalkAction.Node(it, ActionPhase.ENTER) }
        }
    }

    override fun visit(cite: Cite) {
        visitor.visit(cite, phase)
        scheduleExit(cite)
        if (phase == WalkPhase.ENTERING) {
            for (index in cite.citations.indices.reversed()) {
                actions += WalkAction.CitationValue(cite.citations[index], ActionPhase.ENTER)
            }
        }
    }
}
