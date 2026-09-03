@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import cnames.structs.markdown_core_error
import cnames.structs.markdown_core_node
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_ALLOCATION_FAILED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_INTERNAL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ERROR_INVALID_ARGUMENT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_BLOCK_QUOTE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CODE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_CODE_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DIRECTIVE_LABEL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_DOCUMENT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_EMPHASIS
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FORMULA
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_FORMULA_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HEADING
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HTML
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_HTML_BLOCK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_IMAGE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_IMAGE_REFERENCE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LINE_BREAK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LINK
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LINK_REFERENCE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LIST
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_LIST_ITEM
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_PARAGRAPH
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_KIND_REFERENCE_DEFINITION
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
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_PLACEMENT_EMBEDDED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_PLACEMENT_STANDALONE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENCE_COLLAPSED
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENCE_FULL
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_REFERENCE_SHORTCUT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT
import com.nouprax.markdown.core.internal.capi.markdown_core_document_free
import com.nouprax.markdown.core.internal.capi.markdown_core_document_parse
import com.nouprax.markdown.core.internal.capi.markdown_core_document_root
import com.nouprax.markdown.core.internal.capi.markdown_core_error_free
import com.nouprax.markdown.core.internal.capi.markdown_core_error_get_code
import com.nouprax.markdown.core.internal.capi.markdown_core_error_get_message
import com.nouprax.markdown.core.internal.capi.markdown_core_list_flavorVar
import com.nouprax.markdown.core.internal.capi.markdown_core_node_association
import com.nouprax.markdown.core.internal.capi.markdown_core_node_child_count
import com.nouprax.markdown.core.internal.capi.markdown_core_node_code_block_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_definition_resource
import com.nouprax.markdown.core.internal.capi.markdown_core_node_directive_attribute_at
import com.nouprax.markdown.core.internal.capi.markdown_core_node_directive_label
import com.nouprax.markdown.core.internal.capi.markdown_core_node_directive_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_formula_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_first_child
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_kind
import com.nouprax.markdown.core.internal.capi.markdown_core_node_get_next_sibling
import com.nouprax.markdown.core.internal.capi.markdown_core_node_heading_level
import com.nouprax.markdown.core.internal.capi.markdown_core_node_image_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_link_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_list_item_checked
import com.nouprax.markdown.core.internal.capi.markdown_core_node_list_properties
import com.nouprax.markdown.core.internal.capi.markdown_core_node_literal
import com.nouprax.markdown.core.internal.capi.markdown_core_node_reference_form
import com.nouprax.markdown.core.internal.capi.markdown_core_node_scope
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_alignment_at
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_column_count
import com.nouprax.markdown.core.internal.capi.markdown_core_node_table_row_is_header
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_bool
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_i64
import com.nouprax.markdown.core.internal.capi.markdown_core_optional_string
import com.nouprax.markdown.core.internal.capi.markdown_core_parse_options
import com.nouprax.markdown.core.internal.capi.markdown_core_parse_options_init
import com.nouprax.markdown.core.internal.capi.markdown_core_placement_modeVar
import com.nouprax.markdown.core.internal.capi.markdown_core_reference_formVar
import com.nouprax.markdown.core.internal.capi.markdown_core_string
import com.nouprax.markdown.core.internal.capi.markdown_core_table_alignmentVar
import kotlinx.cinterop.BooleanVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
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

internal actual fun parsePlatformDocument(
    source: ByteArray,
    options: ParseOptions,
): Document =
    memScoped {
        val nativeOptions = alloc<markdown_core_parse_options>()
        markdown_core_parse_options_init(nativeOptions.ptr)
        nativeOptions.smart_punctuation = options.smartPunctuation
        nativeOptions.footnotes = options.footnotes
        nativeOptions.strip_html_comments = options.stripHTMLComments
        nativeOptions.tables = options.tables
        nativeOptions.strikethrough = options.strikethrough
        nativeOptions.autolinks = options.autolinks
        nativeOptions.task_lists = options.taskLists
        nativeOptions.formulas = options.formulas
        nativeOptions.directives = options.directives

        val error = alloc<CPointerVar<markdown_core_error>>()
        error.value = null
        val document =
            if (source.isEmpty()) {
                markdown_core_document_parse(null, 0u, nativeOptions.ptr, error.ptr)
            } else {
                source.usePinned { pinned ->
                    markdown_core_document_parse(
                        pinned.addressOf(0).reinterpret(),
                        source.size.toULong(),
                        nativeOptions.ptr,
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
)

/** Copies the C tree iteratively while the immutable native document is alive. */
private class NativeTreeBuilder(
    root: CPointer<markdown_core_node>,
    private val scratch: NativeScratch,
) {
    private val records = mutableListOf(NativeNodeRecord(root))
    private lateinit var built: Array<Markup?>

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

    private fun materialize(index: Int): Markup {
        val record = records[index]
        val node = record.pointer
        val kind = markdown_core_node_get_kind(node)
        val scope = nativeScope(node)
        val children = children(record)
        return when (kind) {
            MARKDOWN_CORE_KIND_DOCUMENT -> {
                Document(children, scope)
            }

            MARKDOWN_CORE_KIND_BLOCK_QUOTE -> {
                BlockQuote(children, scope)
            }

            MARKDOWN_CORE_KIND_PARAGRAPH -> {
                Paragraph(children, scope)
            }

            MARKDOWN_CORE_KIND_HEADING -> {
                Heading(scratch.headingLevel(node), children, scope)
            }

            MARKDOWN_CORE_KIND_THEMATIC_BREAK -> {
                ThematicBreak(scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_LIST -> {
                scratch.list(node, children, scope)
            }

            MARKDOWN_CORE_KIND_LIST_ITEM -> {
                ListItem(scratch.listItemChecked(node), children, scope)
            }

            MARKDOWN_CORE_KIND_CODE_BLOCK -> {
                scratch.codeBlock(node, scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_HTML_BLOCK -> {
                HTMLBlock(scratch.literal(node), scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_FORMULA_BLOCK -> {
                val formula = scratch.formula(node)
                require(formula.first == PlacementMode.STANDALONE) { "formula block is not standalone" }
                FormulaBlock(formula.second, scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_TABLE -> {
                scratch.table(node, children, scope)
            }

            MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK -> {
                scratch.directiveBlock(node, label(record), children, scope)
            }

            MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION -> {
                val association = scratch.association(node)
                FootnoteDefinition(association.first, association.second, children, scope)
            }

            MARKDOWN_CORE_KIND_TEXT -> {
                Text(scratch.literal(node), scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_SOFT_BREAK -> {
                SoftBreak(scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_LINE_BREAK -> {
                LineBreak(scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_CODE -> {
                Code(scratch.literal(node), scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_HTML -> {
                HTML(scratch.literal(node), scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_FORMULA -> {
                val formula = scratch.formula(node)
                Formula(formula.first, formula.second, scope).also { requireLeaf(children, kind) }
            }

            MARKDOWN_CORE_KIND_EMPHASIS -> {
                Emphasis(children, scope)
            }

            MARKDOWN_CORE_KIND_STRONG -> {
                Strong(children, scope)
            }

            MARKDOWN_CORE_KIND_STRIKETHROUGH -> {
                Strikethrough(children, scope)
            }

            MARKDOWN_CORE_KIND_LINK -> {
                val resource = scratch.link(node)
                Link(resource.first, resource.second, children, scope)
            }

            MARKDOWN_CORE_KIND_IMAGE -> {
                val resource = scratch.image(node)
                Image(resource.first, resource.second, children, scope)
            }

            MARKDOWN_CORE_KIND_DIRECTIVE -> {
                scratch.directive(node, label(record), children, scope)
            }

            MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE -> {
                requireLeaf(children, kind)
                val association = scratch.association(node)
                FootnoteReference(association.first, association.second, scope)
            }

            MARKDOWN_CORE_KIND_TABLE_ROW -> {
                scratch.tableRow(node, children, scope)
            }

            MARKDOWN_CORE_KIND_TABLE_CELL -> {
                TableCell(children, scope)
            }

            MARKDOWN_CORE_KIND_DIRECTIVE_LABEL -> {
                DirectiveLabel(children, scope)
            }

            MARKDOWN_CORE_KIND_REFERENCE_DEFINITION -> {
                requireLeaf(children, kind)
                val association = scratch.association(node)
                val resource = scratch.definitionResource(node)
                ReferenceDefinition(association.first, association.second, resource.first, resource.second, scope)
            }

            MARKDOWN_CORE_KIND_LINK_REFERENCE -> {
                val association = scratch.association(node)
                LinkReference(association.first, association.second, scratch.referenceForm(node), children, scope)
            }

            MARKDOWN_CORE_KIND_IMAGE_REFERENCE -> {
                val association = scratch.association(node)
                ImageReference(association.first, association.second, scratch.referenceForm(node), children, scope)
            }

            else -> {
                error("unsupported native node kind $kind")
            }
        }
    }

    private fun children(record: NativeNodeRecord): kotlin.collections.List<Markup> =
        immutableList(record.childCount) { offset ->
            requireNotNull(built[record.childStart + offset]) { "native child was not materialized" }
        }

    private fun label(record: NativeNodeRecord): DirectiveLabel? {
        if (record.labelIndex < 0) return null
        val value = requireNotNull(built[record.labelIndex]) { "native directive label was not materialized" }
        require(value is DirectiveLabel) { "directive label field contains a non-label node" }
        return value
    }

    private fun nativeScope(node: CPointer<markdown_core_node>): Scope =
        markdown_core_node_scope(node).useContents {
            Scope(Position(start.line, start.column), Position(end.line, end.column))
        }

    private fun requireLeaf(
        children: kotlin.collections.List<Markup>,
        kind: UInt,
    ) {
        require(children.isEmpty()) { "native leaf kind $kind contains children" }
    }
}

private class NativeScratch(
    scope: MemScope,
) {
    private val firstString = scope.alloc<markdown_core_string>()
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
    private val placementMode = scope.alloc<markdown_core_placement_modeVar>()
    private val tableAlignment = scope.alloc<markdown_core_table_alignmentVar>()
    private val referenceForm = scope.alloc<markdown_core_reference_formVar>()

    fun headingLevel(node: CPointer<markdown_core_node>): Int {
        require(markdown_core_node_heading_level(node, integer.ptr)) { "invalid heading node" }
        return integer.value
    }

    fun list(
        node: CPointer<markdown_core_node>,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
    ): List {
        require(markdown_core_node_list_properties(node, listFlavor.ptr, optionalLong.ptr, firstBoolean.ptr)) {
            "invalid list node"
        }
        val flavor =
            when (listFlavor.value) {
                MARKDOWN_CORE_LIST_FLAVOR_BULLET -> ListFlavor.BULLET
                MARKDOWN_CORE_LIST_FLAVOR_ORDERED -> ListFlavor.ORDERED
                else -> error("unsupported native list flavor ${listFlavor.value}")
            }
        val items = children.immutableMap { requireNotNull(it as? ListItem) { "list contains a non-item node" } }
        return List(flavor, optionalLong.value.takeIf { optionalLong.has_value }, firstBoolean.value, items, scope)
    }

    fun listItemChecked(node: CPointer<markdown_core_node>): Boolean? {
        require(markdown_core_node_list_item_checked(node, optionalBoolean.ptr)) { "invalid list item node" }
        return optionalBoolean.value.takeIf { optionalBoolean.has_value }
    }

    fun codeBlock(
        node: CPointer<markdown_core_node>,
        scope: Scope,
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
        )
    }

    fun literal(node: CPointer<markdown_core_node>): String {
        require(markdown_core_node_literal(node, firstString.ptr)) { "invalid literal node" }
        return firstString.copyString()
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
    ): Table {
        require(markdown_core_node_table_column_count(node, count.ptr)) { "invalid table node" }
        val alignments =
            immutableList(count.value.checkedSize("table column count")) { index ->
                require(markdown_core_node_table_alignment_at(node, index.toULong(), tableAlignment.ptr)) {
                    "invalid table alignment"
                }
                when (tableAlignment.value) {
                    MARKDOWN_CORE_TABLE_ALIGNMENT_NONE -> TableAlignment.NONE
                    MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT -> TableAlignment.LEFT
                    MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER -> TableAlignment.CENTER
                    MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT -> TableAlignment.RIGHT
                    else -> error("unsupported native table alignment ${tableAlignment.value}")
                }
            }
        val rows = children.immutableMap { requireNotNull(it as? TableRow) { "table contains a non-row node" } }
        val headers = rows.filter(TableRow::isHeader)
        require(headers.size == 1) { "table must contain exactly one header row" }
        return Table(alignments, headers.single(), rows.filterNot(TableRow::isHeader).immutableMap { it }, scope)
    }

    fun tableRow(
        node: CPointer<markdown_core_node>,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
    ): TableRow {
        require(markdown_core_node_table_row_is_header(node, firstBoolean.ptr)) { "invalid table row node" }
        val cells = children.immutableMap { requireNotNull(it as? TableCell) { "table row contains a non-cell node" } }
        return TableRow(firstBoolean.value, cells, scope)
    }

    fun directiveBlock(
        node: CPointer<markdown_core_node>,
        label: DirectiveLabel?,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
    ): DirectiveBlock {
        val properties = directiveProperties(node)
        return DirectiveBlock(properties.first, properties.second, label, children, scope)
    }

    fun directive(
        node: CPointer<markdown_core_node>,
        label: DirectiveLabel?,
        children: kotlin.collections.List<Markup>,
        scope: Scope,
    ): Directive {
        require(children.isEmpty()) { "inline directive contains block content" }
        val properties = directiveProperties(node)
        return Directive(properties.first, properties.second, label, scope)
    }

    private fun directiveProperties(
        node: CPointer<markdown_core_node>,
    ): Pair<String, kotlin.collections.List<DirectiveAttribute>?> {
        require(markdown_core_node_directive_properties(node, firstString.ptr, firstBoolean.ptr, count.ptr)) {
            "invalid directive node"
        }
        val name = firstString.copyString()
        val attributeCount = count.value.checkedSize("directive attribute count")
        if (!firstBoolean.value) {
            require(attributeCount == 0) { "absent directive attributes have a nonzero count" }
            return name to null
        }
        val attributes =
            immutableList(attributeCount) { index ->
                require(
                    markdown_core_node_directive_attribute_at(
                        node,
                        index.toULong(),
                        firstString.ptr,
                        secondString.ptr,
                    ),
                ) { "invalid directive attribute" }
                DirectiveAttribute(firstString.copyString(), secondString.copyString())
            }
        return name to attributes
    }

    fun link(node: CPointer<markdown_core_node>): Pair<String, String?> {
        require(markdown_core_node_link_properties(node, firstString.ptr, firstOptionalString.ptr)) {
            "invalid link node"
        }
        return firstString.copyString() to firstOptionalString.copyOptionalString()
    }

    fun image(node: CPointer<markdown_core_node>): Pair<String, String?> {
        require(markdown_core_node_image_properties(node, firstString.ptr, firstOptionalString.ptr)) {
            "invalid image node"
        }
        return firstString.copyString() to firstOptionalString.copyOptionalString()
    }

    fun association(node: CPointer<markdown_core_node>): Pair<String, String> {
        require(markdown_core_node_association(node, firstString.ptr, secondString.ptr)) { "invalid association node" }
        return firstString.copyString() to secondString.copyString()
    }

    fun definitionResource(node: CPointer<markdown_core_node>): Pair<String, String?> {
        require(markdown_core_node_definition_resource(node, firstString.ptr, firstOptionalString.ptr)) {
            "invalid reference definition node"
        }
        return firstString.copyString() to firstOptionalString.copyOptionalString()
    }

    fun referenceForm(node: CPointer<markdown_core_node>): ReferenceForm {
        require(markdown_core_node_reference_form(node, referenceForm.ptr)) { "invalid reference node" }
        return when (referenceForm.value) {
            MARKDOWN_CORE_REFERENCE_FULL -> ReferenceForm.FULL
            MARKDOWN_CORE_REFERENCE_COLLAPSED -> ReferenceForm.COLLAPSED
            MARKDOWN_CORE_REFERENCE_SHORTCUT -> ReferenceForm.SHORTCUT
            else -> error("unsupported native reference form ${referenceForm.value}")
        }
    }
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
