package com.nouprax.markdown.core

public enum class WalkEvent {
    ENTERING,
    EXITING,
}

public object MarkupWalker {
    public fun walk(
        document: Document,
        visitor: MarkupVisitor<Unit>,
    ) {
        walk(document) { event, node, _ ->
            if (event == WalkEvent.ENTERING) {
                node.accept(visitor)
            }
        }
    }

    /** Walks the document depth-first, supplying each event with the node's
     * resolved absolute scope. */
    public fun walk(
        document: Document,
        visit: (WalkEvent, Markup, Scope) -> Unit,
    ) {
        walk(document, document, visit)
    }

    /** Walks the subtree rooted at [from]; scopes stay document-absolute. */
    public fun walk(
        document: Document,
        from: Markup,
        visit: (WalkEvent, Markup, Scope) -> Unit,
    ) {
        val scope = document.scope(from)
        visit(WalkEvent.ENTERING, from, scope)
        from.walkChildren { child -> walk(document, child, visit) }
        visit(WalkEvent.EXITING, from, scope)
    }

    private fun Markup.walkChildren(walkChild: (Markup) -> Unit) {
        when (this) {
            is Document -> {
                content.forEach(walkChild)
            }

            is BlockQuote -> {
                content.forEach(walkChild)
            }

            is Paragraph -> {
                content.forEach(walkChild)
            }

            is Heading -> {
                content.forEach(walkChild)
            }

            is List -> {
                items.forEach(walkChild)
            }

            is ListItem -> {
                content.forEach(walkChild)
            }

            is Table -> {
                walkChild(header)
                rows.forEach(walkChild)
            }

            is TableRow -> {
                cells.forEach(walkChild)
            }

            is TableCell -> {
                content.forEach(walkChild)
            }

            is DirectiveBlock -> {
                label?.forEach(walkChild)
                content.forEach(walkChild)
            }

            is FootnoteDefinition -> {
                content.forEach(walkChild)
            }

            is Emphasis -> {
                content.forEach(walkChild)
            }

            is Strong -> {
                content.forEach(walkChild)
            }

            is Strikethrough -> {
                content.forEach(walkChild)
            }

            is Link -> {
                content.forEach(walkChild)
            }

            is Image -> {
                content.forEach(walkChild)
            }

            is Directive -> {
                label?.forEach(walkChild)
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
}
