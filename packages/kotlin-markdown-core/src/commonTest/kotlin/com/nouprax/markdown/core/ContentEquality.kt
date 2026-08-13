package com.nouprax.markdown.core

import kotlin.test.assertEquals

/**
 * Field-by-field content comparison for the value-mirror gate and the chain
 * tests.
 *
 * Kotlin's own `==` on nodes is (id, revision) by design, and the value-mirror
 * gate exists precisely because that equality cannot see a value bit that went
 * stale behind an unchanged pair — so nothing here uses `==` on a node. Kind,
 * scope, every typed field, and the child lists are compared structurally;
 * ids and revisions are deliberately not compared, because the reference
 * decode rides its own chain and its own series.
 */
internal fun assertSameForest(
    expected: kotlin.collections.List<Markup>,
    actual: kotlin.collections.List<Markup>,
    context: String,
) {
    // Pairs ride an explicit queue: nesting depth is input-controlled, so the
    // comparison must not ride the call stack.
    val queue = ArrayDeque<Pair<Markup, Markup>>()
    assertEquals(expected.size, actual.size, "$context: top-level block count")
    for (index in expected.indices) {
        queue.addLast(expected[index] to actual[index])
    }
    while (queue.isNotEmpty()) {
        val (left, right) = queue.removeFirst()
        assertEquals(left::class, right::class, "$context: node kind")
        assertEquals(left.scope, right.scope, "$context: ${left::class.simpleName} scope")
        assertEquals(ownFields(left), ownFields(right), "$context: ${left::class.simpleName} fields")
        val leftChildren = left.childValues()
        val rightChildren = right.childValues()
        assertEquals(
            leftChildren.size,
            rightChildren.size,
            "$context: ${left::class.simpleName} child count",
        )
        for (index in leftChildren.indices) {
            queue.addLast(leftChildren[index] to rightChildren[index])
        }
    }
}

/** Diagnostics carry no content equality of their own, so the gate compares
 * their projections. */
internal fun assertSameDiagnostics(
    expected: kotlin.collections.List<Diagnostic>,
    actual: kotlin.collections.List<Diagnostic>,
    context: String,
) {
    assertEquals(
        expected.map { it.code to it.scope },
        actual.map { it.code to it.scope },
        "$context: diagnostics",
    )
}

/** Every typed field the node carries beyond kind, scope, and children — the
 * projection a reuse defect would corrupt. Structure rides [childValues]; a
 * label's presence is still listed here so an absent label can never be
 * mistaken for a shifted child list. */
private fun ownFields(node: Markup): kotlin.collections.List<Any?> =
    when (node) {
        is Document,
        is BlockQuote,
        is Paragraph,
        is ThematicBreak,
        is TableCell,
        is DirectiveLabel,
        is SoftBreak,
        is LineBreak,
        is Emphasis,
        is Strong,
        is Strikethrough,
        -> emptyList()

        is Heading -> listOf(node.level)

        is List -> listOf(node.flavor, node.start, node.tight)

        is ListItem -> listOf(node.checked)

        is CodeBlock -> listOf(node.mode, node.info, node.language, node.literal, node.fenced, node.closed)

        is HTMLBlock -> listOf(node.comment, node.literal)

        is HTML -> listOf(node.comment, node.literal)

        is FormulaBlock -> listOf(node.mode, node.literal)

        is Formula -> listOf(node.mode, node.literal)

        is Code -> listOf(node.mode, node.literal)

        is Text -> listOf(node.literal)

        is Table -> listOf(node.alignments)

        is TableRow -> listOf(node.isHeader)

        is DirectiveBlock -> listOf(node.mode, node.name, node.attributes?.toList(), node.label == null)

        is Directive -> listOf(node.mode, node.name, node.attributes?.toList(), node.label == null)

        is FootnoteDefinition -> listOf(node.label)

        is FootnoteReference -> listOf(node.label)

        is Link -> listOf(node.destination, node.title)

        is Image -> listOf(node.source, node.title)

        is ReferenceDefinition -> listOf(node.label, node.destination, node.title)

        is LinkReference -> listOf(node.label, node.form)

        is ImageReference -> listOf(node.label, node.form)

        is CrossLink -> listOf(node.reference)

        is Embed -> listOf(node.reference)
    }
