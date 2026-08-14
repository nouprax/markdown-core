@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads
import kotlin.jvm.JvmSynthetic

/**
 * What the parser recognizes.
 *
 * Fixed for a document's whole series: an append carries them forward
 * unchanged, and parsing under different options is a new [Document] rather
 * than a mutation.
 */
public data class ParseOptions
    @JvmOverloads
    constructor(
        /** Replaces straight quotes, dashes, and ellipses with typographic
         * forms. */
        public val smartPunctuation: Boolean = true,
        /** Parses footnote definitions and the references to them. */
        public val footnotes: Boolean = true,
        /** Parses pipe tables. */
        public val tables: Boolean = true,
        /** Parses `~struck~` and `~~struck~~`; the closer must match the
         * opener's tilde count. */
        public val strikethrough: Boolean = true,
        /** Recognizes bare URLs and email addresses as links. */
        public val autolinks: Boolean = true,
        /** Parses `[ ]` and `[x]` task-list item markers. */
        public val taskLists: Boolean = true,
        /** Parses formula spans and blocks: dollar and LaTeX delimiters, and
         * `formula` fenced blocks. */
        public val formulas: Boolean = true,
        /** Parses directives — inline `:name`, and the `::name` and `:::name`
         * block forms. */
        public val directives: Boolean = true,
        /** Parses cross-links written as `[[reference]]`. */
        public val crossLinks: Boolean = true,
        /** Parses embeds written as `![[reference]]`. */
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
