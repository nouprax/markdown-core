package com.nouprax.markdown.core

/** Whether a walk callback fires on entering or leaving a node. */
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

/**
 * A read-only depth-first traversal of a document's nodes.
 *
 * Traversal is iterative, over an explicit frame stack rather than the call
 * stack, so a document walks at any nesting depth that parses.
 */
public object MarkupWalker {
    /**
     * Walks the document depth-first, dispatching each node to [visitor] in
     * preorder.
     *
     * No exit event, and no scope handed over: a node carries its own.
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
                    val scope = node.scope
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
}
