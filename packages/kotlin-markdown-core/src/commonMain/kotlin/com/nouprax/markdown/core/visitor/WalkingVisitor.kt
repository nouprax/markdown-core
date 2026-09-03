package com.nouprax.markdown.core

/** The phase of a depth-first markup walk. */
public enum class WalkPhase {
    /** The node has been reached, before any of its owned markup is visited. */
    ENTERING,

    /** Every owned markup relation of the node has been visited. */
    EXITING,
}

/**
 * An exhaustive, node-kind-dispatched observer for a depth-first markup walk.
 *
 * There is no untyped callback or default implementation. Adding a [Markup]
 * kind therefore breaks every walking visitor until it handles the new kind.
 * Markup-valued fields are not projected into a generic children collection:
 * each node-kind traversal branch schedules its own typed relations.
 */
public interface WalkingVisitor {
    public fun visitDocument(
        node: Document,
        phase: WalkPhase,
    )

    public fun visitBlockQuote(
        node: BlockQuote,
        phase: WalkPhase,
    )

    public fun visitParagraph(
        node: Paragraph,
        phase: WalkPhase,
    )

    public fun visitHeading(
        node: Heading,
        phase: WalkPhase,
    )

    public fun visitThematicBreak(
        node: ThematicBreak,
        phase: WalkPhase,
    )

    public fun visitList(
        node: List,
        phase: WalkPhase,
    )

    public fun visitListItem(
        node: ListItem,
        phase: WalkPhase,
    )

    public fun visitCodeBlock(
        node: CodeBlock,
        phase: WalkPhase,
    )

    public fun visitHTMLBlock(
        node: HTMLBlock,
        phase: WalkPhase,
    )

    public fun visitFormulaBlock(
        node: FormulaBlock,
        phase: WalkPhase,
    )

    public fun visitTable(
        node: Table,
        phase: WalkPhase,
    )

    public fun visitTableRow(
        node: TableRow,
        phase: WalkPhase,
    )

    public fun visitTableCell(
        node: TableCell,
        phase: WalkPhase,
    )

    public fun visitDirectiveBlock(
        node: DirectiveBlock,
        phase: WalkPhase,
    )

    public fun visitDirectiveLabel(
        node: DirectiveLabel,
        phase: WalkPhase,
    )

    public fun visitFootnoteDefinition(
        node: FootnoteDefinition,
        phase: WalkPhase,
    )

    public fun visitReferenceDefinition(
        node: ReferenceDefinition,
        phase: WalkPhase,
    )

    public fun visitLinkReference(
        node: LinkReference,
        phase: WalkPhase,
    )

    public fun visitImageReference(
        node: ImageReference,
        phase: WalkPhase,
    )

    public fun visitText(
        node: Text,
        phase: WalkPhase,
    )

    public fun visitSoftBreak(
        node: SoftBreak,
        phase: WalkPhase,
    )

    public fun visitLineBreak(
        node: LineBreak,
        phase: WalkPhase,
    )

    public fun visitCode(
        node: Code,
        phase: WalkPhase,
    )

    public fun visitHTML(
        node: HTML,
        phase: WalkPhase,
    )

    public fun visitFormula(
        node: Formula,
        phase: WalkPhase,
    )

    public fun visitEmphasis(
        node: Emphasis,
        phase: WalkPhase,
    )

    public fun visitStrong(
        node: Strong,
        phase: WalkPhase,
    )

    public fun visitStrikethrough(
        node: Strikethrough,
        phase: WalkPhase,
    )

    public fun visitLink(
        node: Link,
        phase: WalkPhase,
    )

    public fun visitImage(
        node: Image,
        phase: WalkPhase,
    )

    public fun visitDirective(
        node: Directive,
        phase: WalkPhase,
    )

    public fun visitFootnoteReference(
        node: FootnoteReference,
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

private data class WalkAction(
    val node: Markup,
    val phase: ActionPhase,
)

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
        actions += WalkAction(root, ActionPhase.ENTER)
        while (actions.isNotEmpty()) {
            val action = actions.removeAt(actions.lastIndex)
            phase =
                when (action.phase) {
                    ActionPhase.ENTER -> WalkPhase.ENTERING
                    ActionPhase.EXIT -> WalkPhase.EXITING
                }
            action.node.accept(this)
        }
    }

    private fun scheduleExit(node: Markup) {
        if (phase == WalkPhase.ENTERING) actions += WalkAction(node, ActionPhase.EXIT)
    }

    private fun schedule(nodes: kotlin.collections.List<Markup>) {
        for (index in nodes.indices.reversed()) {
            actions += WalkAction(nodes[index], ActionPhase.ENTER)
        }
    }

    override fun visitDocument(node: Document) {
        visitor.visitDocument(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitBlockQuote(node: BlockQuote) {
        visitor.visitBlockQuote(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitParagraph(node: Paragraph) {
        visitor.visitParagraph(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitHeading(node: Heading) {
        visitor.visitHeading(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitThematicBreak(node: ThematicBreak) {
        visitor.visitThematicBreak(node, phase)
        scheduleExit(node)
    }

    override fun visitList(node: List) {
        visitor.visitList(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.items)
    }

    override fun visitListItem(node: ListItem) {
        visitor.visitListItem(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitCodeBlock(node: CodeBlock) {
        visitor.visitCodeBlock(node, phase)
        scheduleExit(node)
    }

    override fun visitHTMLBlock(node: HTMLBlock) {
        visitor.visitHTMLBlock(node, phase)
        scheduleExit(node)
    }

    override fun visitFormulaBlock(node: FormulaBlock) {
        visitor.visitFormulaBlock(node, phase)
        scheduleExit(node)
    }

    override fun visitTable(node: Table) {
        visitor.visitTable(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) {
            schedule(node.rows)
            actions += WalkAction(node.header, ActionPhase.ENTER)
        }
    }

    override fun visitTableRow(node: TableRow) {
        visitor.visitTableRow(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.cells)
    }

    override fun visitTableCell(node: TableCell) {
        visitor.visitTableCell(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitDirectiveBlock(node: DirectiveBlock) {
        visitor.visitDirectiveBlock(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) {
            schedule(node.content)
            node.label?.let { actions += WalkAction(it, ActionPhase.ENTER) }
        }
    }

    override fun visitDirectiveLabel(node: DirectiveLabel) {
        visitor.visitDirectiveLabel(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitFootnoteDefinition(node: FootnoteDefinition) {
        visitor.visitFootnoteDefinition(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitReferenceDefinition(node: ReferenceDefinition) {
        visitor.visitReferenceDefinition(node, phase)
        scheduleExit(node)
    }

    override fun visitLinkReference(node: LinkReference) {
        visitor.visitLinkReference(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitImageReference(node: ImageReference) {
        visitor.visitImageReference(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitText(node: Text) {
        visitor.visitText(node, phase)
        scheduleExit(node)
    }

    override fun visitSoftBreak(node: SoftBreak) {
        visitor.visitSoftBreak(node, phase)
        scheduleExit(node)
    }

    override fun visitLineBreak(node: LineBreak) {
        visitor.visitLineBreak(node, phase)
        scheduleExit(node)
    }

    override fun visitCode(node: Code) {
        visitor.visitCode(node, phase)
        scheduleExit(node)
    }

    override fun visitHTML(node: HTML) {
        visitor.visitHTML(node, phase)
        scheduleExit(node)
    }

    override fun visitFormula(node: Formula) {
        visitor.visitFormula(node, phase)
        scheduleExit(node)
    }

    override fun visitEmphasis(node: Emphasis) {
        visitor.visitEmphasis(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitStrong(node: Strong) {
        visitor.visitStrong(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitStrikethrough(node: Strikethrough) {
        visitor.visitStrikethrough(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitLink(node: Link) {
        visitor.visitLink(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitImage(node: Image) {
        visitor.visitImage(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) schedule(node.content)
    }

    override fun visitDirective(node: Directive) {
        visitor.visitDirective(node, phase)
        scheduleExit(node)
        if (phase == WalkPhase.ENTERING) {
            node.label?.let { actions += WalkAction(it, ActionPhase.ENTER) }
        }
    }

    override fun visitFootnoteReference(node: FootnoteReference) {
        visitor.visitFootnoteReference(node, phase)
        scheduleExit(node)
    }
}
