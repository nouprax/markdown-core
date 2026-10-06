package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

/**
 * Writes MCB3 messages the parser never produces (docs/architecture/wire-format.md),
 * so decoder tests state records instead of patching byte offsets.
 */
private class MessageWriter {
    private val bytes = ArrayList<Byte>()

    /** The id the next record takes: records are numbered in the order they are written. */
    private var next = 1L

    fun u8(value: Int) = apply { bytes += value.toByte() }

    fun u32(value: Long) = apply { repeat(4) { u8((value shr (it * 8)).toInt() and 0xff) } }

    fun u32(value: Int) = u32(value.toLong() and 0xffff_ffffL)

    fun int(value: Long) = apply { repeat(8) { u8((value shr (it * 8)).toInt() and 0xff) } }

    fun double(value: Double) = int(value.toRawBits())

    fun bool(value: Boolean) = u8(if (value) 1 else 0)

    fun string(value: String) =
        apply {
            val encoded = value.encodeToByteArray()
            u32(encoded.size)
            bytes += encoded.toList()
        }

    /** `T?`: a presence, then `write(value)` when present. */
    fun <T> optional(
        value: T?,
        write: MessageWriter.(T) -> Unit,
    ) = apply {
        bool(value != null)
        if (value != null) write(value)
    }

    fun attributes(
        classes: kotlin.collections.List<String> = emptyList(),
        records: kotlin.collections.List<Pair<String, String>> = emptyList(),
    ) = apply {
        u32(classes.size)
        classes.forEach { string(it) }
        u32(records.size)
        records.forEach { (name, value) -> string(name).string(value) }
    }

    /** A record's kind and inherited fields; its own fields follow. */
    fun record(
        kind: WireNodeKind,
        anchor: String? = null,
        classes: kotlin.collections.List<String> = emptyList(),
    ) = record(kind.rawValue, anchor, classes)

    fun record(
        ordinal: Int,
        anchor: String? = null,
        classes: kotlin.collections.List<String> = emptyList(),
        id: Long = next,
        lead: Int = 0,
        span: Long = 0,
        runs: kotlin.collections.List<Run> = emptyList(),
    ) = apply {
        next = id + 1
        u8(ordinal)
        int(id)
        u32(lead).u32(span)
        u32(runs.size)
        runs.forEach { u32(it.lead).u32(it.span.toLong()).u32(it.length.toLong()) }
        optional(anchor) { string(it) }
        attributes(classes)
    }

    fun text(literal: String) = record(WireNodeKind.TEXT).string(literal)

    /** A document record over the `content` nodes written before it, and its definition tables. */
    fun root(
        content: Int,
        footnotes: kotlin.collections.List<Long> = emptyList(),
        specimens: kotlin.collections.List<Long> = emptyList(),
        references: kotlin.collections.List<Long> = emptyList(),
        labels: kotlin.collections.List<Pair<String, Long>> = emptyList(),
    ) = record(WireNodeKind.DOCUMENT)
        .u32(content)
        .bool(false)
        .table(footnotes)
        .table(specimens)
        .table(references)
        .apply {
            u32(labels.size)
            labels.forEach { (label, id) -> string(label).int(id) }
        }

    private fun table(ids: kotlin.collections.List<Long>) =
        apply {
            u32(ids.size)
            ids.forEach { int(it) }
        }

    fun document(): ByteArray = message(0)

    fun error(code: Int): ByteArray = u32(code).message(1)

    private fun message(status: Int): ByteArray {
        val header =
            MessageWriter()
                .u8(0x4d)
                .u8(0x43)
                .u8(0x42)
                .u8(0x33)
                .u32(9 + bytes.size)
                .u8(status)
        return (header.bytes + bytes).toByteArray()
    }
}

private fun decode(writer: MessageWriter): Document = WireDecoder.decode(writer.document(), TextUnit.UTF16)

class WireDecoderTest {
    @Test
    fun failuresKeepTheirCodeAcrossTheWire() {
        // Each `markdown_core_status` value reaches the consumer as the code of that value.
        fun failure(code: Int) =
            assertFailsWith<MarkdownCoreException> { WireDecoder.decode(MessageWriter().error(code), TextUnit.UTF16) }
        assertEquals(ErrorCode.ALLOCATION_FAILED, failure(1).code)
        assertEquals(ErrorCode.OUT_OF_BOUNDS, failure(2).code)
        assertEquals(ErrorCode.KIND_MISMATCH, failure(3).code)
        assertEquals(ErrorCode.INSIDE_SCALAR, failure(4).code)
    }

    @Test
    fun definitionBodiesHoldContent() {
        // Term `T`; two bodies, the first empty and the second holding `b`.
        val definition =
            assertIs<DefinitionList>(
                decode(
                    MessageWriter()
                        .text("T")
                        .text("b")
                        .record(WireNodeKind.DEFINITION)
                        .u32(1)
                        .u32(2)
                        .u32(0)
                        .u32(1)
                        .bool(true)
                        .record(WireNodeKind.DEFINITION_LIST)
                        .u32(1)
                        .root(1),
                ).content.single(),
            ).definitions.single()
        assertEquals(listOf("T"), definition.term.map { assertIs<Text>(it).literal })
        assertEquals(
            listOf(emptyList(), listOf("b")),
            definition.content.map { body ->
                body.map { assertIs<Text>(it).literal }
            },
        )
        assertTrue(definition.compact)
    }

    @Test
    fun everyBranchOfAnOrderedListSurvivesDecoding() {
        // Variant and delimiter branches in the contract's declaration order,
        // with the Bool payloads the Alpha, Roman and Parenthesis branches carry.
        fun list(write: MessageWriter.() -> Unit): List =
            assertIs<List>(
                decode(
                    MessageWriter()
                        .record(WireNodeKind.LIST)
                        .u8(1)
                        .optional(7L) { int(it) }
                        .apply(write)
                        .bool(true)
                        .u32(0)
                        .root(1),
                ).content.single(),
            )
        for ((branch, expected) in listOf(0 to OrderedListVariant.Decimal, 3 to OrderedListVariant.Default)) {
            val decoded = list { bool(true).u8(branch).bool(false) }
            assertSame(expected, decoded.variant)
            assertNull(decoded.delimiter)
            assertEquals(ListFlavor.ORDERED, decoded.flavor)
            assertEquals(7L, decoded.start)
        }
        for (lowercased in listOf(false, true)) {
            assertEquals(
                lowercased,
                assertIs<OrderedListVariant.Alpha>(
                    list {
                        bool(true).u8(1).bool(lowercased).bool(false)
                    }.variant,
                ).lowercased,
            )
            assertEquals(
                lowercased,
                assertIs<OrderedListVariant.Roman>(
                    list {
                        bool(true).u8(2).bool(lowercased).bool(false)
                    }.variant,
                ).lowercased,
            )
            assertEquals(
                lowercased,
                assertIs<OrderedListDelimiter.Parenthesis>(
                    list { bool(false).bool(true).u8(1).bool(lowercased) }.delimiter,
                ).closed,
            )
        }
        assertSame(OrderedListDelimiter.Period, list { bool(false).bool(true).u8(0) }.delimiter)
        assertSame(OrderedListDelimiter.Default, list { bool(false).bool(true).u8(2) }.delimiter)
    }

    @Test
    fun linksAndReferencesWriteTheirDestinationAndTitleInTheirRecords() {
        // A direct link writes the url branch and its title, a reference
        // occurrence the reference branch and no title, and a Reference its
        // label, url and title. Each node holds the anchor and attributes its
        // own record writes.
        val document =
            decode(
                MessageWriter()
                    .text("a")
                    .record(WireNodeKind.LINK)
                    .u8(0)
                    .string("/u")
                    .optional("t") { string(it) }
                    .u32(1)
                    .record(WireNodeKind.LINK, anchor = "own", classes = listOf("mine"))
                    .u8(2)
                    .string("r")
                    .optional<String>(null) { string(it) }
                    .u32(0)
                    .record(WireNodeKind.EMBEDDED)
                    .u8(2)
                    .string("r")
                    .optional<String>(null) { string(it) }
                    .optional<Long>(null) { int(it) }
                    .u32(0)
                    .record(WireNodeKind.PARAGRAPH)
                    .u32(3)
                    .record(WireNodeKind.REFERENCE, anchor = "definition", classes = listOf("shared"))
                    .string("r")
                    .u8(0)
                    .string("/r")
                    .optional("title") { string(it) }
                    .root(2, references = listOf(6), labels = listOf("r" to 6L)),
            )
        val inlines = assertIs<Paragraph>(document.content[0]).content
        val direct = assertIs<Link>(inlines[0])
        assertEquals(Destination.Url("/u"), direct.dest)
        assertEquals("t", direct.title)
        assertNull(direct.anchor)
        assertSame(Attributes.empty, direct.attributes)
        val occurrence = assertIs<Link>(inlines[1])
        assertEquals(Destination.Reference("r"), occurrence.dest)
        assertNull(occurrence.title)
        assertEquals("own", occurrence.anchor)
        assertEquals(listOf("mine"), occurrence.attributes.classes)
        val image = assertIs<Embedded>(inlines[2])
        assertEquals(Destination.Reference("r"), image.dest)
        assertNull(image.title)
        assertNull(image.dimensions)

        val reference = assertIs<Reference>(document.content[1])
        assertEquals(MarkupID(6), reference.id)
        assertEquals("r", reference.label)
        assertEquals(Destination.Url("/r"), reference.dest)
        assertEquals("title", reference.title)
        assertEquals("definition", reference.anchor)
        assertEquals(listOf("shared"), reference.attributes.classes)
        assertEquals(listOf(reference), document.references)
        assertSame<Markup?>(reference, document.reference("r"))
    }

    @Test
    fun theLabelTableNamesAReferenceOrASectionTitle() {
        // A label resolves to the Heading its id names; an id naming any
        // other kind is an invalid message.
        fun document(id: Long) =
            decode(
                MessageWriter()
                    .text("T")
                    .record(WireNodeKind.HEADING)
                    .int(1)
                    .u32(1)
                    .text("p")
                    .record(WireNodeKind.PARAGRAPH)
                    .u32(1)
                    .root(2, labels = listOf("t" to id)),
            )
        val valid = document(2)
        assertSame<Markup?>(valid.content[0], valid.reference("t"))
        assertTrue(valid.references.isEmpty())
        assertNull(valid.reference("p"))
        assertFailsWith<NoSuchElementException> { document(4) }
    }

    @Test
    fun recordsCarryTheirIdAndTheirSignedExtentAndRunsVerbatim() {
        val runs =
            listOf(
                Run(Int.MIN_VALUE, 0u, UInt.MAX_VALUE),
                Run(3, 2u, 2u),
                Run(Int.MAX_VALUE, UInt.MAX_VALUE, 0u),
            )
        val document =
            decode(
                MessageWriter()
                    .record(WireNodeKind.TEXT.rawValue, id = 7, lead = -2, span = 0xffff_ffffL)
                    .string("t")
                    .record(WireNodeKind.PARAGRAPH.rawValue, runs = runs)
                    .u32(1)
                    .root(1),
            )
        val paragraph = assertIs<Paragraph>(document.content.single())
        val text = paragraph.content.single()
        assertEquals(MarkupID(7), text.id)
        assertEquals(Extent(-2, UInt.MAX_VALUE), text.extent)
        assertTrue(text.runs.isEmpty())
        assertEquals(runs, paragraph.runs)
        assertEquals(MarkupID(9), document.id)
    }

    @Test
    fun anInlineNoteIsTheFirstNodeItsCitationTakes() {
        // A footnote target's `note` branch writes nothing for its footnote:
        // the record takes the note ahead of its prefix and suffix.
        val document =
            decode(
                MessageWriter()
                    .text("n")
                    .record(WireNodeKind.FOOTNOTE)
                    .bool(false)
                    .u32(1)
                    .text("p")
                    .record(WireNodeKind.CITATION)
                    .u8(1)
                    .u8(1)
                    .u32(1)
                    .u32(0)
                    .record(WireNodeKind.CITE)
                    .u32(1)
                    .root(1, footnotes = listOf(2)),
            )
        val citation = assertIs<Cite>(document.content.single()).citations.single()
        val note = assertIs<FootnoteTarget.Note>(assertIs<CitationReferent.Footnote>(citation.referent).target)
        assertEquals(MarkupID(2), note.footnote.id)
        assertNull(note.footnote.label)
        assertEquals("n", assertIs<Text>(note.footnote.content.single()).literal)
        assertEquals("p", assertIs<Text>(citation.prefix.single()).literal)
        assertSame(note.footnote, document.footnotes.single())
        assertNull(document.footnote("n"))
    }

    @Test
    fun definitionTablesNameDefinitionsOfTheirKind() {
        fun document(
            footnotes: kotlin.collections.List<Long>,
            specimens: kotlin.collections.List<Long>,
        ) = decode(
            MessageWriter()
                .record(WireNodeKind.FOOTNOTE)
                .optional("a") { string(it) }
                .u32(0)
                .record(WireNodeKind.SPECIMEN)
                .optional("s") { string(it) }
                .optional(3L) { int(it) }
                .u32(0)
                .record(WireNodeKind.FOOTNOTE)
                .optional("a") { string(it) }
                .u32(0)
                .root(3, footnotes, specimens),
        )
        val valid = document(listOf(1, 3), listOf(2))
        assertEquals(valid.content.filterIsInstance<Footnote>(), valid.footnotes)
        assertSame<Markup?>(valid.content[0], valid.footnote("a"))
        assertSame<Markup?>(valid.content[1], valid.specimen("s"))
        assertEquals(3L, valid.specimens.single().start)
    }
}
