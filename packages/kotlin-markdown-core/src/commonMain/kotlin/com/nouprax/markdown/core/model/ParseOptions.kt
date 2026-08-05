@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads
import kotlin.jvm.JvmSynthetic

public data class ParseOptions
    @JvmOverloads
    constructor(
        public val smartPunctuation: Boolean = true,
        public val footnotes: Boolean = true,
        public val tables: Boolean = true,
        public val strikethrough: Boolean = true,
        public val autolinks: Boolean = true,
        public val taskLists: Boolean = true,
        public val formulas: Boolean = true,
        public val directives: Boolean = true,
        public val crossLinks: Boolean = true,
        public val embeds: Boolean = true,
    )

@JvmSynthetic
internal fun ParseOptions.toNativeMask(): Int =
    listOf(
        smartPunctuation,
        footnotes,
        tables,
        strikethrough,
        autolinks,
        taskLists,
        formulas,
        directives,
        crossLinks,
        embeds,
    ).foldIndexed(0) { index, mask, enabled -> if (enabled) mask or (1 shl index) else mask }
