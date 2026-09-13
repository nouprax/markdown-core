package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

class ApiTest {
    @Test
    fun embeddedCrossLinksShareDimensionsAndKeepRawPrefixes() {
        val document = Document.parse("![[v.mp4|*raw*|2147483647x2]] ![[n|3]] [[n|100]] ![[n|bad|01]] ![[n]]\n")
        val links = assertIs<Paragraph>(document.content.single()).content.filterIsInstance<CrossEmbedded>()
        assertEquals(listOf(Dimensions(2147483647, 2), Dimensions(3), null, null), links.map { it.dimensions })
        assertEquals(
            "100",
            assertIs<Paragraph>(document.content.single())
                .content
                .filterIsInstance<CrossLink>()
                .single()
                .label,
        )
        assertEquals(listOf("*raw*", "", "bad|01", null), links.map { it.label })
    }

    @Test
    fun imageDimensionsBelongToOccurrencesWithSharedDestinations() {
        val document = Document.parse("![*alt*|2147483647x2][r] ![3][r] ![bad|01][r]\n\n[r]: /shared \"title\"\n")
        val images = assertIs<Paragraph>(document.content.single()).content.filterIsInstance<Embedded>()
        assertEquals(listOf(Dimensions(2147483647, 2), Dimensions(3), null), images.map { it.dimensions })
        assertEquals(1, setOf(Dimensions(640, 480), Dimensions(640, 480)).size)
        assertFailsWith<IllegalArgumentException> { Dimensions(0) }
        assertFailsWith<IllegalArgumentException> { Dimensions(1, 0) }
        assertSame(images[0].dest, images[1].dest)
        assertSame(images[1].dest, images[2].dest)
        assertEquals("title", images[0].title)
        val alt = assertIs<Emphasis>(images[0].content.single())
        assertEquals("alt", assertIs<Text>(alt.content.single()).literal)
        assertEquals(7, alt.scope.end.column)
        assertTrue(images[1].content.isEmpty())
        assertEquals("bad|01", assertIs<Text>(images[2].content.single()).literal)
        val visitor = RecordingWalkingVisitor()
        images[0].walk(visitor)
        assertEquals(
            listOf(
                "entering:Embedded",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Embedded",
            ),
            visitor.events,
        )
    }

    @Test
    fun propertiesKeepRecognizedFieldsAndLiteralProse() {
        val source =
            "---\r\nname: 9007199254740993\r\nnot YAML\r\n...\r\nunknown: ignored\r\n" +
                "comment: *x\r\nname: duplicate\r\nabstract: |\r\n  first\r\n\r\n  second\r\n" +
                "comment: |\r\n  # prose\r\n---\r\nbody\r\n"
        val document = Document.parse(source)
        val metadata = assertNotNull(document.metadata)
        assertEquals(
            listOf(
                MetadataValue.Scalar(MetadataScalar.Number("9007199254740993")),
                MetadataValue.Scalar(MetadataScalar.Text("first\n\nsecond\n")),
                MetadataValue.Scalar(MetadataScalar.Text("# prose\n")),
            ),
            listOf(metadata.name, metadata.`abstract`, metadata.comment),
        )
        assertEquals(14, metadata.scope.end.line)
        assertEquals(
            15,
            document.content[0]
                .scope.start.line,
        )
        val empty = assertNotNull(Document.parse("---\nunknown: 1\nfree text\n---").metadata)
        assertEquals(Metadata(scope = empty.scope), empty)
        assertEquals(null, Document.parse("---\nname: 1\n").metadata)
    }

    @Test
    fun taskMarkersPreserveScalarsAndDeriveCompletion() {
        for (marker in listOf(" ", "x", "X", "?", "é", "✓", "🚀", "́", "]")) {
            val item = assertIs<List>(Document.parse("- [$marker] body\n").content.single()).items.single()
            assertEquals(marker, item.marker)
            assertEquals(true, item.tasked)
            assertEquals(marker != " ", item.completed)
        }
        val item = assertIs<List>(Document.parse("- [é] body\n").content.single()).items.single()
        assertEquals(null, item.marker)
        assertEquals(false, item.tasked)
        assertEquals(false, item.completed)
    }

    @Test
    fun theDialectHasNoSwitches() {
        // One witness per feature that used to sit behind a `ParseOptions`
        // field, and one for the substitution smart punctuation used to make:
        // a plain parse recognizes all of them, and there is nothing to pass.
        assertIs<Table>(
            Document
                .parse("| a |\n| --- |\n| b |\n")
                .content
                .first(),
        )
        val witnesses =
            listOf(
                "~~x~~\n" to "Strikethrough scope=",
                "www.example.com\n" to "Link scope=",
                "- [x] task\n" to "marker=\"x\"",
                "ref[^a]\n\n[^a]: note\n" to "Cite scope=",
                "\$x\$\n" to "Formula scope=",
                ":badge[label]\n" to "Directive scope=",
                "\"quotes\" -- ...\n" to "literal=\"\\\"quotes\\\" -- ...\"",
            )
        for ((source, witness) in witnesses) {
            assertTrue(Document.parse(source).dump().contains(witness), "expected $witness for $source")
        }
    }

    @Test
    fun visitorIsTypedAndDispatchesByNodeKind() {
        val document = Document.parse("# Heading\n\nBody\n")
        val visitor = KindVisitor()
        assertEquals("heading:1", document.content.first().accept(visitor))
        assertEquals("Document", document.accept(visitor))
        assertEquals("Paragraph", document.content.last().accept(visitor))
    }

    @Test
    fun marksRetainTypedContentAndWalkBothPhasesAfterNativeRelease() {
        val paragraph = Document.parse("==a *b*==").content.first() as Paragraph
        val mark = paragraph.content.first() as Mark
        val visitor = RecordingWalkingVisitor()
        mark.walk(visitor)
        assertEquals(
            listOf(
                "entering:Mark",
                "entering:Text",
                "exiting:Text",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Mark",
            ),
            visitor.events,
        )
        assertEquals(2, mark.content.size)
        assertEquals("b", ((mark.content[1] as Emphasis).content.first() as Text).literal)
        assertEquals(Scope(Position(1, 1), Position(1, 9)), mark.scope)
    }

    @Test
    fun insertionsRetainTypedContentAndWalkBothPhasesAfterNativeRelease() {
        val paragraph = Document.parse("++a *b*++").content.first() as Paragraph
        val insertion = paragraph.content.first() as Insertion
        val visitor = RecordingWalkingVisitor()
        insertion.walk(visitor)
        assertEquals(
            listOf(
                "entering:Insertion",
                "entering:Text",
                "exiting:Text",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Insertion",
            ),
            visitor.events,
        )
        assertEquals(2, insertion.content.size)
        assertEquals("b", ((insertion.content[1] as Emphasis).content.first() as Text).literal)
        assertEquals(Scope(Position(1, 1), Position(1, 9)), insertion.scope)
    }

    @Test
    fun spansRetainTypedContentAndWalkBothPhasesAfterNativeRelease() {
        val paragraph = Document.parse("[a *b*]{}").content.first() as Paragraph
        val span = paragraph.content.first() as Span
        val visitor = RecordingWalkingVisitor()
        span.walk(visitor)
        assertEquals(
            listOf(
                "entering:Span",
                "entering:Text",
                "exiting:Text",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Span",
            ),
            visitor.events,
        )
        assertEquals(2, span.content.size)
        assertEquals("b", ((span.content[1] as Emphasis).content.first() as Text).literal)
        assertEquals(Scope(Position(1, 1), Position(1, 9)), span.scope)
    }

    @Test
    fun superscriptsRetainTypedContentAndWalkBothPhasesAfterNativeRelease() {
        val paragraph = Document.parse("^a*b*^").content.first() as Paragraph
        val superscript = paragraph.content.first() as Superscript
        val visitor = RecordingWalkingVisitor()
        superscript.walk(visitor)
        assertEquals(
            listOf(
                "entering:Superscript",
                "entering:Text",
                "exiting:Text",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Superscript",
            ),
            visitor.events,
        )
        assertEquals(2, superscript.content.size)
        assertEquals("b", ((superscript.content[1] as Emphasis).content.first() as Text).literal)
        assertEquals(Scope(Position(1, 1), Position(1, 6)), superscript.scope)
    }

    @Test
    fun subscriptsRetainTypedContentAndWalkBothPhasesAfterNativeRelease() {
        val paragraph = Document.parse("~a*b*~").content.first() as Paragraph
        val subscript = paragraph.content.first() as Subscript
        val visitor = RecordingWalkingVisitor()
        subscript.walk(visitor)
        assertEquals(
            listOf(
                "entering:Subscript",
                "entering:Text",
                "exiting:Text",
                "entering:Emphasis",
                "entering:Text",
                "exiting:Text",
                "exiting:Emphasis",
                "exiting:Subscript",
            ),
            visitor.events,
        )
        assertEquals(2, subscript.content.size)
        assertEquals("b", ((subscript.content[1] as Emphasis).content.first() as Text).literal)
        assertEquals(Scope(Position(1, 1), Position(1, 6)), subscript.scope)
    }

    @Test
    fun walkingVisitorIsTypedAndPreservesOwnedFieldSemantics() {
        val block = assertIs<DirectiveBlock>(Document.parse(":::note[Title]\nBody\n:::\n").content.single())
        val visitor = RecordingWalkingVisitor()
        block.walk(visitor)

        assertEquals(
            listOf(
                "entering:DirectiveBlock",
                "entering:DirectiveLabel",
                "entering:Text",
                "exiting:Text",
                "exiting:DirectiveLabel",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:DirectiveBlock",
            ),
            visitor.events,
        )
        assertEquals(listOf("Paragraph"), block.content.map { it::class.simpleName })

        val table = assertIs<Table>(Document.parse("| a |\n| --- |\n| b |\n").content.single())
        val tableVisitor = RecordingWalkingVisitor()
        table.walk(tableVisitor)
        assertEquals(listOf(1, 3), tableVisitor.tableRowKinds)
    }
}

class UnicodeTest {
    @Test
    fun standardUtf8SurvivesTheNativeBoundary() {
        val document = Document.parse("héllo 🚀 中文\n")
        val paragraph = assertIs<Paragraph>(document.content.first())
        assertEquals("héllo 🚀 中文", assertIs<Text>(paragraph.content.first()).literal)
    }
}

class ErrorsTest {
    @Test
    fun emptyInputIsAValidDocument() {
        assertEquals(Scope(Position(1, 1), Position(0, 0)), Document.parse("").scope)
        assertEquals(Scope(Position(1, 1), Position(1, 2)), Document.parse("é").scope)
        assertTrue(
            Document
                .parse("")
                .content
                .isEmpty(),
        )
    }
}

class BindingMappingTest {
    @Test
    fun ordinaryQuoteIsAMetadataFreeCallout() {
        val document = Document.parse("> quote\n")
        val callout = assertIs<Callout>(document.content.single())
        assertEquals(null, callout.variant)
        assertEquals(null, callout.collapsed)
        assertEquals(null, callout.title)
        assertEquals(1, callout.content.size)
        assertEquals(
            "Document scope=1:1..1:7 anchor=null attributes={} children=1\n" +
                "└── Callout scope=1:1..1:7 anchor=null attributes={} variant=null collapsed=null children=1\n" +
                "    └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1\n" +
                "        └── Text scope=1:3..1:7 anchor=null attributes={} literal=\"quote\" children=0\n",
            document.dump(),
        )
    }

    @Test
    fun authoredCalloutTitleWalksBeforeBodyAfterNativeRelease() {
        val callout = assertIs<Callout>(Document.parse("> [!CuStOm]- **T**\n> body\n").content.single())
        assertEquals("CuStOm", callout.variant)
        assertEquals(true, callout.collapsed)
        val title = assertIs<Strong>(callout.title!!.single())
        assertEquals("T", assertIs<Text>(title.content.single()).literal)
        assertIs<Paragraph>(callout.content.single())
        val visitor = RecordingWalkingVisitor()
        callout.walk(visitor)
        assertEquals(
            listOf(
                "entering:Callout",
                "entering:Strong",
                "entering:Text",
                "exiting:Text",
                "exiting:Strong",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:Callout",
            ),
            visitor.events,
        )
        val values = Document.parse("> [!note]+ %%t%%\n\n> [!note]\n").content
        val comment = assertIs<Callout>(values[0])
        assertEquals(false, comment.collapsed)
        assertEquals("t", assertIs<Comment>(comment.title!!.single()).literal)
        assertTrue(comment.content.isEmpty())
        val empty = assertIs<Callout>(values[1])
        assertEquals(null, empty.title)
        assertEquals(null, empty.collapsed)
    }

    @Test
    fun extendedKindsDecodeDumpAndWalk() {
        // One document verifies the extended node kinds and their semantic
        // fields through decode, dump, and traversal.
        val source =
            listOf(
                "[foo]: /url \"t\"",
                "",
                ":::note[Title]{kind=demo}",
                "Body",
                ":::",
                "",
                "See [foo], [foo][foo], ![foo] and \$\$x\$\$.",
                "",
                "3. one",
                "4. two",
                "",
                "| a | b | c | d |",
                "| :- | :-: | -: | --- |",
                "| 1 | 2 | 3 | 4 |",
                "",
            ).joinToString("\n")
        val document = Document.parse(source)

        // M2: the definition produces no node, and every reference form is
        // the Link or Embedded it names, with the definition's destination and
        // title.
        val block = assertIs<DirectiveBlock>(document.content[0])
        assertIs<DirectiveLabel>(assertNotNull(block.label))
        assertEquals(1, block.content.size)
        assertIs<Paragraph>(block.content.single())
        assertEquals(
            "kind",
            block.attributes.records
                .first()
                .name,
        )

        val inlines = assertIs<Paragraph>(document.content[1]).content
        val links = inlines.filterIsInstance<Link>()
        assertEquals(2, links.size)
        for (link in links) {
            assertEquals("/url", assertIs<Destination.Url>(link.dest).value)
            assertEquals("t", link.title)
        }
        assertSame(links[0].dest, links[1].dest, "one definition materializes one resource")
        val image = inlines.filterIsInstance<Embedded>().single()
        assertEquals("/url", assertIs<Destination.Url>(image.dest).value)
        assertSame(links[0].dest, image.dest, "an image reference shares the definition's resource too")
        assertEquals(Placement.STANDALONE, inlines.filterIsInstance<Formula>().single().mode)

        // Fully qualified: the model's `List` shadows `kotlin.collections.List`.
        val list = assertIs<com.nouprax.markdown.core.List>(document.content[2])
        assertEquals(ListFlavor.ORDERED, list.flavor)
        assertEquals(3, list.start)

        val table = assertIs<Table>(document.content[3])
        assertEquals(
            listOf(
                TableAlignment.LEFT,
                TableAlignment.CENTER,
                TableAlignment.RIGHT,
                TableAlignment.NONE,
            ),
            table.columns.map { it.alignment },
        )

        // The owning node keeps its label field separate from block content;
        // the per-node dumper deliberately emits both relations.
        val dump = document.dump()
        for (fragment in listOf("Link scope=", "Embedded scope=", "DirectiveLabel")) {
            assertTrue(dump.contains(fragment), "dump is missing $fragment")
        }
        assertEquals(listOf("Paragraph"), block.content.map { it::class.simpleName })
        assertEquals(listOf("Text"), assertNotNull(block.label).content.map { it::class.simpleName })
    }

    @Test
    fun inlineFootnotesKeepDirectContentSourceIdsAndFiniteVisitation() {
        val document = Document.parse("^[^[x]]\n\n[^inline-1]: authored\n")
        assertEquals(listOf("inline-1-1", "inline-2", "inline-1"), document.footnotes.map { it.id })
        val outer = assertIs<Cite>(assertIs<Paragraph>(document.content.single()).content.single())
        assertEquals("inline-1-1", assertIs<CitationReferent.Footnote>(outer.citations.single().referent).id)
        val inner = assertIs<Cite>(document.footnotes[0].content.single())
        assertEquals("inline-2", assertIs<CitationReferent.Footnote>(inner.citations.single().referent).id)
        assertEquals("x", assertIs<Text>(document.footnotes[1].content.single()).literal)
        assertIs<Paragraph>(document.footnotes[2].content.single())
        val visitor = RecordingWalkingVisitor()
        document.walk(visitor)
        assertEquals(
            listOf(
                "entering:Document",
                "entering:Paragraph",
                "entering:Cite",
                "entering:Citation",
                "exiting:Citation",
                "exiting:Cite",
                "exiting:Paragraph",
                "entering:Footnote",
                "entering:Cite",
                "entering:Citation",
                "exiting:Citation",
                "exiting:Cite",
                "exiting:Footnote",
                "entering:Footnote",
                "entering:Text",
                "exiting:Text",
                "exiting:Footnote",
                "entering:Footnote",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:Footnote",
                "exiting:Document",
            ),
            visitor.events,
        )
    }

    @Test
    fun citationsAreValuesAndTheDocumentOwnsItsFootnotes() {
        // M4: an inherited call is a one-item cite naming its footnote by id
        // with empty affixes; the footnote is a value the document owns, never
        // content, and the walk reaches it after the content. Repeated calls
        // share one footnote: the first definition of an id is the one they
        // resolve to, and a later definition of the same id is a footnote
        // after it, as the inherited grammar parses it.
        val document = Document.parse("[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n")
        val cites = assertIs<Paragraph>(document.content.single()).content.filterIsInstance<Cite>()
        assertEquals(2, cites.size)
        for (cite in cites) {
            val citation = cite.citations.single()
            assertEquals("a", assertIs<CitationReferent.Footnote>(citation.referent).id)
            assertEquals(emptyList(), citation.prefix)
            assertEquals(emptyList(), citation.suffix)
        }
        assertEquals(listOf("a", "a"), document.footnotes.map { it.id })
        val footnote = document.footnotes.first()
        assertEquals(Scope(Position(3, 1), Position(4, 0)), footnote.scope)
        assertEquals("once", assertIs<Text>(assertIs<Paragraph>(footnote.content.single()).content.single()).literal)
        val later = document.footnotes.last()
        assertEquals(Scope(Position(5, 1), Position(5, 11)), later.scope)
        assertEquals("twice", assertIs<Text>(assertIs<Paragraph>(later.content.single()).content.single()).literal)
        assertTrue(
            document.dump().endsWith(
                "└── Footnote scope=5:1..5:11 id=\"a\" children=1\n" +
                    "    └── Paragraph scope=5:7..5:11 anchor=null attributes={} children=1\n" +
                    "        └── Text scope=5:7..5:11 anchor=null attributes={} literal=\"twice\" children=0\n",
            ),
        )
        assertTrue(document.dump().startsWith("Document scope=1:1..5:11 anchor=null attributes={} children=1\n"))

        val visitor = RecordingWalkingVisitor()
        document.walk(visitor)
        val cite = listOf("entering:Cite", "entering:Citation", "exiting:Citation", "exiting:Cite")
        val footnoteEvents =
            listOf(
                "entering:Footnote",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:Footnote",
            )
        assertEquals(
            listOf("entering:Document", "entering:Paragraph") + cite + listOf("entering:Text", "exiting:Text") + cite +
                listOf("exiting:Paragraph") + footnoteEvents + footnoteEvents + listOf("exiting:Document"),
            visitor.events,
        )
    }

    @Test
    fun aDirectiveBlockWithNoLabelTakesTheOtherArm() {
        val bare = assertIs<DirectiveBlock>(Document.parse(":::note\nBody\n:::\n").content.single())
        assertEquals(null, bare.label)
        assertTrue(bare.dump().contains("children=1"))
    }

    @Test
    fun everyOptionalFieldIsReadBothPresentAndAbsent() {
        // Requirement 14 gives every optional field two answers and the decoder
        // an arm for each. A corpus that always writes the field takes one arm
        // and never the other, so write both and compare them side by side.
        val withEverything =
            Document.parse(
                listOf(
                    "``` kotlin",
                    "code",
                    "```",
                    "",
                    "[a](/u \"t\") ![b](/s \"u\") `c`",
                    "",
                    ":::note{k=v}",
                    "body",
                    ":::",
                    "",
                    "- [x] done",
                    "",
                ).joinToString("\n"),
            )
        val withNothing =
            Document.parse(
                listOf(
                    "```",
                    "code",
                    "```",
                    "",
                    "[a](/u) ![b](/s)",
                    "",
                    ":::note",
                    "body",
                    ":::",
                    "",
                    "- plain",
                    "",
                ).joinToString("\n"),
            )

        val fenced = assertIs<CodeBlock>(withEverything.content[0])
        assertEquals("kotlin", fenced.language)
        assertEquals("kotlin", fenced.info)
        val bare = assertIs<CodeBlock>(withNothing.content[0])
        assertEquals(null, bare.language)
        assertEquals(null, bare.info)

        val rich = assertIs<Paragraph>(withEverything.content[1]).content
        assertEquals("t", rich.filterIsInstance<Link>().single().title)
        assertEquals("u", rich.filterIsInstance<Embedded>().single().title)
        val plain = assertIs<Paragraph>(withNothing.content[1]).content
        assertEquals(null, plain.filterIsInstance<Link>().single().title)
        assertEquals(null, plain.filterIsInstance<Embedded>().single().title)

        assertEquals(listOf(Record("k", "v")), assertIs<DirectiveBlock>(withEverything.content[2]).attributes.records)
        assertEquals(emptyList(), assertIs<DirectiveBlock>(withNothing.content[2]).attributes.records)

        val checked = assertIs<com.nouprax.markdown.core.List>(withEverything.content[3])
        assertEquals("x", checked.items.single().marker)
        assertEquals(true, checked.items.single().tasked)
        assertEquals(true, checked.items.single().completed)
        val unchecked = assertIs<com.nouprax.markdown.core.List>(withNothing.content[3])
        assertEquals(null, unchecked.items.single().marker)
        assertEquals(false, unchecked.items.single().tasked)
        assertEquals(false, unchecked.items.single().completed)
        val parenthesized = assertIs<com.nouprax.markdown.core.List>(Document.parse("1) item\n").content.single())
        assertEquals(false, assertIs<OrderedListDelimiter.Parenthesis>(parenthesized.delimiter).closed)
    }

    @Test
    fun theDumpEscapesEveryCharacterJsonCannotCarryLiterally() {
        // A fenced code block carries its literal through untouched, so it is
        // the one place a test can put every escape the dumper knows.
        val literal = "a\"b\\c\td\u0008e\u000cf\u0001g"
        val dump = Document.parse("```\n$literal\n```\n").dump()
        for (escape in listOf("\\\"", "\\\\", "\\t", "\\b", "\\f", "\\n", "\\u0001")) {
            assertTrue(dump.contains(escape), "dump is missing the escape $escape")
        }
    }
}

class OwnershipTest {
    @Test
    fun returnedTreesOutliveEveryNativeDocument() {
        val documents = kotlin.collections.List(300) { Document.parse("# Copy\n\n- [x] item\n") }
        assertTrue(documents.all { it.content.size == 2 })
        assertEquals(1, assertIs<Heading>(documents.last().content.first()).level)
    }

    @Test
    fun readOnlyCollectionsDoNotLeakMutableImplementations() {
        val content = Document.parse("one *two* three\n").content
        assertFailsWith<ClassCastException> {
            @Suppress("UNCHECKED_CAST")
            (content as MutableList<Markup>).clear()
        }
    }
}

class RobustnessTest {
    @Test
    fun largeDocumentsCopyCompletelyBeforeNativeRelease() {
        val unit = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n"
        val document = Document.parse(unit.repeat(5_000))
        assertEquals(10_000, document.content.size)
    }

    @Test
    fun uncappedListNestingRemainsTraversable() {
        val depth = 10_000
        val document = Document.parse("- ".repeat(depth) + "leaf\n")
        val visitor = RecordingWalkingVisitor(recordEvents = false)
        document.walk(visitor)
        assertEquals(visitor.entered, visitor.exited)
        assertTrue(visitor.entered > depth * 2)

        var node: Markup =
            document
                .content
                .single()
        repeat(depth) {
            val list = assertIs<List>(node)
            node =
                list.items
                    .single()
                    .content
                    .single()
        }
        assertIs<Paragraph>(node)
    }

    @Test
    fun repeatedParseAndReleaseRemainsStable() {
        repeat(2_000) {
            assertEquals(
                2,
                Document
                    .parse("# Copy\n\n- [x] item 🚀\n")
                    .content.size,
            )
        }
    }

    @Test
    fun forwardHeadingReferencesShareFinalTargetAfterNativeRelease() {
        val anchor = "a".repeat(1024)
        val count = 5_000
        val document = Document.parse("[Target]\n\n".repeat(count) + "# Target {#$anchor .heading k=1}\n")
        val links = document.content.take(count).map { assertIs<Link>(assertIs<Paragraph>(it).content.single()) }
        assertEquals(anchor, document.content[count].anchor)
        val destination = links.first().dest
        assertEquals("#$anchor", assertIs<Destination.Url>(destination).value)
        for (link in links) {
            assertSame(destination, link.dest)
            assertEquals(null, link.anchor)
            assertEquals(null, link.title)
            assertTrue(link.attributes.classes.isEmpty())
            assertTrue(link.attributes.records.isEmpty())
        }
    }

    @Test
    fun everyOccurrenceOfOneDefinitionMaterializesOneResource() {
        // M2: the C tree shares one resource across every occurrence of a
        // definition, the JNI payload sends it once, and both decoders reuse
        // the one value they built for it.
        val destination = "/" + "u".repeat(1024)
        val count = 5_000
        val anchor = "a".repeat(1024)
        val classes = " .c".repeat(1024)
        val document =
            Document.parse("[a]: $destination {#$anchor$classes k=$destination}\n\n" + "[a]\n\n".repeat(count))
        val links = document.content.map { assertIs<Link>(assertIs<Paragraph>(it).content.single()) }
        assertEquals(count, links.size)
        assertEquals(anchor, links[0].anchor)
        assertEquals(1024, links[0].attributes.classes.size)
        assertTrue(links.all { it.attributes === links[0].attributes })
        val first = links.first().dest
        assertEquals(destination, assertIs<Destination.Url>(first).value)
        assertTrue(links.all { it.dest === first }, "every occurrence materializes the one resource")
    }

    @Test
    fun attributeSitesKeepNativeValuesAndOccurrenceScopes() {
        val document =
            Document.parse(
                "# T ## {#heading}\n\n`x`{.code} [x][r]{#own .same k=2} ![alt|20x30][r]{width=50% height=2in}\n\n[r]: /u {#definition .same k=1 k=1}\n",
            )
        assertEquals("heading", document.content[0].anchor)
        val paragraph = assertIs<Paragraph>(document.content[1])
        val code = assertIs<Code>(paragraph.content[0])
        val link = assertIs<Link>(paragraph.content[2])
        val image = assertIs<Embedded>(paragraph.content[4])
        assertEquals(listOf("code"), code.attributes.classes)
        assertEquals(10, code.scope.end.column)
        assertEquals("own", link.anchor)
        assertEquals(listOf("same", "same"), link.attributes.classes)
        assertEquals(listOf("1", "1", "2"), link.attributes.records.map { it.value })
        assertEquals("definition", image.anchor)
        assertEquals(Dimensions(20, 30), image.dimensions)
        assertEquals(
            listOf("50%", "2in"),
            image.attributes.records
                .takeLast(2)
                .map { it.value },
        )
        assertEquals(3, link.scope.end.line)
        assertEquals(3, image.scope.end.line)
    }
}
