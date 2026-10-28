package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * The bindings' query corpus (specs/context): what the C queries answer at
 * the cursor of every example of the context fixture, in both units, which
 * the Kotlin queries must answer line for line.
 */
class QueriesAstTest {
    @Test
    fun theContextSpellingPathFieldAndMarkerQueriesAnswerAsTheCQueriesDo() {
        assertTrue(contextCases.isNotEmpty())
        for (case in contextCases) {
            for ((unit, answer) in listOf(TextUnit.UTF8 to case.utf8, TextUnit.UTF16 to case.utf16)) {
                assertEquals(
                    answer.expected,
                    render(case.source, unit, Position(answer.line, answer.column)),
                    "context.txt:${case.line} in $unit",
                )
            }
        }
    }
}

/**
 * Every query at [position] of [source], asked of a flushed session of it
 * and rendered as specs/context's README says.
 */
private fun render(
    source: String,
    unit: TextUnit,
    position: Position,
): String =
    MarkdownSession(source, unit).use { session ->
        val document = session.flush()
        session.locate(position)
        val context = session.context
        val lines = mutableListOf<String>()
        for (block in context.blocks) lines += "block ${block.kind ?: "None"} ${text(block.scope)}"
        for (opener in context.inlines) lines += "inline ${opener.kind ?: "None"} ${text(opener.scope)}"
        for (slot in context.slots) {
            val prefix = slot.prefix.joinToString("") { " prefix=\"$it\"" }
            lines += "slot ${slot.kind} ${slot.owner} ${if (slot.closed) "closed" else "open"} " +
                "before=${text(slot.before)} after=${text(slot.after)}$prefix"
        }
        for ((index, slot) in context.slots.withIndex()) {
            for ((value, spelled) in contextValues.zip(session.spell(slot, contextValues))) {
                lines += "spell $index \"$value\" " + (spelled?.let { "\"${it.text}\" ${it.cursor}" } ?: "none")
            }
        }
        lines += (listOf("path") + session.path(position).map { it.id.value.toString() }).joinToString(" ")
        for (node in NodeVisitor().also { document.walk(it) }.nodes) {
            var line = "node ${node.id.value}"
            for (kind in SlotKind.entries + listOf(null)) {
                val scopes = session.scope(node, kind)
                if (scopes.isNotEmpty()) line += " ${kind ?: "none"}=${text(scopes)}"
            }
            val markers = session.markers(node)
            if (markers.isNotEmpty()) line += " markers=${text(markers)}"
            lines += line
        }
        lines.joinToString("") { it + "\n" }
    }

private fun text(scope: Scope): String =
    "${scope.start.line}:${scope.start.column}..${scope.end.line}:${scope.end.column}"

private fun text(scopes: kotlin.collections.List<Scope>): String = scopes.joinToString(",") { text(it) }
