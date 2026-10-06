package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertNotSame
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

/** Every node of [root], in canonical walk order. */
private fun nodes(root: Markup): kotlin.collections.List<Markup> = NodeVisitor().also { root.walk(it) }.nodes

class IdentityTest {
    @Test
    fun aFreshParseNumbersEveryOwnedNodeFromOneInWalkOrder() {
        // Unique across every owned relation -- content, labels, titles,
        // captions, terms, bodies, affixes, inline notes, metadata -- because
        // the walk visits every one of them.
        for (testCase in canonicalAstCases) {
            val ids = nodes(Document.parse(testCase.source)).map { it.id.value }
            assertEquals((1L..ids.size).toList(), ids, testCase.name)
        }
    }

    @Test
    fun twoFreshParsesOfOneTextAreEqualWithTheirIds() {
        for (testCase in canonicalAstCases) {
            val first = Document.parse(testCase.source)
            val second = Document.parse(testCase.source)
            assertNotSame(first, second)
            assertEquals(first, second, testCase.name)
            assertEquals(first.hashCode(), second.hashCode(), testCase.name)
            assertEquals(nodes(first), nodes(second), testCase.name)
        }
    }

    @Test
    fun documentsThatDifferOnlyAtTheDeepestLeafAreNotEqual() {
        val first = Document.parse("> - [*a*](/u)\n")
        val second = Document.parse("> - [*b*](/u)\n")
        assertNotEquals(first, second)
        // The same shape numbers the same ids, and a hash reads only the id.
        assertEquals(first.hashCode(), second.hashCode())
        val left = nodes(first)
        val right = nodes(second)
        assertEquals(left.map { it.id }, right.map { it.id })
        assertEquals(left.dropLast(1).map { it::class }, right.dropLast(1).map { it::class })
        assertNotEquals(left.last(), right.last())
    }

    @Test
    fun equalityComparesKindFieldsExtentsAndEveryRelation() {
        val base = Document.parse("a\n\nb\n")
        // An equal subtree of another document is equal: same id, extent and text.
        assertEquals(base.content[0], Document.parse("a\n\nc\n").content[0])
        assertNotEquals(base, Document.parse("a\n\nc\n"))
        // The same text one line lower keeps its id and changes its lead.
        val moved = Document.parse("a\n\n\nb\n")
        assertEquals(base.content[1].id, moved.content[1].id)
        assertNotEquals(base.content[1].extent, moved.content[1].extent)
        assertNotEquals(base.content[1], moved.content[1])
        // A different id is a different node, whatever else it holds.
        assertNotEquals<Markup>(base.content[0], Document.parse("*x*\n\na\n").content[1])
        // Attributes and a scalar of a relation's owner are values too.
        assertNotEquals(Document.parse("a {.x}\n"), Document.parse("a {.y}\n"))
        assertNotEquals(Document.parse("# a\n"), Document.parse("## a\n"))
        // The unit is not a contract field: one text parsed in either unit is one value.
        val utf8 = Document.parse("é🚀\n", TextUnit.UTF8)
        val utf16 = Document.parse("é🚀\n", TextUnit.UTF16)
        assertEquals(utf8, utf16)
        assertEquals(utf8.hashCode(), utf16.hashCode())

        // Runs are values too, a run that reads no content included; inline
        // extents are offsets in the root's content, so content read from
        // other source bytes is equal.
        fun paragraph(runs: kotlin.collections.List<Run>) =
            Paragraph(emptyList(), MarkupID(1), Extent(0, 3u), runs, null, Attributes.empty)
        val runs = listOf(Run(0, 1u, 1u), Run(1, 1u, 1u))
        assertEquals(paragraph(runs), paragraph(runs))
        assertNotEquals(paragraph(runs), paragraph(emptyList()))
        assertNotEquals(paragraph(runs), paragraph(listOf(Run(0, 1u, 1u), Run(1, 1u, 0u))))
        assertNotEquals(paragraph(runs), paragraph(runs + Run(0, 1u, 0u)))

        fun quoted(source: String) =
            assertIs<Paragraph>(assertIs<Callout>(Document.parse(source).content.single()).content.single())
        val near = quoted("> a\n> b\n")
        val far = quoted("> a\n>  b\n")
        assertNotEquals(near.runs, far.runs)
        assertEquals(near.content, far.content)
        // Inline notes compare through their citation's relation.
        assertEquals(Document.parse("x^[*n*]\n"), Document.parse("x^[*n*]\n"))
        assertNotEquals(Document.parse("x^[*n*]\n"), Document.parse("x^[*m*]\n"))
        assertNotEquals(Document.parse("x[^n]\n\n[^n]: n\n"), Document.parse("x^[n]\n\n[^n]: n\n"))
    }

    @Test
    fun aDescriptionNamesTheKindAndTheId() {
        val document = Document.parse("text\n")
        assertEquals("Document(id=1)", document.toString())
        assertEquals("Text(id=3)", assertIs<Paragraph>(document.content.single()).content.single().toString())
    }
}

class DefinitionTableTest {
    @Test
    fun tablesListEveryDefinitionInSourceOrderAndLookupsAnswerTheFirstLabel() {
        val source = "Call[^b] ^[inline] [^a]\n\n[^a]: first\n\n[^b]: only\n\n[^a]: second\n"
        val document = Document.parse(source)
        val paragraph = assertIs<Paragraph>(document.content.first())
        val inline =
            paragraph.content
                .filterIsInstance<Cite>()
                .map { it.citations.single().referent }
                .filterIsInstance<CitationReferent.Footnote>()
                .map { it.target }
                .filterIsInstance<FootnoteTarget.Note>()
                .single()
                .footnote
        assertEquals(listOf(inline) + document.content.drop(1), document.footnotes)
        assertEquals(listOf(null, "a", "b", "a"), document.footnotes.map { it.label })
        assertSame<Markup?>(document.content[1], document.footnote("a"))
        assertSame<Markup?>(document.content[2], document.footnote("b"))
        assertNull(document.footnote("c"))
        assertTrue(document.specimens.isEmpty())
        assertNull(document.specimen("a"))
        // The tables list nodes of the tree, never copies of them.
        val walked = nodes(document)
        assertTrue(document.footnotes.all { footnote -> walked.any { it === footnote } })
    }
}

class ScopeTest {
    @Test
    fun scopesCountColumnsInTheDocumentUnit() {
        // é is two UTF-8 bytes and one UTF-16 unit; 🚀 is four bytes and two units.
        val source = "é🚀 x\r\nz\n"
        for ((unit, end) in listOf(TextUnit.UTF8 to 8, TextUnit.UTF16 to 5)) {
            val document = Document.parse(source, unit)
            assertEquals(unit, document.unit)
            val paragraph = assertIs<Paragraph>(document.content.single())
            val (first, _, second) = paragraph.content
            assertEquals(listOf(Scope(Position(1, 1), Position(1, end))), document.scope(first, source))
            // A CRLF is one line terminator.
            assertEquals(listOf(Scope(Position(2, 1), Position(2, 1))), document.scope(second, source))
            assertEquals(listOf(Scope(Position(1, 1), Position(2, 1))), document.scope(paragraph, source))
        }
    }

    @Test
    fun scopesFollowTheBytesAtTheEdgesOfTheSource() {
        for (source in listOf("", "\n")) {
            val document = Document.parse(source)
            assertEquals(listOf(Scope(Position(1, 1), Position(1, 0))), document.scope(document, source))
        }
        // A node that ends right after a line terminator ends at `L:0`, in either unit.
        val source = "[^é]\n\n[^é]: 🚀\n\n[^é]: twice\n"
        for (unit in TextUnit.entries) {
            val document = Document.parse(source, unit)
            assertEquals(
                listOf(Scope(Position(3, 1), Position(4, 0))),
                document.scope(document.footnotes.first(), source),
            )
        }
    }

    @Test
    fun anInlineNoteCoversItsCaretAndBrackets() {
        val source = "x^[note]\n"
        val document = Document.parse(source)
        val citation = assertIs<Cite>(assertIs<Paragraph>(document.content.single()).content[1]).citations.single()
        val note = assertIs<FootnoteTarget.Note>(assertIs<CitationReferent.Footnote>(citation.referent).target)
        assertEquals(-2, note.footnote.extent.lead)
        assertEquals(listOf(Scope(Position(1, 2), Position(1, 8))), document.scope(note.footnote, source))
        assertEquals(listOf(Scope(Position(1, 4), Position(1, 7))), document.scope(citation, source))
    }

    @Test
    fun aNodeHasAScopeForEachOfItsSourceRanges() {
        val source = "- a\n  *b\n  c*\n"
        val document = Document.parse(source)
        val item = assertIs<List>(document.content.single()).items.single()
        val paragraph = assertIs<Paragraph>(item.content.single())
        val emphasis = assertIs<Emphasis>(paragraph.content[2])
        // A leaf block inside a container owns each line from where the
        // container's prefix ends: the runs its content was read from, one
        // per line, and the source between them is not its own.
        assertEquals(listOf(Run(0, 2u, 2u), Run(2, 3u, 3u), Run(2, 2u, 2u)), paragraph.runs)
        assertTrue(item.runs.isEmpty() && emphasis.runs.isEmpty())
        assertEquals(
            listOf(
                Scope(Position(1, 3), Position(2, 0)),
                Scope(Position(2, 3), Position(3, 0)),
                Scope(Position(3, 3), Position(3, 4)),
            ),
            document.scope(paragraph, source),
        )
        // An inline node's extent is in its root's content, whose runs map
        // it to the source: the indentation between its lines is not its own.
        assertEquals(
            listOf(Scope(Position(2, 3), Position(3, 0)), Scope(Position(3, 3), Position(3, 4))),
            document.scope(emphasis, source),
        )
        // A byte between a node's ranges is not the node's: the indentation is the item's.
        assertSame(item, document.node(Position(3, 1), source))
        assertSame(emphasis, document.node(Position(3, 4), source))
        assertSame(emphasis.content.last(), document.node(Position(3, 3), source))
        // A subtree dump places a node in content through its root's runs.
        assertEquals(
            "Emphasis scope=2:3..3:0,3:3..3:4 anchor=null attributes={} children=3\n" +
                "├── Text scope=2:4..2:4 anchor=null attributes={} literal=\"b\" children=0\n" +
                "├── SoftBreak scope=2:5..3:0 anchor=null attributes={} children=0\n" +
                "└── Text scope=3:3..3:3 anchor=null attributes={} literal=\"c\" children=0\n",
            document.dump(emphasis, source),
        )
        // The source must cover the last range.
        val failure = assertFailsWith<MarkdownCoreException> { document.scope(emphasis, source.dropLast(2)) }
        assertEquals(ErrorCode.OUT_OF_BOUNDS, failure.code)
    }

    @Test
    fun theSourceBetweenRunsIsCutFromEveryNodeTheyPlace() {
        val source = "> a *b\n> c* d\n"
        val document = Document.parse(source)
        val callout = assertIs<Callout>(document.content.single())
        val paragraph = assertIs<Paragraph>(callout.content.single())
        val emphasis = assertIs<Emphasis>(paragraph.content[1])
        // One run per line; the quote marker between them is the callout's.
        assertEquals(listOf(Run(0, 5u, 5u), Run(2, 4u, 4u)), paragraph.runs)
        assertEquals(
            listOf(Scope(Position(1, 3), Position(2, 0)), Scope(Position(2, 3), Position(2, 6))),
            document.scope(paragraph, source),
        )
        assertEquals(
            listOf(Scope(Position(1, 5), Position(2, 0)), Scope(Position(2, 3), Position(2, 4))),
            document.scope(emphasis, source),
        )
        assertSame(callout, document.node(Position(2, 1), source))
    }

    @Test
    fun runsThatReadNoContentPlaceABlockThatIsNoInlineRoot() {
        val source = "> ```\n> x\n> ```\n"
        val document = Document.parse(source)
        val callout = assertIs<Callout>(document.content.single())
        val code = assertIs<CodeBlock>(callout.content.single())
        // A code block's literal is no inline content: its runs only say
        // which source is its own, so the quote markers are the callout's.
        assertTrue(code.runs.isNotEmpty() && code.runs.all { it.length == 0u })
        assertEquals(
            listOf(
                Scope(Position(1, 3), Position(2, 0)),
                Scope(Position(2, 3), Position(3, 0)),
                Scope(Position(3, 3), Position(3, 5)),
            ),
            document.scope(code, source),
        )
        assertSame(callout, document.node(Position(2, 1), source))
        assertSame(code, document.node(Position(2, 3), source))
    }

    @Test
    fun hitTestingAnswersTheLastNodeHoldingTheByteInEitherUnit() {
        val source = "é🚀 *x*\r\nz\n"
        val utf8 = Document.parse(source, TextUnit.UTF8)
        val utf16 = Document.parse(source, TextUnit.UTF16)

        fun text(document: Document): Markup = assertIs<Paragraph>(document.content.single()).content.first()
        // The rocket starts at byte 2: UTF-8 column 3, UTF-16 column 2.
        assertSame(text(utf8), utf8.node(Position(1, 3), source))
        assertSame(text(utf16), utf16.node(Position(1, 2), source))
        // Inside the rocket's bytes, or between its surrogates, no scalar starts.
        assertNull(utf8.node(Position(1, 4), source))
        assertNull(utf16.node(Position(1, 3), source))
        assertSame(text(utf16), utf16.node(Position(1, 4), source))
        // The deepest node wins: the emphasis's text, then the emphasis at its delimiter.
        val emphasis = assertIs<Emphasis>(assertIs<Paragraph>(utf16.content.single()).content[1])
        assertSame(emphasis.content.single(), utf16.node(Position(1, 6), source))
        assertSame(emphasis, utf16.node(Position(1, 5), source))
        // A line terminator is a byte of its line; past it there is none.
        assertIs<SoftBreak>(utf16.node(Position(1, 8), source))
        assertSame(utf16.content.single(), utf16.node(Position(1, 9), source))
        assertNull(utf16.node(Position(1, 10), source))
        assertSame(assertIs<Paragraph>(utf16.content.single()).content.last(), utf16.node(Position(2, 1), source))
        // Past the last line there is none.
        assertNull(utf16.node(Position(4, 1), source))
    }

    @Test
    fun aSourceThatEndsBeforeTheNodeIsOutOfBounds() {
        // The UTF-16 columns of a short source would read past its bytes.
        fun outOfBounds(call: () -> Any) =
            assertEquals(ErrorCode.OUT_OF_BOUNDS, assertFailsWith<MarkdownCoreException> { call() }.code)

        val source = "é🚀 x\n"
        for ((unit, end) in listOf(TextUnit.UTF8 to 8, TextUnit.UTF16 to 5)) {
            val document = Document.parse(source, unit)
            val paragraph = document.content.single()
            val short = source.dropLast(2)
            outOfBounds { document.scope(paragraph, short) }
            outOfBounds { document.dump(short) }
            outOfBounds { document.dump(paragraph, short) }
            outOfBounds { MarkupDumper.dump(document, short) }
            outOfBounds { MarkupDumper.dump(document, paragraph, short) }
            // A source that covers the node's end is enough, and the node's end is exclusive.
            assertEquals(listOf(Scope(Position(1, 1), Position(1, end))), document.scope(paragraph, source.dropLast(1)))
        }
    }

    @Test
    fun aPositionBelowTheFirstLineOrColumnIsOutOfBounds() {
        val source = "a\n"
        val document = Document.parse(source)
        for (position in listOf(Position(0, 1), Position(1, 0), Position(-1, 1), Position(1, -1))) {
            val failure = assertFailsWith<MarkdownCoreException> { document.node(position, source) }
            assertEquals(ErrorCode.OUT_OF_BOUNDS, failure.code)
        }
        // Line 1, column 1 is the smallest position; a line past the source holds no node.
        assertIs<Text>(document.node(Position(1, 1), source))
        assertNull(document.node(Position(3, 1), source))
    }

    @Test
    fun theDumpPrintsUtf8ColumnsInEitherUnit() {
        val source = "é🚀\n"
        val expected =
            "Document scope=1:1..1:6 anchor=null attributes={} children=1\n" +
                "└── Paragraph scope=1:1..1:6 anchor=null attributes={} children=1\n" +
                "    └── Text scope=1:1..1:6 anchor=null attributes={} literal=\"é🚀\" children=0\n"
        for (unit in TextUnit.entries) {
            assertEquals(expected, Document.parse(source, unit).dump(source))
        }
    }

    @Test
    fun aSubtreeDumpStartsAtItsNodeWithItsOwnScopes() {
        val source = "a\n\n> *b*\n"
        val document = Document.parse(source)
        val callout = document.content[1]
        assertEquals(
            "Callout scope=3:1..3:5 anchor=null attributes={} variant=null collapsed=null children=1\n" +
                "└── Paragraph scope=3:3..3:5 anchor=null attributes={} children=1\n" +
                "    └── Emphasis scope=3:3..3:5 anchor=null attributes={} children=1\n" +
                "        └── Text scope=3:4..3:4 anchor=null attributes={} literal=\"b\" children=0\n",
            document.dump(callout, source),
        )
        assertEquals(document.dump(source), MarkupDumper.dump(document, document, source))
    }
}
