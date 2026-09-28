package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

/**
 * Writes MCB2 messages the parser never produces (docs/architecture/wire-format.md),
 * so decoder tests state records instead of patching byte offsets.
 */
private class MessageWriter {
    private val bytes = ArrayList<Byte>()

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
    ) = apply {
        u8(ordinal)
        repeat(4) { u32(1) }
        optional(anchor) { string(it) }
        attributes(classes)
    }

    fun text(literal: String) = record(WireNodeKind.TEXT).string(literal)

    /** A document record over the `content` nodes written before it. */
    fun root(content: Int) =
        record(WireNodeKind.DOCUMENT)
            .u32(content)
            .bool(false)
            .u32(0)
            .u32(0)

    fun document(): ByteArray = message(0)

    fun error(
        code: Int,
        message: String,
    ): ByteArray = u32(code).string(message).message(1)

    private fun message(status: Int): ByteArray {
        val header =
            MessageWriter()
                .u8(0x4d)
                .u8(0x43)
                .u8(0x42)
                .u8(0x32)
                .u32(9 + bytes.size)
                .u8(status)
        return (header.bytes + bytes).toByteArray()
    }
}

private fun decode(writer: MessageWriter): Document = WireDecoder.decode(writer.document())

private inline fun rejects(
    message: String,
    decode: () -> Unit,
) {
    val failure = assertFailsWith<IllegalArgumentException> { decode() }
    assertTrue(message in failure.message.orEmpty(), "expected \"$message\", got \"${failure.message}\"")
}

class WireDecoderTest {
    @Test
    fun parseFailuresKeepTheirCodeAcrossTheWire() {
        // Allocation failure must not be collapsed into an internal error that
        // a consumer could mistake for a recoverable path.
        fun failure(code: Int) =
            assertFailsWith<ParseException> { WireDecoder.decode(MessageWriter().error(code, "bad")) }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure(1).code)
        assertEquals("bad", failure(1).message)
        assertEquals(ParseErrorCode.ALLOCATION_FAILED, failure(2).code)
        assertEquals(ParseErrorCode.INTERNAL, failure(99).code)
    }

    @Test
    fun malformedMessagesAreRejectedBeforeTheyEnterTheAst() {
        // The two sides of the wire are built separately, and a decoder that
        // silently mapped an unknown value would turn a protocol mismatch into
        // a wrong document. Each guard is exercised, so none can be removed and
        // stay green.
        fun text() = MessageWriter().text("t")
        assertEquals("t", assertIs<Text>(decode(text().root(1)).content.single()).literal)

        // Values outside the contract's enums, booleans and branches.
        rejects(
            "invalid enum index 2",
        ) {
            decode(
                MessageWriter()
                    .record(WireNodeKind.FORMULA)
                    .u8(2)
                    .string("x")
                    .root(1),
            )
        }
        rejects("invalid boolean 2") {
            decode(
                MessageWriter()
                    .record(WireNodeKind.CODE_BLOCK)
                    .bool(false)
                    .bool(false)
                    .string("x")
                    .u8(2),
            )
        }
        rejects("invalid branch 2") {
            decode(
                MessageWriter()
                    .record(WireNodeKind.CROSS_LINK)
                    .u8(2)
                    .string("p")
                    .bool(false)
                    .root(1),
            )
        }
        rejects("32-bit integer") {
            decode(
                MessageWriter()
                    .record(WireNodeKind.HEADING)
                    .int(1L shl 32)
                    .u32(0)
                    .root(1),
            )
        }

        // A typed field accepts its own kind only, and content accepts no typed
        // kind: a directive's label is a field, never a generic child.
        rejects("places a PARAGRAPH node in a DIRECTIVE_LABEL field") {
            decode(
                MessageWriter()
                    .record(WireNodeKind.PARAGRAPH)
                    .u32(0)
                    .record(WireNodeKind.DIRECTIVE)
                    .string("n")
                    .bool(true)
                    .root(1),
            )
        }
        rejects("places a DIRECTIVE_LABEL node in a content field") {
            decode(MessageWriter().record(WireNodeKind.DIRECTIVE_LABEL).u32(0).root(1))
        }
        rejects("places a PARAGRAPH node in a DEFINITION field") {
            decode(
                MessageWriter()
                    .record(
                        WireNodeKind.PARAGRAPH,
                    ).u32(0)
                    .record(WireNodeKind.DEFINITION_LIST)
                    .u32(1)
                    .root(1),
            )
        }

        // The shape of the message as a whole.
        rejects("unknown node kind 99") { decode(MessageWriter().record(99)) }
        rejects("names more nodes than precede it") { decode(MessageWriter().root(1)) }
        rejects("not one document tree") { decode(text().text("u").root(1)) }
        rejects("not one document tree") { decode(text()) }
        rejects("unknown resource 1") {
            decode(
                MessageWriter()
                    .record(WireNodeKind.LINK)
                    .u32(1)
                    .u32(0)
                    .root(1),
            )
        }
        rejects("count exceeds the message") {
            decode(MessageWriter().record(WireNodeKind.TABLE).bool(false).u32(0xffff_ffffL))
        }

        val valid = text().root(1).document()
        val truncated = valid.copyOf(valid.size - 1).also { it[4] = (valid.size - 1).toByte() }
        rejects("truncated native result") { WireDecoder.decode(truncated) }
        rejects("length does not match") { WireDecoder.decode(valid + 0.toByte()) }
        rejects("invalid native result at byte 0") { WireDecoder.decode(valid.copyOf().also { it[0] = 0 }) }
        rejects("unsupported native result status 2") { WireDecoder.decode(valid.copyOf().also { it[8] = 2 }) }
        rejects("truncated native result header") { WireDecoder.decode(valid.copyOf(8)) }
    }

    @Test
    fun definitionBodiesHoldContentAndDefinitionListsHoldDefinitionsOnly() {
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
        rejects("places a FOOTNOTE node in a content field") {
            decode(
                MessageWriter()
                    .text("T")
                    .record(WireNodeKind.FOOTNOTE)
                    .string("x")
                    .u32(0)
                    .record(WireNodeKind.DEFINITION)
                    .u32(1)
                    .u32(1)
                    .u32(1)
                    .bool(false)
                    .record(WireNodeKind.DEFINITION_LIST)
                    .u32(1)
                    .root(1),
            )
        }
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
    fun aResourceIsDefinedOnceAndInheritedByEveryOccurrence() {
        // The first occurrence defines resource 0; the second names it. The
        // node's anchor is its own when present, and its attributes are the
        // resource's followed by its own.
        val document =
            decode(
                MessageWriter()
                    .record(WireNodeKind.LINK)
                    .u32(0)
                    .u8(0)
                    .string("/u")
                    .optional("t") { string(it) }
                    .optional("definition") { string(it) }
                    .attributes(listOf("shared"))
                    .u32(0)
                    .record(WireNodeKind.LINK, anchor = "own", classes = listOf("mine"))
                    .u32(0)
                    .u32(0)
                    .record(WireNodeKind.PARAGRAPH)
                    .u32(2)
                    .root(1),
            )
        val (first, second) = assertIs<Paragraph>(document.content.single()).content.map { assertIs<Link>(it) }
        assertSame(first.dest, second.dest)
        assertEquals("/u", assertIs<Destination.Url>(first.dest).value)
        assertEquals("t", second.title)
        assertEquals("definition", first.anchor)
        assertEquals("own", second.anchor)
        assertEquals(listOf("shared"), first.attributes.classes)
        assertEquals(listOf("shared", "mine"), second.attributes.classes)
    }
}
