package com.nouprax.markdown.core

import java.lang.ref.WeakReference
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

/**
 * Every operation that follows tree edges -- decoding, release, equality,
 * walking, scope queries, hit testing and the description -- runs with an
 * explicit work stack. Each check runs on a thread with a small fixed stack,
 * so a recursion regression fails here deterministically instead of depending
 * on the platform's default stack size.
 */
class DeepTreeTest {
    @Test
    fun deepDocumentsStayWithinASmallStack() {
        for (depth in listOf(30_000, 65_536)) onSmallStack { deep(depth) }
    }

    private fun deep(depth: Int) {
        val source = "- ".repeat(depth) + "leaf\n"
        val document = Document.parse(source)

        // Equality of two deep documents that differ only at the deepest leaf.
        assertEquals(document, Document.parse(source))
        val other = Document.parse("- ".repeat(depth) + "lean\n")
        assertNotEquals(document, other)
        assertEquals(document.hashCode(), other.hashCode())

        // Walking visits every node once, numbered in walk order.
        val visitor = NodeVisitor()
        document.walk(visitor)
        assertEquals(depth * 2 + 3, visitor.nodes.size)
        assertEquals((1L..visitor.nodes.size).toList(), visitor.nodes.map { it.id.value })
        val leaf = assertIs<Text>(visitor.nodes.last())
        assertEquals("leaf", leaf.literal)

        // Scope queries and hit testing walk the whole document.
        val column = depth * 2 + 1
        assertEquals(Scope(Position(1, column), Position(1, column + 3)), document.scope(leaf, source))
        assertSame(leaf, document.node(Position(1, column), source))
        assertNull(document.node(Position(2, 1), source))

        // A description never follows tree edges.
        assertEquals("Document(id=1)", document.toString())
        assertEquals("Text(id=${depth * 2 + 3})", leaf.toString())

        // Release of a deep document while a view still holds a subtree.
        val (released, subtree) = retain(source, depth / 2)
        collect(released)
        val retained = NodeVisitor()
        subtree.walk(retained)
        assertEquals((depth - depth / 2) * 2 + 2, retained.nodes.size)
        assertEquals(leaf, retained.nodes.last())
    }

    @Test
    fun deepSessionDocumentsStayWithinASmallStack() {
        for (depth in listOf(30_000, 65_536)) onSmallStack { deepSession(depth) }
    }

    /**
     * The deep documents of a session (plan gates 4.9): the edit at the
     * deepest leaf publishes a document that compares, walks, locates and
     * describes like a fresh one, and the previous document is released while
     * the new one is alive.
     */
    private fun deepSession(depth: Int) {
        MarkdownSession("- ".repeat(depth) + "leaf\n").use { session ->
            val (previous, edited) = edit(session, depth)
            collect(previous)
            val text = session.text
            val visitor = NodeVisitor()
            edited.walk(visitor)
            assertEquals(depth * 2 + 3, visitor.nodes.size)
            val leaf = assertIs<Text>(visitor.nodes.last())
            assertEquals("lean", leaf.literal)
            val column = depth * 2 + 1
            assertEquals(Scope(Position(1, column), Position(1, column + 3)), edited.scope(leaf, text))
            assertSame(leaf, edited.node(Position(1, column), text))
            assertEquals("Document(id=1)", edited.toString())
        }
    }

    /** Edits the deepest leaf of [session]; the previous document is unreachable on return. */
    private fun edit(
        session: MarkdownSession,
        depth: Int,
    ): Pair<WeakReference<Document>, Document> {
        val previous = session.document
        val edited = session.edit(listOf(TextEdit(depth * 2, depth * 2 + 4, "lean")))
        assertNotEquals(previous, edited)
        assertEquals(previous.content.single().id, edited.content.single().id)
        return WeakReference(previous) to edited
    }

    /** Parses [source] and keeps only the list [levels] levels down; the document is unreachable on return. */
    private fun retain(
        source: String,
        levels: Int,
    ): Pair<WeakReference<Document>, Markup> {
        val document = Document.parse(source)
        var node: Markup = document.content.single()
        repeat(levels) {
            node =
                assertIs<List>(node)
                    .items
                    .single()
                    .content
                    .single()
        }
        return WeakReference(document) to node
    }

    /** Collects until [reference] is cleared: nothing but the retained subtree keeps the document's nodes. */
    private fun collect(reference: WeakReference<*>) {
        repeat(50) {
            if (reference.get() == null) return
            System.gc()
            Thread.sleep(10)
        }
        assertTrue(reference.get() == null, "the released document is still reachable")
    }

    private fun onSmallStack(check: () -> Unit) {
        var failure: Throwable? = null
        val thread = Thread(null, { runCatching(check).onFailure { failure = it } }, "deep", 256 * 1024)
        thread.start()
        thread.join()
        failure?.let { throw it }
    }
}
