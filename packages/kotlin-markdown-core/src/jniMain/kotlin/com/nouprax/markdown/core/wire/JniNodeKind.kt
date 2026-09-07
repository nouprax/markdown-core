package com.nouprax.markdown.core

internal enum class JniNodeKind(
    val rawValue: Int,
) {
    DOCUMENT(1),
    CALLOUT(2),
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
    TEXT(13),
    SOFT_BREAK(14),
    LINE_BREAK(15),
    CODE(16),
    HTML(17),
    FORMULA(18),
    EMPHASIS(19),
    STRONG(20),
    STRIKETHROUGH(21),
    LINK(22),
    IMAGE(23),
    DIRECTIVE(24),
    CITE(25),
    TABLE_ROW(26),
    TABLE_CELL(27),
    DIRECTIVE_LABEL(28),
    COMMENT(29),
    ;

    companion object {
        private val byRawValue = entries.associateBy(JniNodeKind::rawValue)

        fun from(rawValue: Int): JniNodeKind = byRawValue[rawValue] ?: error("unsupported native node kind $rawValue")
    }
}
