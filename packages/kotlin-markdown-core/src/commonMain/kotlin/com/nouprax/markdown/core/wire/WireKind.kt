package com.nouprax.markdown.core

internal enum class WireKind(
    val rawValue: Int,
) {
    DOCUMENT(1),
    BLOCK_QUOTE(2),
    PARAGRAPH(3),
    HEADING(4),
    THEMATIC_BREAK(5),
    LIST(6),
    LIST_ITEM(7),
    CODE_BLOCK(8),
    HTML_BLOCK(9),
    FORMULA_BLOCK(10),
    TABLE(11),
    DIRECTIVE_BLOCK(12),
    FOOTNOTE_DEFINITION(13),
    TEXT(14),
    SOFT_BREAK(15),
    LINE_BREAK(16),
    CODE(17),
    HTML(18),
    FORMULA(19),
    EMPHASIS(20),
    STRONG(21),
    STRIKETHROUGH(22),
    LINK(23),
    IMAGE(24),
    DIRECTIVE(25),
    FOOTNOTE_REFERENCE(26),
    TABLE_ROW(27),
    TABLE_CELL(28),
    ;

    companion object {
        private val byRawValue = entries.associateBy(WireKind::rawValue)

        fun from(rawValue: Int): WireKind = byRawValue[rawValue] ?: error("unsupported native node kind $rawValue")
    }
}
