package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertSame
import kotlin.test.assertTrue

private fun payloadBytes(vararg parts: Any): ByteArray {
    val out = mutableListOf<Byte>()
    for (part in parts) {
        when (part) {
            is String -> out += part.encodeToByteArray().toList()
            is Byte -> out += part
            is Long -> repeat(8) { shift -> out += ((part shr (shift * 8)) and 0xff).toByte() }
            is Int -> repeat(4) { shift -> out += ((part shr (shift * 8)) and 0xff).toByte() }
            else -> error("unsupported payload part")
        }
    }
    return out.toByteArray()
}

class PayloadDecoderTest {
    @Test
    fun definitionsRequireBodiesAndTypedListMembers() {
        fun payload(
            bodyCount: Int = 1,
            compact: Byte = 1,
            childKind: Byte = 38,
        ): ByteArray =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                2,
                1,
                -1,
                0,
                0,
                0.toByte(),
                1,
                37.toByte(),
                1,
                1,
                2,
                1,
                -1,
                0,
                0,
                1,
                childKind,
                1,
                1,
                2,
                1,
                -1,
                0,
                0,
                *if (childKind == 38.toByte()) arrayOf<Any>(compact, 0, bodyCount, 0) else arrayOf<Any>(0),
                0,
                0,
            )
        val bytes = payload()
        val definition =
            (
                PayloadDecoder
                    .decode(
                        bytes,
                    ).content
                    .single() as DefinitionList
            ).definitions.single()
        bytes.fill(0)
        assertTrue(definition.compact)
        assertTrue(definition.term.isEmpty())
        assertEquals(listOf(emptyList()), definition.content)
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(bodyCount = 0)) }
        assertFailsWith<IllegalStateException> { PayloadDecoder.decode(payload(compact = 2)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(childKind = 3)) }
    }

    @Test
    fun crossLinksKeepOwnedValuesAndRejectWrongDestinationBranches() {
        fun payload(
            branch: Int = 2,
            kind: Int = 32,
            withDimensions: Boolean = true,
            width: Int = 640,
            height: Int = 480,
        ): ByteArray =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0,
                0.toByte(),
                1,
                3.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0,
                1,
                kind.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0,
                branch,
                0,
                2,
                "id",
                0,
                *(if (withDimensions) arrayOf<Any>(1.toByte(), width, 1.toByte(), height) else emptyArray()),
                0,
                0,
            )
        val ordinary = PayloadDecoder.decode(payload(kind = 30, withDimensions = false))
        assertEquals("", assertIs<CrossLink>(assertIs<Paragraph>(ordinary.content.single()).content.single()).label)
        val bytes = payload()
        val document = PayloadDecoder.decode(bytes)
        val link = assertIs<CrossEmbedded>(assertIs<Paragraph>(document.content.single()).content.single())
        assertEquals("", link.label)
        assertEquals("", assertIs<Destination.Cross>(link.dest).path)
        bytes.fill(0)
        assertEquals("id", assertIs<Destination.Cross>(link.dest).anchor)
        assertEquals(Dimensions(640, 480), link.dimensions)
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(kind = 30)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(width = 0)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(height = 0)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(1)) }
    }

    @Test
    fun metadataAndOccurrenceFieldsSurviveThePayloadLifetime() {
        fun string(value: String): Array<Any> = arrayOf(value.encodeToByteArray().size, value)

        fun scope(): Array<Any> = arrayOf(1, 1, 1, 4)

        fun payload(
            scalarKind: Byte = 1,
            width: Int = 640,
            height: Int = 480,
        ): ByteArray =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                *scope(),
                -1,
                0,
                0,
                1.toByte(),
                43.toByte(),
                *scope(),
                -1,
                0,
                0,
                1.toByte(), // name: explicit null
                1.toByte(),
                0.toByte(),
                1.toByte(), // title: boolean
                1.toByte(),
                scalarKind,
                1.toByte(),
                1.toByte(), // subtitle: exact number
                1.toByte(),
                2.toByte(),
                *string("9007199254740993"),
                1.toByte(), // time: text
                1.toByte(),
                3.toByte(),
                *string("中文\nquoted"),
                1.toByte(), // date: empty list
                2.toByte(),
                0,
                1.toByte(), // authors: list
                2.toByte(),
                2,
                1.toByte(),
                *string("1.25"),
                2.toByte(),
                *string(""),
                0.toByte(), // keywords, abstract, state, comment absent
                0.toByte(),
                0.toByte(),
                0.toByte(),
                2, // two image occurrences sharing one resource
                23.toByte(),
                *scope(),
                *string("first"),
                2,
                *string("a"),
                *string("a"),
                2,
                *string("k"),
                *string("1"),
                *string("k"),
                *string("2"),
                0,
                1,
                *string("/u"),
                -1,
                -1, // definition anchor
                0, // definition classes
                0, // definition records
                1.toByte(),
                width,
                1.toByte(),
                height,
                0,
                23.toByte(),
                *scope(),
                -1,
                0,
                0,
                0,
                0.toByte(),
                0,
                0,
                0,
            )
        val bytes = payload()
        val document = PayloadDecoder.decode(bytes)
        bytes.fill(0)
        val metadata = document.metadata!!
        assertEquals(MetadataScalar.Null, assertIs<MetadataValue.Scalar>(metadata.name).value)
        assertEquals(MetadataScalar.Bool(true), assertIs<MetadataValue.Scalar>(metadata.title).value)
        assertEquals(MetadataScalar.Number("9007199254740993"), assertIs<MetadataValue.Scalar>(metadata.subtitle).value)
        assertEquals(MetadataScalar.Text("中文\nquoted"), assertIs<MetadataValue.Scalar>(metadata.time).value)
        assertEquals(emptyList(), assertIs<MetadataValue.List>(metadata.date).items)
        assertEquals(
            listOf(MetadataListItem.Number("1.25"), MetadataListItem.Text("")),
            assertIs<MetadataValue.List>(metadata.authors).items,
        )
        assertEquals(null, metadata.keywords)
        assertEquals(null, metadata.comment)
        val first = document.content[0] as Embedded
        val second = document.content[1] as Embedded
        assertTrue(first.dest === second.dest)
        assertEquals(Dimensions(640, 480), first.dimensions)
        assertEquals(null, second.dimensions)
        assertEquals("first", first.anchor)
        assertEquals(listOf("a", "a"), first.attributes.classes)
        assertEquals(listOf(Record("k", "1"), Record("k", "2")), first.attributes.records)
        assertTrue(document.dump().contains("subtitle=scalar(number(\"9007199254740993\"))"))
        val visitor = RecordingWalkingVisitor()
        document.walk(visitor)
        assertEquals(listOf("enter:Document", "enter:Metadata", "exit:Metadata"), visitor.events.take(3))
        assertFailsWith<IllegalStateException> { PayloadDecoder.decode(payload(scalarKind = 9)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(width = 0)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(height = 0)) }
    }

    @Test
    fun tableWireCarriesGroupsWidthsAndSpans() {
        fun payload(
            head: Int = 1,
            relative: Double = 0.25,
            rowspan: Long = 1,
        ): ByteArray {
            val parts =
                mutableListOf<Any>(
                    "MKJ1",
                    0.toByte(),
                    1.toByte(),
                    1,
                    1,
                    4,
                    1,
                    -1,
                    0,
                    0, // anchor and attributes
                    0.toByte(), // no metadata
                    1,
                    11.toByte(),
                    1,
                    1,
                    4,
                    1,
                    -1,
                    0,
                    0, // anchor and attributes
                    2, // table, two columns
                    1.toByte(),
                    1.toByte(),
                    relative.toBits(),
                    0.toByte(),
                    0.toByte(),
                    head,
                    1,
                    1,
                    0.toByte(), // no caption
                    3,
                ) // group counts and total rows
            repeat(3) { index ->
                parts.addAll(
                    listOf(
                        26.toByte(),
                        index + 1,
                        1,
                        index + 1,
                        1,
                        -1,
                        0,
                        0, // anchor and attributes
                        1,
                        27.toByte(),
                        index + 1,
                        1,
                        index + 1,
                        1,
                        -1,
                        0,
                        0, // anchor and attributes
                        rowspan,
                        2L,
                        0,
                    ),
                )
            }
            parts.addAll(listOf(0, 0)) // document definitions
            return payloadBytes(*parts.toTypedArray())
        }
        val bytes = payload()
        val table = PayloadDecoder.decode(bytes).content.single() as Table
        bytes.fill(0)
        assertEquals(0.25, table.columns[0].relative)
        assertEquals(null, table.columns[1].relative)
        assertEquals(1, table.head.size)
        assertEquals(1, table.content.size)
        assertEquals(1, table.foot.size)
        assertEquals(2, table.foot[0].cells[0].colspan)
        assertTrue(table.dump().contains("TableFoot children=1"))
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(head = -1)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(head = 2)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(relative = Double.NaN)) }
        assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(payload(rowspan = 0)) }
    }

    @Test
    fun specimensShareCitationOwnershipWithoutListState() {
        val payload =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                7,
                8,
                -1,
                0,
                0, // anchor and attributes
                0.toByte(), // no metadata
                1, // document and content
                3.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0, // anchor and attributes
                1, // paragraph
                25.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0, // anchor and attributes
                1, // cite
                40.toByte(),
                1,
                2,
                1,
                7,
                -1,
                0,
                0,
                3.toByte(),
                6,
                "étude",
                0,
                0, // specimen referent, empty affixes
                1,
                41.toByte(),
                3,
                1,
                3,
                8,
                -1,
                0,
                0,
                1,
                "n",
                0, // one footnote, empty body
                2, // specimen definitions
                42.toByte(),
                5,
                1,
                5,
                8,
                -1,
                0,
                0,
                6,
                "étude",
                5,
                0,
                1.toByte(),
                1,
                3.toByte(),
                5,
                5,
                5,
                8,
                -1,
                0,
                0, // anchor and attributes
                1,
                13.toByte(),
                5,
                5,
                5,
                8,
                -1,
                0,
                0, // anchor and attributes
                4,
                "body",
                42.toByte(),
                7,
                1,
                7,
                8,
                -1,
                0,
                0,
                -1,
                0,
                0,
                0.toByte(),
                0, // anonymous, no reset
            )
        val document = PayloadDecoder.decode(payload)
        payload.fill(0)
        assertEquals("n", document.footnotes.single().id)
        assertEquals(listOf("étude", null), document.specimens.map { it.id })
        assertEquals(listOf(5L, null), document.specimens.map { it.start })
        val cite = (document.content.single() as Paragraph).content.single() as Cite
        assertEquals("étude", assertIs<CitationReferent.Specimen>(cite.citations.single().referent).id)
        assertEquals(
            "body",
            (
                (
                    document.specimens
                        .first()
                        .content
                        .single() as Paragraph
                ).content.single() as Text
            ).literal,
        )
        assertTrue(
            document.dump().contains(
                "Specimen scope=5:1..5:8 anchor=null attributes={} id=\"étude\" start=5 children=1",
            ),
        )
        assertTrue(
            document.dump().contains("Specimen scope=7:1..7:8 anchor=null attributes={} id=null start=null children=0"),
        )
        val visitor = RecordingWalkingVisitor()
        document.walk(visitor)
        assertEquals(2, visitor.events.count { it == "enter:Specimen" })
        assertTrue(visitor.events.indexOf("exit:Footnote") < visitor.events.indexOf("enter:Specimen"))
        assertEquals(visitor.entered, visitor.exited)
    }

    @Test
    fun orderedValuesAndUtf8MarkersSurviveWireDecoding() {
        // Reserved values cannot yet be produced by parsing; exercise the wire
        // with all payload combinations, including a four-byte UTF-8 scalar.
        fun payload(
            variant: Int,
            lowercased: Boolean,
            delimiter: Int,
            closed: Boolean,
        ): ByteArray =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                1,
                1,
                -1,
                0,
                0, // anchor and attributes
                0.toByte(), // no metadata
                1, // document scope and one child
                6.toByte(),
                1,
                1,
                1,
                1, // list scope
                -1,
                0,
                0, // anchor and attributes
                2,
                1,
                0,
                1.toByte(), // ordered, start=1 as int64, present
                variant,
                (if (lowercased) 1 else 0).toByte(),
                delimiter,
                (if (closed) 1 else 0).toByte(),
                1.toByte(),
                1,
                7.toByte(),
                1,
                1,
                1,
                1, // item scope
                -1,
                0,
                0, // anchor and attributes
                4,
                "🚀",
                0, // marker, no content
                0, // no document footnotes
                0, // no document specimens
            )
        val delimiters =
            listOf(
                Triple(1, false, OrderedListDelimiter.Period),
                Triple(2, false, OrderedListDelimiter.Parenthesis(false)),
                Triple(2, true, OrderedListDelimiter.Parenthesis(true)),
                Triple(3, false, OrderedListDelimiter.Default),
            )
        for (variant in listOf(2, 3)) {
            for (lowercased in listOf(false, true)) {
                for ((delimiter, closed, expected) in delimiters) {
                    val bytes = payload(variant, lowercased, delimiter, closed)
                    val document = PayloadDecoder.decode(bytes)
                    bytes.fill(0)
                    val list = document.content.single() as List
                    val decodedLowercased =
                        if (variant == 2) {
                            assertIs<OrderedListVariant.Alpha>(list.variant).lowercased
                        } else {
                            assertIs<OrderedListVariant.Roman>(list.variant).lowercased
                        }
                    assertEquals(lowercased, decodedLowercased)
                    when (expected) {
                        OrderedListDelimiter.Period -> {
                            assertEquals(expected, list.delimiter)
                        }

                        OrderedListDelimiter.Default -> {
                            assertEquals(expected, list.delimiter)
                        }

                        is OrderedListDelimiter.Parenthesis -> {
                            assertEquals(
                                closed,
                                assertIs<OrderedListDelimiter.Parenthesis>(list.delimiter).closed,
                            )
                        }
                    }
                    assertEquals("🚀", list.items.single().marker)
                    val spelling =
                        when (expected) {
                            OrderedListDelimiter.Period -> "period"
                            OrderedListDelimiter.Default -> "default"
                            is OrderedListDelimiter.Parenthesis -> "parenthesis(closed=${expected.closed})"
                        }
                    assertTrue(document.dump().contains("delimiter=$spelling"))
                    assertTrue(document.dump().contains("marker=\"🚀\""))
                }
            }
        }
        assertFailsWith<IllegalStateException> { PayloadDecoder.decode(payload(2, true, 99, false)) }
    }

    @Test
    fun aTitleIsDecodedBeforeTheContentAndDumpedAsAGroup() {
        // The title path of the wire: a node-valued list the payload sends
        // between the callout's metadata and its content. This payload is
        // built by hand to isolate the transport: a document holding
        // one collapsed `note` callout whose title is the text `T` and whose
        // content is empty.
        val payload =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0, // anchor and attributes
                0.toByte(), // no metadata
                1,
                2.toByte(),
                1,
                1,
                1,
                8,
                -1,
                0,
                0, // anchor and attributes
                4,
                "note",
                1.toByte(),
                1,
                13.toByte(),
                1,
                10,
                1,
                10,
                -1,
                0,
                0, // anchor and attributes
                1,
                "T",
                0,
                0,
                0,
            )
        val document = PayloadDecoder.decode(payload)
        val callout = document.content.single() as Callout
        assertEquals("note", callout.variant)
        assertEquals(true, callout.collapsed)
        assertEquals("T", (callout.title!!.single() as Text).literal)
        assertEquals(emptyList(), callout.content)
        assertEquals(
            "Document scope=1:1..1:8 anchor=null attributes={} children=1\n" +
                "└── Callout scope=1:1..1:8 anchor=null attributes={} variant=\"note\" collapsed=true children=0\n" +
                "    └── Title children=1\n" +
                "        └── Text scope=1:10..1:10 anchor=null attributes={} literal=\"T\" children=0\n",
            document.dump(),
        )
        val events = mutableListOf<String>()
        callout.walk(
            object : MarkupVisitor by RecordingWalkingVisitor() {
                override fun visit(
                    callout: Callout,
                    phase: MarkupVisitPhase,
                ) {
                    events += "$phase:Callout"
                }

                override fun visit(
                    text: Text,
                    phase: MarkupVisitPhase,
                ) {
                    events += "$phase:Text"
                }
            },
        )
        assertEquals(listOf("ENTER:Callout", "ENTER:Text", "EXIT:Text", "EXIT:Callout"), events)
    }

    @Test
    fun aCiteCarriesItsItemsAndTheDocumentItsFootnotesAsValues() {
        // The value paths of the wire (M4): a cite's items follow it as a
        // counted list -- scope, referent branch and fields, prefix, suffix --
        // and the document's footnotes follow its content the same way. No
        // parse produces the `bib` branch or a non-empty affix until P7, so
        // the payload is built by hand: a paragraph holding one cite whose
        // item names bib key `k` in author-in-text mode with the prefix
        // `see ` and the suffix `p. 3`, then one footnote `n` holding `note`.
        val text: (Int, Int, Int, Int, String) -> Array<Any> = { l1, c1, l2, c2, literal ->
            arrayOf(13.toByte(), l1, c1, l2, c2, -1, 0, 0, literal.encodeToByteArray().size, literal)
        }
        val payload =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                3,
                9,
                -1,
                0,
                0, // anchor and attributes
                0.toByte(), // no metadata
                1,
                3.toByte(),
                1,
                1,
                1,
                20,
                -1,
                0,
                0, // anchor and attributes
                1,
                25.toByte(),
                1,
                1,
                1,
                20,
                -1,
                0,
                0, // anchor and attributes
                1,
                40.toByte(),
                1,
                2,
                1,
                19,
                -1,
                0,
                0,
                1.toByte(),
                1,
                "k",
                2,
                1,
                *text(1, 2, 1, 5, "see "),
                1,
                *text(1, 10, 1, 13, "p. 3"),
                1,
                41.toByte(),
                3,
                1,
                3,
                9,
                -1,
                0,
                0,
                1,
                "n",
                1,
                3.toByte(),
                3,
                6,
                3,
                9,
                -1,
                0,
                0, // anchor and attributes
                1,
                *text(3, 6, 3, 9, "note"),
                0, // no document specimens
            )
        val document = PayloadDecoder.decode(payload)
        val cite = (document.content.single() as Paragraph).content.single() as Cite
        val citation = cite.citations.single()
        val referent = citation.referent as CitationReferent.Bib
        assertEquals("k", referent.key)
        assertEquals(BibMode.AUTHOR_IN_TEXT, referent.mode)
        assertEquals("see ", (citation.prefix.single() as Text).literal)
        assertEquals("p. 3", (citation.suffix.single() as Text).literal)
        assertEquals("n", document.footnotes.single().id)
        assertEquals(
            "Document scope=1:1..3:9 anchor=null attributes={} children=1\n" +
                "├── Paragraph scope=1:1..1:20 anchor=null attributes={} children=1\n" +
                "│   └── Cite scope=1:1..1:20 anchor=null attributes={} children=1\n" +
                "│       └── Citation scope=1:2..1:19 " +
                "anchor=null attributes={} referent=bib(key=\"k\",mode=authorInText) children=0\n" +
                "│           ├── CitationPrefix children=1\n" +
                "│           │   └── Text scope=1:2..1:5 anchor=null attributes={} literal=\"see \" children=0\n" +
                "│           └── CitationSuffix children=1\n" +
                "│               └── Text scope=1:10..1:13 anchor=null attributes={} literal=\"p. 3\" children=0\n" +
                "└── Footnote scope=3:1..3:9 anchor=null attributes={} id=\"n\" children=1\n" +
                "    └── Paragraph scope=3:6..3:9 anchor=null attributes={} children=1\n" +
                "        └── Text scope=3:6..3:9 anchor=null attributes={} literal=\"note\" children=0\n",
            document.dump(),
        )
        val visitor = RecordingWalkingVisitor()
        cite.walk(visitor)
        assertEquals(
            listOf(
                "enter:Cite",
                "enter:Citation",
                "enter:Text",
                "exit:Text",
                "enter:Text",
                "exit:Text",
                "exit:Citation",
                "exit:Cite",
            ),
            visitor.events,
        )
    }

    @Test
    fun decodingDepthIsBoundedByTheHeapAndTypedListsRejectAtTheKind() {
        // A payload the parser cannot produce -- 200 000 nested emphasis
        // nodes around one text -- decodes without touching the JVM stack:
        // the decoder holds one frame per open container in arrays, so the
        // depth costs the arrays, not a closure per level or a stack frame.
        val depth = 200_000
        val header = byteArrayOf(0x4d, 0x4b, 0x4a, 0x31, 0)
        val node = { kind: Byte, tail: ByteArray -> payloadBytes(kind, 1, 1, 1, 1, -1, 0, 0, *tail.toTypedArray()) }
        val emphasis = node(19, payloadBytes(1))
        val payload =
            ByteArray(
                header.size +
                    node(
                        1,
                        payloadBytes(0.toByte(), 1),
                    ).size + node(3, payloadBytes(1)).size + depth * emphasis.size +
                    node(13, payloadBytes(1, "x")).size +
                    8,
            )
        var offset = 0
        for (part in listOf(header, node(1, payloadBytes(0.toByte(), 1)), node(3, payloadBytes(1)))) {
            part.copyInto(payload, offset)
            offset += part.size
        }
        repeat(depth) {
            emphasis.copyInto(payload, offset)
            offset += emphasis.size
        }
        for (part in listOf(node(13, payloadBytes(1, "x")), payloadBytes(0, 0))) {
            part.copyInto(payload, offset)
            offset += part.size
        }
        assertEquals(payload.size, offset)
        val document = PayloadDecoder.decode(payload)
        payload.fill(0)
        var current: Markup = assertIs<Paragraph>(document.content.single()).content.single()
        repeat(depth) {
            val level = assertIs<Emphasis>(current)
            assertSame(Attributes.empty, level.attributes)
            assertTrue(level.content is RandomAccess)
            current = level.content.single()
        }
        assertEquals("x", assertIs<Text>(current).literal)
        val visitor = RecordingWalkingVisitor(recordEvents = false)
        document.walk(visitor)
        assertEquals(depth + 3, visitor.entered)
        assertEquals(visitor.entered, visitor.exited)

        // A list rejects a member of the wrong kind as its kind byte is read,
        // before the member's fields are: each payload ends right after that
        // byte, so a decoder that read on would report the truncation instead.
        fun container(
            kind: Byte,
            scalars: Array<Any>,
            memberKind: Byte,
        ): ByteArray =
            payloadBytes(
                "MKJ1",
                0.toByte(),
                1.toByte(),
                1,
                1,
                1,
                1,
                -1,
                0,
                0,
                0.toByte(),
                1,
                kind,
                1,
                1,
                1,
                1,
                -1,
                0,
                0,
                *scalars,
                1,
                memberKind,
            )
        val list = arrayOf<Any>(1, 0L, 0.toByte(), 0, 0.toByte(), 0, 0.toByte(), 1.toByte())
        assertEquals(
            "list contains a non-item node",
            assertFailsWith<IllegalArgumentException> { PayloadDecoder.decode(container(6, list, 3)) }.message,
        )
        assertEquals(
            "owned node in ordinary content",
            assertFailsWith<IllegalArgumentException> {
                PayloadDecoder.decode(
                    container(3, emptyArray(), 41),
                )
            }.message,
        )
        assertEquals(
            "truncated payload",
            assertFailsWith<IllegalArgumentException> {
                PayloadDecoder.decode(
                    container(3, emptyArray(), 13),
                )
            }.message,
        )
    }

    @Test
    fun corruptedPayloadFailsInsteadOfProducingAPartialTree() {
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode(byteArrayOf(0x4d, 0x4b, 0x4a))
        }
    }

    @Test
    fun malformedJniPayloadValuesAreRejectedBeforeTheyEnterTheAst() {
        assertFailsWith<IllegalArgumentException> { PayloadNodeKind.from(0) }
        assertFailsWith<IllegalArgumentException> { PayloadNodeKind.from(-1) }
        assertFailsWith<IllegalArgumentException> { PayloadNodeKind.from(255) }
        assertFailsWith<IllegalArgumentException> {
            PayloadNodeKind.from(PayloadNodeKind.entries.maxOf { it.rawValue } + 1)
        }
        assertEquals(PayloadNodeKind.COMMENT, PayloadNodeKind.from(29))
        assertEquals(PayloadNodeKind.CROSS_LINK, PayloadNodeKind.from(30))
        assertEquals(PayloadNodeKind.CROSS_EMBEDDED, PayloadNodeKind.from(32))
        assertEquals(PayloadNodeKind.CITE, PayloadNodeKind.from(25))
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode("MKJ1".encodeToByteArray())
        }
    }

    @Test
    fun failedAndStructurallyInvalidPayloadsAreRejected() {
        val failure =
            assertFailsWith<ParseException> {
                PayloadDecoder.decode(payloadBytes("MKJ1", 1.toByte(), 1, 3, "bad"))
            }
        assertEquals(ParseErrorCode.INVALID_ARGUMENT, failure.code)
        assertEquals("bad", failure.message)
        assertEquals(
            ParseErrorCode.INTERNAL,
            assertFailsWith<ParseException> {
                PayloadDecoder.decode(payloadBytes("MKJ1", 1.toByte(), 99, 1, "x"))
            }.code,
        )
        val allocationFailure =
            assertFailsWith<ParseException> {
                PayloadDecoder.decode(payloadBytes("MKJ1", 1.toByte(), 2, 13, "out of memory"))
            }
        assertEquals(ParseErrorCode.ALLOCATION_FAILED, allocationFailure.code)
        assertEquals("out of memory", allocationFailure.message)

        assertFailsWith<IllegalStateException> {
            PayloadDecoder.decode(payloadBytes("MKJ1", 2.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode(payloadBytes("MKJ0", 0.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode(payloadBytes("MKJ1", 0.toByte(), 3.toByte()))
        }
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode(payloadBytes("MKJ1", 0.toByte(), 1.toByte(), 1, 1))
        }
        assertFailsWith<IllegalArgumentException> {
            PayloadDecoder.decode(payloadBytes("MKJ1", 1.toByte(), 1, -2))
        }
    }
}
