package com.nouprax.markdown.core

public enum class WalkEvent {
    ENTERING,
    EXITING,
}

public object Walker {
    public fun walk(
        root: Markup,
        visitor: Visitor<Unit>,
    ) {
        walk(root) { event, node ->
            if (event == WalkEvent.ENTERING) {
                node.accept(visitor)
            }
        }
    }

    public fun walk(
        root: Markup,
        visit: (WalkEvent, Markup) -> Unit,
    ) {
        visit(WalkEvent.ENTERING, root)
        root.children().forEach { walk(it, visit) }
        visit(WalkEvent.EXITING, root)
    }
}

/**
 * Every child of a node, in the order the C tree holds them -- which is what a
 * region's owner path counts. [Walker] walks with it and
 * [Document.ownerOf] descends with it, so the two cannot disagree.
 */
internal fun Markup.children(): kotlin.collections.List<Markup> =
    when (this) {
        is DocumentRoot -> content

        is BlockQuote -> content

        is Paragraph -> content

        is Heading -> content

        is List -> items

        is ListItem -> content

        is Table -> listOf(header) + rows

        is TableRow -> cells

        is TableCell -> content

        // Label first, then content: they are two runs of one C child list.
        is DirectiveBlock -> if (label == null) content else listOf(label) + content

        is DirectiveLabel -> content

        is FootnoteDefinition -> content

        is Emphasis -> content

        is Strong -> content

        is Strikethrough -> content

        is Link -> content

        is Image -> content

        is LinkReference -> content

        is ImageReference -> content

        is Directive -> if (label == null) emptyList() else listOf(label)

        is ThematicBreak,
        is ReferenceDefinition,
        is CodeBlock,
        is HTMLBlock,
        is FormulaBlock,
        is Text,
        is SoftBreak,
        is LineBreak,
        is Code,
        is HTML,
        is Formula,
        is FootnoteReference,
        -> emptyList()
    }
