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
        root.walkChildren(visit)
        visit(WalkEvent.EXITING, root)
    }

    private fun Markup.walkChildren(visit: (WalkEvent, Markup) -> Unit) {
        when (this) {
            is Document -> {
                content.walk(visit)
            }

            is BlockQuote -> {
                content.walk(visit)
            }

            is Paragraph -> {
                content.walk(visit)
            }

            is Heading -> {
                content.walk(visit)
            }

            is List -> {
                items.walk(visit)
            }

            is ListItem -> {
                content.walk(visit)
            }

            is Table -> {
                walk(header, visit)
                rows.walk(visit)
            }

            is TableRow -> {
                cells.walk(visit)
            }

            is TableCell -> {
                content.walk(visit)
            }

            is DirectiveBlock -> {
                label?.walk(visit)
                content.walk(visit)
            }

            is FootnoteDefinition -> {
                content.walk(visit)
            }

            is Emphasis -> {
                content.walk(visit)
            }

            is Strong -> {
                content.walk(visit)
            }

            is Strikethrough -> {
                content.walk(visit)
            }

            is Link -> {
                content.walk(visit)
            }

            is Image -> {
                content.walk(visit)
            }

            is Directive -> {
                label?.walk(visit)
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
            is Formula,
            is FootnoteReference,
            -> {}
        }
    }

    private fun <Node : Markup> kotlin.collections.List<Node>.walk(visit: (WalkEvent, Markup) -> Unit) {
        forEach { walk(it, visit) }
    }
}
