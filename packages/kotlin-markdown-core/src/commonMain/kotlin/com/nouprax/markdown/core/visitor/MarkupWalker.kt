package com.nouprax.markdown.core

// The walker selects typed callbacks; the canonical walk schedules each kind's named fields.
internal class MarkupWalker(
    private val visitor: MarkupVisitor,
) {
    fun walk(root: Markup) {
        val traversal = MarkupTraversal(root, 0)
        while (traversal.next()) {
            val node = traversal.node ?: continue
            val entered = traversal.step == MarkupTraversal.Step.ENTER
            dispatch(node, if (entered) MarkupVisitPhase.ENTER else MarkupVisitPhase.EXIT)
        }
    }

    private fun dispatch(
        node: Markup,
        phase: MarkupVisitPhase,
    ) {
        when (node) {
            is Document -> visitor.visit(node, phase)
            is Callout -> visitor.visit(node, phase)
            is Paragraph -> visitor.visit(node, phase)
            is Heading -> visitor.visit(node, phase)
            is ThematicBreak -> visitor.visit(node, phase)
            is List -> visitor.visit(node, phase)
            is ListItem -> visitor.visit(node, phase)
            is CodeBlock -> visitor.visit(node, phase)
            is HTMLBlock -> visitor.visit(node, phase)
            is FormulaBlock -> visitor.visit(node, phase)
            is Table -> visitor.visit(node, phase)
            is TableCaption -> visitor.visit(node, phase)
            is TableRow -> visitor.visit(node, phase)
            is TableCell -> visitor.visit(node, phase)
            is DirectiveBlock -> visitor.visit(node, phase)
            is DirectiveLabel -> visitor.visit(node, phase)
            is Text -> visitor.visit(node, phase)
            is SoftBreak -> visitor.visit(node, phase)
            is LineBreak -> visitor.visit(node, phase)
            is Code -> visitor.visit(node, phase)
            is HTML -> visitor.visit(node, phase)
            is Comment -> visitor.visit(node, phase)
            is CrossLink -> visitor.visit(node, phase)
            is CrossEmbedded -> visitor.visit(node, phase)
            is Formula -> visitor.visit(node, phase)
            is Emphasis -> visitor.visit(node, phase)
            is Strong -> visitor.visit(node, phase)
            is Strikethrough -> visitor.visit(node, phase)
            is Mark -> visitor.visit(node, phase)
            is Insertion -> visitor.visit(node, phase)
            is Span -> visitor.visit(node, phase)
            is Superscript -> visitor.visit(node, phase)
            is Subscript -> visitor.visit(node, phase)
            is DefinitionList -> visitor.visit(node, phase)
            is Definition -> visitor.visit(node, phase)
            is Link -> visitor.visit(node, phase)
            is Embedded -> visitor.visit(node, phase)
            is Directive -> visitor.visit(node, phase)
            is Cite -> visitor.visit(node, phase)
            is Citation -> visitor.visit(node, phase)
            is Footnote -> visitor.visit(node, phase)
            is Specimen -> visitor.visit(node, phase)
            is Metadata -> visitor.visit(node, phase)
        }
    }
}
