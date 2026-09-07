@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import cnames.structs.markdown_core_citation
import cnames.structs.markdown_core_error
import cnames.structs.markdown_core_footnote
import cnames.structs.markdown_core_node
import cnames.structs.markdown_core_resource
import cnames.structs.markdown_core_specimen
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_BIB_MODE_NORMAL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_DESTINATION_CROSS
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_DESTINATION_URL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_ALLOCATION_FAILED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_INTERNAL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_INVALID_ARGUMENT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CALLOUT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CITE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CODE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CODE_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_COMMENT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CROSS_LINK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE_LABEL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DOCUMENT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_EMPHASIS
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FORMULA
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FORMULA_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HEADING
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HTML
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HTML_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_IMAGE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LINE_BREAK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LINK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LIST
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LIST_ITEM
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_MARK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_PARAGRAPH
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_SOFT_BREAK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_STRIKETHROUGH
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_STRONG
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_TABLE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_TABLE_CELL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_TABLE_ROW
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_TEXT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_THEMATIC_BREAK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_LIST_FLAVOR_BULLET
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_LIST_FLAVOR_ORDERED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_BOOL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_ITEM_NUMBER
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_ITEM_TEXT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_LIST
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_NULL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_NUMBER
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_SCALAR
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_METADATA_TEXT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_PLACEMENT_EMBEDDED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_PLACEMENT_STANDALONE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENT_BIB
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENT_FOOTNOTE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENT_SPECIMEN
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT
import com.nouprax.markdown.core.internal.capi.markdown_core_citation_next
import com.nouprax.markdown.core.internal.capi.markdown_core_citation_prefix
import com.nouprax.markdown.core.internal.capi.markdown_core_citation_referent
import com.nouprax.markdown.core.internal.capi.markdown_core_citation_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_citation_suffix
import com.nouprax.markdown.core.internal.capi.markdown_core_destination
import com.nouprax.markdown.core.internal.capi.markdown_core_document_free
import com.nouprax.markdown.core.internal.capi.markdown_core_document_parse
import com.nouprax.markdown.core.internal.capi.markdown_core_document_root
import com.nouprax.markdown.core.internal.capi.markdown_core_error_free
import com.nouprax.markdown.core.internal.capi.markdown_core_error_get_code
import com.nouprax.markdown.core.internal.capi.markdown_core_error_get_message
import com.nouprax.markdown.core.internal.capi.markdown_core_footnote_content
import com.nouprax.markdown.core.internal.capi.markdown_core_footnote_id
import com.nouprax.markdown.core.internal.capi.markdown_core_footnote_next
import com.nouprax.markdown.core.internal.capi.markdown_core_footnote_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_list_flavorVar
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_list_item
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_at
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_count
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_item_at
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_item_count
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_kind
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_name
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_scalar
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_record_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_scalar
import com.nouprax.markdown.core.internal.capi.markdown_core_metadata_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_node_anchor
import com.nouprax.markdown.core.internal.capi.markdown_core_node_attribute_class_at
import com.nouprax.markdown.core.internal.capi.markdown_core_node_attribute_class_count
import com.nouprax.markdown.core.internal.capi.markdown_core_node_attribute_record_at
import com.nouprax.markdown.core.internal.capi.markdown_core_node_attribute_record_count
import com.nouprax.markdown.core.internal.capi.markdown_core_node_callout_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_callout_title
import com.nouprax.markdown.core.internal.capi.markdown_core_node_child_count
import com.nouprax.markdown.core.internal.capi.markdown_core_node_cite_citations
import com.nouprax.markdown.core.internal.capi.markdown_core_node_code_block_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_cross_link_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_destination
import com.nouprax.markdown.core.internal.capi.markdown_core_node_directive_label
import com.nouprax.markdown.core.internal.capi.markdown_core_node_directive_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_document_footnotes
import com.nouprax.markdown.core.internal.capi.markdown_core_node_document_metadata
import com.nouprax.markdown.core.internal.capi.markdown_core_node_document_specimens
import com.nouprax.markdown.core.internal.capi.markdown_core_node_formula_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_first_child
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_kind
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_next_sibling
import com.nouprax.markdown.core.internal.capi.markdown_core_node_heading_level
import com.nouprax.markdown.core.internal.capi.markdown_core_node_image_dimensions
import com.nouprax.markdown.core.internal.capi.markdown_core_node_list_item_marker
import com.nouprax.markdown.core.internal.capi.markdown_core_node_list_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_literal
import com.nouprax.markdown.core.internal.capi.markdown_core_node_resource
import com.nouprax.markdown.core.internal.capi.markdown_core_node_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_cell_spans
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_column_at
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_title
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_bool
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_i64
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_string
import com.nouprax.markdown.core.internal.capi.markdown_core_ordered_list_delimiter
import com.nouprax.markdown.core.internal.capi.markdown_core_ordered_list_variant
import com.nouprax.markdown.core.internal.capi.markdown_core_placement_modeVar
import com.nouprax.markdown.core.internal.capi.markdown_core_referent
import com.nouprax.markdown.core.internal.capi.markdown_core_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_specimen_content
import com.nouprax.markdown.core.internal.capi.markdown_core_specimen_next
import com.nouprax.markdown.core.internal.capi.markdown_core_specimen_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_specimen_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_string
import com.nouprax.markdown.core.internal.capi.markdown_core_table_column
import kotlinx.cinterop.BooleanVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.CValue
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.MemScope
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.useContents
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar

internal actual fun parsePlatformDocument(source: ByteArray): Document =
    memScoped {
        val error = alloc<CPointerVar<markdown_core_error>>()
        error.value = null
        val document =
            if (source.isEmpty()) {
                markdown_core_document_parse(null, 0u, error.ptr)
            } else {
                source.usePinned { pinned ->
                    markdown_core_document_parse(
                        pinned.addressOf(0).reinterpret(),
                        source.size.toULong(),
                        error.ptr,
                    )
                }
            }
        if (document == null) {
            val nativeError = requireNotNull(error.value) { "native parser failed without an error" }
            try {
                val code =
                    when (markdown_core_error_get_code(nativeError)) {
                        MARKDOWN_CORE_ERROR_INVALID_ARGUMENT -> ParseErrorCode.INVALID_ARGUMENT
                        MARKDOWN_CORE_ERROR_ALLOCATION_FAILED -> ParseErrorCode.ALLOCATION_FAILED
                        MARKDOWN_CORE_ERROR_INTERNAL -> ParseErrorCode.INTERNAL
                        else -> ParseErrorCode.INTERNAL
                    }
                val message = markdown_core_error_get_message(nativeError).useContents { copyString() }
                throw ParseException(code, message)
            } finally {
                markdown_core_error_free(nativeError)
            }
        }

        try {
            val root = requireNotNull(markdown_core_document_root(document)) { "native document has no root" }
            val markup = NativeTreeBuilder(root, NativeScratch(this)).build()
            require(markup is Document) { "native document root has the wrong kind" }
            markup
        } finally {
            markdown_core_document_free(document)
        }
    }

private data class NativeNodeRecord(
    val pointer: CPointer<markdown_core_node>,
    var childStart: Int = 0,
    var childCount: Int = 0,
    var labelIndex: Int = -1,
    var titleStart: Int = 0,
    var titleCount: Int = 0,
    /** The document's footnote records or the cite's citation records: a start and a count. */
    var valueStart: Int = 0,
    var valueCount: Int = 0,
    var specimenStart: Int = 0,
    var specimenCount: Int = 0,
)

/** One item a cite owns; its prefix and suffix nodes are recorded like children. */
private class NativeCitationRecord(
    val pointer: CPointer<markdown_core_citation>,
    val prefixStart: Int,
    val prefixCount: Int,
    val suffixStart: Int,
    val suffixCount: Int,
)

/** One footnote the document owns; its content nodes are recorded like children. */
private class NativeFootnoteRecord(
    val pointer: CPointer<markdown_core_footnote>,
    val contentStart: Int,
    val contentCount: Int,
)

private class NativeSpecimenRecord(
    val pointer: CPointer<markdown_core_specimen>,
    val contentStart: Int,
    val contentCount: Int,
)

/** Copies the C tree iteratively while the immutable native document is alive. */
private class NativeTreeBuilder(
    root: CPointer<markdown_core_node>,
    private val scratch: NativeScratch,
) {
    private val records = mutableListOf(NativeNodeRecord(root))
    private val citationRecords = mutableListOf<NativeCitationRecord>()
    private val footnoteRecords = mutableListOf<NativeFootnoteRecord>()
    private val specimenRecords = mutableListOf<NativeSpecimenRecord>()
    private lateinit var built: Array<Markup?>

    /**
     * Every occurrence of one reference definition shares one resource in the
     * C tree, and its identity keys one materialization here, so a long
     * destination referenced many times is copied out of C once.
     */
    private val resources = HashMap<CPointer<markdown_core_resource>, Pair<Destination, String?>>()

    private fun resource(node: CPointer<markdown_core_node>): Pair<Destination, String?> {
        val identity = requireNotNull(markdown_core_node_resource(node)) { "invalid link or image node" }
        return resources.getOrPut(identity) { scratch.destination(node) to scratch.title(node) }
    }

    fun build(): Markup {
        collectRelations()
        built = arrayOfNulls(records.size)
        for (index in records.indices.reversed()) built[index] = materialize(index)
        return requireNotNull(built[0])
    }

    private fun collectRelations() {
        var index = 0
        while (index < records.size) {
            val record = records[index]
            when (markdown_core_node_get_kind(record.pointer)) {
                MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK,
                MARKDOWN_CORE_KIND_DIRECTIVE,
                -> {
                    markdown_core_node_directive_label(record.pointer)?.let { label ->
                        record.labelIndex = records.size
                        records += NativeNodeRecord(label)
                    }
                }

                MARKDOWN_CORE_KIND_CALLOUT -> {
                    // The title is a sibling chain the callout owns beside its
                    // content; its nodes are recorded like children, and the
                    // record remembers which are the title's. A present title
                    // holds at least one node, so its count is its presence.
                    record.titleStart = records.size
                    record.titleCount = recordChain(markdown_core_node_callout_title(record.pointer))
                }

                MARKDOWN_CORE_KIND_DOCUMENT -> {
                    // The footnotes are values the document owns beside its
                    // content (M4); each one's content is recorded like children.
                    record.valueStart = footnoteRecords.size
                    var footnote = markdown_core_node_document_footnotes(record.pointer)
                    while (footnote != null) {
                        val contentStart = records.size
                        val contentCount = recordChain(markdown_core_footnote_content(footnote))
                        footnoteRecords += NativeFootnoteRecord(footnote, contentStart, contentCount)
                        record.valueCount++
                        footnote = markdown_core_footnote_next(footnote)
                    }
                    record.specimenStart = specimenRecords.size
                    var specimen = markdown_core_node_document_specimens(record.pointer)
                    while (specimen != null) {
                        val contentStart = records.size
                        val contentCount = recordChain(markdown_core_specimen_content(specimen))
                        specimenRecords += NativeSpecimenRecord(specimen, contentStart, contentCount)
                        record.specimenCount++
                        specimen = markdown_core_specimen_next(specimen)
                    }
                }

                MARKDOWN_CORE_KIND_CITE -> {
                    // The items are values the cite owns (M4); each one's
                    // prefix and suffix are recorded like children.
                    record.valueStart = citationRecords.size
                    var citation = markdown_core_node_cite_citations(record.pointer)
                    while (citation != null) {
                        val prefixStart = records.size
                        val prefixCount = recordChain(markdown_core_citation_prefix(citation))
                        val suffixStart = records.size
                        val suffixCount = recordChain(markdown_core_citation_suffix(citation))
                        citationRecords +=
                            NativeCitationRecord(citation, prefixStart, prefixCount, suffixStart, suffixCount)
                        record.valueCount++
                        citation = markdown_core_citation_next(citation)
                    }
                    require(record.valueCount >= 1) { "native cite holds no citation" }
                }
            }
            record.childStart = records.size
            var child = markdown_core_node_get_first_child(record.pointer)
            while (child != null) {
                records += NativeNodeRecord(child)
                record.childCount++
                child = markdown_core_node_get_next_sibling(child)
            }
            require(record.childCount == markdown_core_node_child_count(record.pointer).checkedSize("child count")) {
                "native child sequence does not match its count"
            }
            index++
        }
    }

    /** Records every node of a sibling chain a value owns and answers how many there were. */
    private fun recordChain(first: CPointer<markdown_core_node>?): Int {
        var count = 0
        var node = first
        while (node != null) {
            records += NativeNodeRecord(node)
            count++
            node = markdown_core_node_get_next_sibling(node)
        }
        return count
    }

    private fun materialize(index: Int): Markup {
        val record = records[index]
        val node = record.pointer
        val kind = markdown_core_node_get_kind(node)
        val scope = nativeScope(node)
        val anchor = markdown_core_node_anchor(node).useContents { copyOptionalString() }
        val attributes = scratch.attributes(node)
        val children = children(record)
        return when (kind) {
            MARKDOWN_CORE_KIND_DOCUMENT -> {
                Document(
                    children,
                    scratch.metadata(node),
                    footnotes(record),
                    specimens(record),
                    scope,
                    anchor,
                    attributes,
                )
            }

            MARKDOWN_CORE_KIND_CALLOUT -> {
                val (variant, collapsed) = scratch.callout(node)
                Callout(variant, collapsed, title(record), children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_PARAGRAPH -> {
                Paragraph(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_HEADING -> {
                Heading(scratch.headingLevel(node), children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_THEMATIC_BREAK -> {
                ThematicBreak(scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_LIST -> {
                scratch.list(node, children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_LIST_ITEM -> {
                ListItem(scratch.listItemMarker(node), children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_CODE_BLOCK -> {
                scratch.codeBlock(node, scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_HTML_BLOCK -> {
                HTMLBlock(scratch.literal(node), scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_FORMULA_BLOCK -> {
                val formula = scratch.formula(node)
                require(formula.first == PlacementMode.STANDALONE) { "formula block is not standalone" }
                FormulaBlock(formula.second, scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_TABLE -> {
                scratch.table(node, children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK -> {
                scratch.directiveBlock(node, label(record), children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_TEXT -> {
                Text(scratch.literal(node), scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_SOFT_BREAK -> {
                SoftBreak(scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_LINE_BREAK -> {
                LineBreak(scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_CODE -> {
                Code(scratch.literal(node), scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_HTML -> {
                HTML(scratch.literal(node), scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_CROSS_LINK -> {
                val fields = scratch.crossLink(node)
                CrossLink(fields.first, scratch.destination(node), fields.second, scope, anchor, attributes)
                    .also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_COMMENT -> {
                Comment(scratch.literal(node), scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_FORMULA -> {
                val formula = scratch.formula(node)
                Formula(formula.first, formula.second, scope, anchor, attributes).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_EMPHASIS -> {
                Emphasis(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_STRONG -> {
                Strong(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_STRIKETHROUGH -> {
                Strikethrough(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_MARK -> {
                Mark(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_LINK -> {
                val resource = resource(node)
                Link(resource.first, resource.second, children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_IMAGE -> {
                val resource = resource(node)
                scratch.dimensions(node).let { (width, height) ->
                    Image(resource.first, resource.second, width, height, children, scope, anchor, attributes)
                }
            }

            MARKDOWN_CORE_KIND_DIRECTIVE -> {
                scratch.directive(node, label(record), children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_CITE -> {
                requireLeaf(children, kind)
                Cite(citations(record), scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_TABLE_ROW -> {
                scratch.tableRow(children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_TABLE_CELL -> {
                scratch.tableCell(node, children, scope, anchor, attributes)
            }

            MARKDOWN_CORE_KIND_DIRECTIVE_LABEL -> {
                DirectiveLabel(children, scope, anchor, attributes)
            }

            else -> {
                error("unsupported native node kind $kind")
            }
        }
    }

    private fun children(record: NativeNodeRecord): kotlin.collections.List<Markup> =
        nodes(record.childStart, record.childCount, "child")

    private fun title(record: NativeNodeRecord): kotlin.collections.List<Markup>? {
        if (record.titleCount == 0) return null
        return nodes(record.titleStart, record.titleCount, "callout title")
    }

    private fun footnotes(record: NativeNodeRecord): kotlin.collections.List<Footnote> =
        immutableList(record.valueCount) { offset ->
            val footnote = footnoteRecords[record.valueStart + offset]
            Footnote(
                scratch.footnoteId(footnote.pointer),
                nodes(footnote.contentStart, footnote.contentCount, "footnote content"),
                markdown_core_footnote_scope(footnote.pointer).toScope(),
            )
        }

    private fun specimens(record: NativeNodeRecord): kotlin.collections.List<Specimen> =
        immutableList(record.specimenCount) { offset ->
            val specimen = specimenRecords[record.specimenStart + offset]
            scratch.specimen(
                specimen.pointer,
                nodes(specimen.contentStart, specimen.contentCount, "specimen content"),
                markdown_core_specimen_scope(specimen.pointer).toScope(),
            )
        }

    private fun citations(record: NativeNodeRecord): kotlin.collections.List<Citation> =
        immutableList(record.valueCount) { offset ->
            val citation = citationRecords[record.valueStart + offset]
            Citation(
                scratch.referent(citation.pointer),
                nodes(citation.prefixStart, citation.prefixCount, "citation prefix"),
                nodes(citation.suffixStart, citation.suffixCount, "citation suffix"),
                markdown_core_citation_scope(citation.pointer).toScope(),
            )
        }

    private fun nodes(
        start: Int,
        count: Int,
        what: String,
    ): kotlin.collections.List<Markup> =
        immutableList(count) { offset ->
            requireNotNull(built[start + offset]) { "native $what was not materialized" }
        }

    private fun label(record: NativeNodeRecord): DirectiveLabel? {
        if (record.labelIndex < 0) return null
        val value = requireNotNull(built[record.labelIndex]) { "native directive label was not materialized" }
        require(value is DirectiveLabel) { "directive label field contains a non-label node" }
        return value
    }

    private fun nativeScope(node: CPointer<markdown_core_node>): Scope = markdown_core_node_scope(node).toScope()

    private fun requireLeaf(
        children: kotlin.collections.List<Markup>,
        kind: UInt,
    ) {
        require(children.isEmpty()) { "native leaf kind $kind contains children" }
    }
}

internal fun decodeNativeListDelimiter(value: markdown_core_ordered_list_delimiter): OrderedListDelimiter =
    when (value.kind) {
        MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD -> OrderedListDelimiter.Period
        MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS -> OrderedListDelimiter.Parenthesis(value.closed)
        MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT -> OrderedListDelimiter.Default
        else -> error("unsupported native list delimiter ${value.kind}")
    }

private class NativeScratch(
    scope: MemScope,
) {
    private val firstString = scope.alloc<markdown_core_string>()
    private val imageWidth = scope.alloc<markdown_core_optional_i64>()
    private val imageHeight = scope.alloc<markdown_core_optional_i64>()
    private val metadataScalar = scope.alloc<markdown_core_metadata_scalar>()
    private val metadataItem = scope.alloc<markdown_core_metadata_list_item>()
    private val secondString = scope.alloc<markdown_core_string>()
    private val thirdString = scope.alloc<markdown_core_string>()
    private val firstOptionalString = scope.alloc<markdown_core_optional_string>()
    private val secondOptionalString = scope.alloc<markdown_core_optional_string>()
    private val optionalLong = scope.alloc<markdown_core_optional_i64>()
    private val optionalBoolean = scope.alloc<markdown_core_optional_bool>()
    private val firstBoolean = scope.alloc<BooleanVar>()
    private val secondBoolean = scope.alloc<BooleanVar>()
    private val integer = scope.alloc<IntVar>()
    private val count = scope.alloc<size_tVar>()
    private val listFlavor = scope.alloc<markdown_core_list_flavorVar>()
    private val listVariant = scope.alloc<markdown_core_ordered_list_variant>()
    private val listDelimiter = scope.alloc<markdown_core_ordered_list_delimiter>()
    private val placementMode = scope.alloc<markdown_core_placement_modeVar>()
    private val tableColumn = scope.alloc<markdown_core_table_column>()
    private val tableHead = scope.alloc<size_tVar>()
    private val tableContent = scope.alloc<size_tVar>()
    private val tableFoot = scope.alloc<size_tVar>()
    private val tableRowspan = scope.alloc<kotlinx.cinterop.LongVar>()
    private val tableColspan = scope.alloc<kotlinx.cinterop.LongVar>()
    private val destination = scope.alloc<markdown_core_destination>()
    private val referent = scope.alloc<markdown_core_referent>()

    fun headingLevel(node: CPointer<markdown_core_node>): Int {
        require(markdown_core_node_heading_level(node, integer.ptr)) { "invalid heading node" }
        return integer.value
    }

    fun list(
        node: CPointer<markdown_core_node>,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): List {
        require(
            markdown_core_node_list_properties(
                node,
                listFlavor.ptr,
                optionalLong.ptr,
                listVariant.ptr,
                listDelimiter.ptr,
                firstBoolean.ptr,
            ),
        ) {
            "invalid list node"
        }
        val flavor =
            when (listFlavor.value) {
                MARKDOWN_CORE_LIST_FLAVOR_BULLET -> ListFlavor.BULLET
                MARKDOWN_CORE_LIST_FLAVOR_ORDERED -> ListFlavor.ORDERED
                else -> error("unsupported native list flavor ${listFlavor.value}")
            }
        val items = children.immutableMap { requireNotNull(it as? ListItem) { "list contains a non-item node" } }
        val variant =
            when (listVariant.kind) {
                MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA -> OrderedListVariant.Alpha(listVariant.lowercased)
                MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN -> OrderedListVariant.Roman(listVariant.lowercased)
                MARKDOWN_CORE_ORDERED_LIST_VARIANT_DEFAULT -> OrderedListVariant.Default
                else -> OrderedListVariant.Decimal
            }.takeIf { optionalLong.has_value }
        val delimiter = decodeNativeListDelimiter(listDelimiter).takeIf { optionalLong.has_value }
        return List(
            flavor,
            optionalLong.value.takeIf { optionalLong.has_value },
            variant,
            delimiter,
            firstBoolean.value,
            items,
            scope,
            anchor,
            attributes,
        )
    }

    fun listItemMarker(node: CPointer<markdown_core_node>): String? {
        require(markdown_core_node_list_item_marker(node, firstOptionalString.ptr)) {
            "invalid list item node"
        }
        return firstOptionalString.copyOptionalString()
    }

    fun codeBlock(
        node: CPointer<markdown_core_node>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): CodeBlock {
        require(
            markdown_core_node_code_block_properties(
                node,
                firstOptionalString.ptr,
                secondOptionalString.ptr,
                thirdString.ptr,
                firstBoolean.ptr,
                secondBoolean.ptr,
            ),
        ) { "invalid code block node" }
        return CodeBlock(
            firstOptionalString.copyOptionalString(),
            secondOptionalString.copyOptionalString(),
            thirdString.copyString(),
            firstBoolean.value,
            secondBoolean.value,
            scope,
            anchor,
            attributes,
        )
    }

    fun literal(node: CPointer<markdown_core_node>): String {
        require(markdown_core_node_literal(node, firstString.ptr)) { "invalid literal node" }
        return firstString.copyString()
    }

    fun callout(node: CPointer<markdown_core_node>): Pair<String?, Boolean?> {
        require(markdown_core_node_callout_properties(node, firstOptionalString.ptr, optionalBoolean.ptr)) {
            "invalid callout node"
        }
        return firstOptionalString.copyOptionalString() to optionalBoolean.value.takeIf { optionalBoolean.has_value }
    }

    fun formula(node: CPointer<markdown_core_node>): Pair<PlacementMode, String> {
        require(markdown_core_node_formula_properties(node, placementMode.ptr, firstString.ptr)) {
            "invalid formula node"
        }
        val mode =
            when (placementMode.value) {
                MARKDOWN_CORE_PLACEMENT_EMBEDDED -> PlacementMode.EMBEDDED
                MARKDOWN_CORE_PLACEMENT_STANDALONE -> PlacementMode.STANDALONE
                else -> error("unsupported native placement mode ${placementMode.value}")
            }
        return mode to firstString.copyString()
    }

    fun table(
        node: CPointer<markdown_core_node>,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): Table {
        require(markdown_core_node_table_properties(node, count.ptr, tableHead.ptr, tableContent.ptr, tableFoot.ptr)) {
            "invalid table node"
        }
        val columns =
            immutableList(count.value.checkedSize("table column count")) { index ->
                require(
                    markdown_core_node_table_column_at(node, index.toULong(), tableColumn.ptr),
                ) { "invalid table column" }
                val alignment =
                    when (tableColumn.alignment) {
                        MARKDOWN_CORE_TABLE_ALIGNMENT_NONE -> TableAlignment.NONE
                        MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT -> TableAlignment.LEFT
                        MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER -> TableAlignment.CENTER
                        MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT -> TableAlignment.RIGHT
                        else -> error("unsupported native table alignment ${tableColumn.alignment}")
                    }
                TableColumn(alignment, tableColumn.relative.value.takeIf { tableColumn.relative.has_value })
            }
        val head = tableHead.value.checkedSize("table head count")
        val content = tableContent.value.checkedSize("table content count")
        val foot = tableFoot.value.checkedSize("table foot count")
        require(head.toLong() + content + foot == children.size.toLong()) { "invalid table row groups" }
        val rows = children.immutableMap { requireNotNull(it as? TableRow) { "table contains a non-row node" } }
        return Table(
            columns,
            immutableList(head) { rows[it] },
            immutableList(content) { rows[head + it] },
            immutableList(foot) { rows[head + content + it] },
            scope,
            anchor,
            attributes,
        )
    }

    fun tableRow(
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): TableRow {
        val cells = children.immutableMap { requireNotNull(it as? TableCell) { "table row contains a non-cell node" } }
        return TableRow(cells, scope, anchor, attributes)
    }

    fun tableCell(
        node: CPointer<markdown_core_node>,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): TableCell {
        require(markdown_core_node_table_cell_spans(node, tableRowspan.ptr, tableColspan.ptr)) { "invalid table cell" }
        require(tableRowspan.value in 1..Int.MAX_VALUE.toLong() && tableColspan.value in 1..Int.MAX_VALUE.toLong()) {
            "invalid table cell spans"
        }
        return TableCell(tableRowspan.value.toInt(), tableColspan.value.toInt(), children, scope, anchor, attributes)
    }

    fun directiveBlock(
        node: CPointer<markdown_core_node>,
        label: DirectiveLabel?,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): DirectiveBlock {
        val properties = directiveProperties(node)
        return DirectiveBlock(properties, label, children, scope, anchor, attributes)
    }

    fun directive(
        node: CPointer<markdown_core_node>,
        label: DirectiveLabel?,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
        anchor: String?,
        attributes: Attributes,
    ): Directive {
        require(children.isEmpty()) { "inline directive contains block content" }
        val properties = directiveProperties(node)
        return Directive(properties, label, scope, anchor, attributes)
    }

    private fun directiveProperties(node: CPointer<markdown_core_node>): String {
        require(markdown_core_node_directive_properties(node, firstString.ptr)) { "invalid directive node" }
        return firstString.copyString()
    }

    fun attributes(node: CPointer<markdown_core_node>): Attributes {
        val classes =
            immutableList(markdown_core_node_attribute_class_count(node).checkedSize("class count")) { index ->
                require(
                    markdown_core_node_attribute_class_at(node, index.toULong(), firstString.ptr),
                ) { "invalid class" }
                firstString.copyString()
            }
        val records =
            immutableList(markdown_core_node_attribute_record_count(node).checkedSize("record count")) { index ->
                require(
                    markdown_core_node_attribute_record_at(node, index.toULong(), firstString.ptr, secondString.ptr),
                ) {
                    "invalid record"
                }
                Record(firstString.copyString(), secondString.copyString())
            }
        return Attributes(classes, records)
    }

    fun dimensions(node: CPointer<markdown_core_node>): Pair<Int?, Int?> {
        require(
            markdown_core_node_image_dimensions(node, imageWidth.ptr, imageHeight.ptr),
        ) { "invalid image dimensions" }

        fun dimension(value: markdown_core_optional_i64): Int? {
            if (!value.has_value) return null
            require(value.value in 1..Int.MAX_VALUE.toLong()) { "invalid image dimension" }
            return value.value.toInt()
        }
        return dimension(imageWidth) to dimension(imageHeight)
    }

    fun metadata(node: CPointer<markdown_core_node>): Metadata? {
        val metadata = markdown_core_node_document_metadata(node) ?: return null
        val records =
            immutableList(markdown_core_metadata_record_count(metadata).checkedSize("metadata count")) { index ->
                val record = requireNotNull(markdown_core_metadata_record_at(metadata, index.toULong()))
                val name = markdown_core_metadata_record_name(record).useContents { copyString() }
                val value =
                    when (markdown_core_metadata_record_kind(record)) {
                        MARKDOWN_CORE_METADATA_SCALAR -> {
                            require(
                                markdown_core_metadata_record_scalar(record, metadataScalar.ptr),
                            ) { "invalid metadata scalar" }
                            MetadataValue.Scalar(
                                when (metadataScalar.kind) {
                                    MARKDOWN_CORE_METADATA_NULL -> {
                                        MetadataScalar.Null
                                    }

                                    MARKDOWN_CORE_METADATA_BOOL -> {
                                        MetadataScalar.Bool(metadataScalar.value.boolean)
                                    }

                                    MARKDOWN_CORE_METADATA_NUMBER -> {
                                        MetadataScalar.Number(
                                            metadataScalar.value.string.copyString(),
                                        )
                                    }

                                    MARKDOWN_CORE_METADATA_TEXT -> {
                                        MetadataScalar.Text(
                                            metadataScalar.value.string.copyString(),
                                        )
                                    }

                                    else -> {
                                        error("invalid metadata scalar kind")
                                    }
                                },
                            )
                        }

                        MARKDOWN_CORE_METADATA_LIST -> {
                            MetadataValue.List(
                                immutableList(
                                    markdown_core_metadata_record_item_count(record).checkedSize("metadata list count"),
                                ) { itemIndex ->
                                    require(
                                        markdown_core_metadata_record_item_at(
                                            record,
                                            itemIndex.toULong(),
                                            metadataItem.ptr,
                                        ),
                                    ) {
                                        "invalid metadata item"
                                    }
                                    when (metadataItem.kind) {
                                        MARKDOWN_CORE_METADATA_ITEM_NUMBER -> {
                                            MetadataListItem.Number(
                                                metadataItem.value.copyString(),
                                            )
                                        }

                                        MARKDOWN_CORE_METADATA_ITEM_TEXT -> {
                                            MetadataListItem.Text(
                                                metadataItem.value.copyString(),
                                            )
                                        }

                                        else -> {
                                            error("invalid metadata item kind")
                                        }
                                    }
                                },
                            )
                        }

                        else -> {
                            error("invalid metadata value kind")
                        }
                    }
                MetadataRecord(name, value, markdown_core_metadata_record_scope(record).toScope())
            }
        return Metadata(records, markdown_core_metadata_scope(metadata).toScope())
    }

    fun destination(node: CPointer<markdown_core_node>): Destination {
        require(markdown_core_node_destination(node, destination.ptr)) { "invalid link or image node" }
        return when (destination.kind) {
            MARKDOWN_CORE_DESTINATION_URL -> {
                Destination.Url(destination.url.copyString())
            }

            MARKDOWN_CORE_DESTINATION_CROSS -> {
                Destination.Cross(destination.path.copyString(), destination.anchor.copyOptionalString())
            }

            else -> {
                error("unsupported native destination kind ${destination.kind}")
            }
        }
    }

    fun crossLink(node: CPointer<markdown_core_node>): Pair<Boolean, String?> {
        require(markdown_core_node_cross_link_properties(node, firstBoolean.ptr, firstOptionalString.ptr)) {
            "invalid cross link"
        }
        return firstBoolean.value to firstOptionalString.copyOptionalString()
    }

    fun title(node: CPointer<markdown_core_node>): String? {
        require(markdown_core_node_title(node, firstOptionalString.ptr)) { "invalid link or image node" }
        return firstOptionalString.copyOptionalString()
    }

    /** A branch's fields exist only in that branch, so only they are copied. */
    fun specimen(
        pointer: CPointer<markdown_core_specimen>,
        content: kotlin.collections.List<Markup>,
        scope: Scope,
    ): Specimen {
        require(
            markdown_core_specimen_properties(pointer, firstOptionalString.ptr, optionalLong.ptr),
        ) { "invalid specimen" }
        return Specimen(
            firstOptionalString.copyOptionalString(),
            optionalLong.value.takeIf {
                optionalLong.has_value
            },
            content,
            scope,
        )
    }

    fun referent(citation: CPointer<markdown_core_citation>): CitationReferent {
        require(markdown_core_citation_referent(citation, referent.ptr)) { "invalid citation" }
        return when (referent.kind) {
            MARKDOWN_CORE_REFERENT_BIB -> {
                CitationReferent.Bib(referent.key.copyString(), bibMode())
            }

            MARKDOWN_CORE_REFERENT_FOOTNOTE -> {
                CitationReferent.Footnote(referent.id.copyString())
            }

            MARKDOWN_CORE_REFERENT_SPECIMEN -> {
                CitationReferent.Specimen(referent.id.copyString())
            }

            else -> {
                error("unsupported native referent kind ${referent.kind}")
            }
        }
    }

    private fun bibMode(): BibMode =
        when (referent.mode) {
            MARKDOWN_CORE_BIB_MODE_NORMAL -> BibMode.NORMAL
            MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT -> BibMode.AUTHOR_IN_TEXT
            MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR -> BibMode.SUPPRESS_AUTHOR
            else -> error("unsupported native bib mode ${referent.mode}")
        }

    fun footnoteId(footnote: CPointer<markdown_core_footnote>): String {
        require(markdown_core_footnote_id(footnote, firstString.ptr)) { "invalid footnote" }
        return firstString.copyString()
    }
}

private fun CValue<markdown_core_scope>.toScope(): Scope =
    useContents {
        Scope(Position(start.line, start.column), Position(end.line, end.column))
    }

private fun markdown_core_optional_string.copyOptionalString(): String? = if (has_value) value.copyString() else null

private fun markdown_core_string.copyString(): String {
    val byteCount = length.checkedSize("string length")
    if (byteCount == 0) return ""
    return requireNotNull(data) { "native string has bytes but no data" }.readBytes(byteCount).decodeToString()
}

private fun ULong.checkedSize(field: String): Int {
    require(this <= Int.MAX_VALUE.toULong()) { "native $field exceeds the Kotlin collection limit" }
    return toInt()
}
