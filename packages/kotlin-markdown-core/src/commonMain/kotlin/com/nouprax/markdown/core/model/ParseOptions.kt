package com.nouprax.markdown.core

public data class ParseOptions(
    public val smartPunctuation: Boolean = true,
    public val footnotes: Boolean = true,
    public val stripHTMLComments: Boolean = true,
    public val tables: Boolean = true,
    public val strikethrough: Boolean = true,
    public val autolinks: Boolean = true,
    public val taskLists: Boolean = true,
    public val formulas: Boolean = true,
    public val dollarFormulaDelimiters: Boolean = true,
    public val latexFormulaDelimiters: Boolean = true,
    public val directives: Boolean = true,
)

internal fun ParseOptions.toNativeMask(): Int =
    listOf(
        smartPunctuation,
        footnotes,
        stripHTMLComments,
        tables,
        strikethrough,
        autolinks,
        taskLists,
        formulas,
        dollarFormulaDelimiters,
        latexFormulaDelimiters,
        directives,
    ).foldIndexed(0) { index, mask, enabled -> if (enabled) mask or (1 shl index) else mask }
