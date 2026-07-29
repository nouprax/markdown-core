package com.nouprax.markdown.core

public enum class WalkEvent {
    ENTERING,
    EXITING,
}

private sealed interface WalkFrame

private class EnterFrame(
    val node: Markup,
) : WalkFrame

private class ExitFrame(
    val node: Markup,
    val scope: Scope,
) : WalkFrame

public object MarkupWalker {
    /**
     * Walks the document depth-first, dispatching each node to [visitor] in
     * preorder. Scope-free by construction: a structural visitor neither
     * pays scope materialization nor depends on the snapshot's resolver
     * state, so a retained snapshot traverses regardless of whether it ever
     * resolved scopes.
     */
    public fun walk(
        document: Document,
        visitor: MarkupVisitor<Unit>,
    ) {
        val stack = ArrayDeque<Markup>()
        stack.addLast(document)
        while (stack.isNotEmpty()) {
            val node = stack.removeLast()
            node.accept(visitor)
            val children = node.childValues()
            for (index in children.indices.reversed()) {
                stack.addLast(children[index])
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
        // Nesting depth is input-controlled, so the traversal runs over an
        // explicit frame stack: a document that parsed must also walk,
        // whatever its depth. Children are pushed reversed so pops preserve
        // the recursive ENTERING/EXITING order exactly.
        val stack = ArrayDeque<WalkFrame>()
        stack.addLast(EnterFrame(from))
        while (stack.isNotEmpty()) {
            when (val frame = stack.removeLast()) {
                is ExitFrame -> {
                    visit(WalkEvent.EXITING, frame.node, frame.scope)
                }

                is EnterFrame -> {
                    val node = frame.node
                    val scope = document.scope(node)
                    visit(WalkEvent.ENTERING, node, scope)
                    stack.addLast(ExitFrame(node, scope))
                    val children = node.childValues()
                    for (index in children.indices.reversed()) {
                        stack.addLast(EnterFrame(children[index]))
                    }
                }
            }
        }
    }

    private fun Markup.childValues(): kotlin.collections.List<Markup> =
        when (this) {
            is Document -> {
                content
            }

            is BlockQuote -> {
                content
            }

            is Paragraph -> {
                content
            }

            is Heading -> {
                content
            }

            is List -> {
                items
            }

            is ListItem -> {
                content
            }

            is Table -> {
                buildList {
                    add(header)
                    addAll(rows)
                }
            }

            is TableRow -> {
                cells
            }

            is TableCell -> {
                content
            }

            is DirectiveBlock -> {
                label.orEmpty() + content
            }

            is FootnoteDefinition -> {
                content
            }

            is Emphasis -> {
                content
            }

            is Strong -> {
                content
            }

            is Strikethrough -> {
                content
            }

            is Link -> {
                content
            }

            is Image -> {
                content
            }

            is Directive -> {
                label.orEmpty()
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
            -> {
                emptyList()
            }
        }
}
