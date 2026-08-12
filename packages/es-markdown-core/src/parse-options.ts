/**
 * The feature switches for one parse, fixed for the document's whole series.
 *
 * Every switch is optional and an omitted one is ON, so the only reason to
 * name one is to turn it off. A switch that is present must be a boolean —
 * including one written as `undefined`, which is a `TypeError` and not a
 * fallback to the default.
 */
export interface ParseOptions {
    /** Replaces straight quotes, dashes, and ellipses with typographic
     * forms. */
    readonly smartPunctuation?: boolean;
    /** Parses footnote definitions and references. */
    readonly footnotes?: boolean;
    /** Parses pipe tables. */
    readonly tables?: boolean;
    /** Parses `~struck~` and `~~struck~~`; the closer must match the opener's
     * tilde count. */
    readonly strikethrough?: boolean;
    /** Recognizes bare URLs and email addresses as links.
     *
     * `<https://x>` is CommonMark's own autolink and is not gated here. */
    readonly autolinks?: boolean;
    /** Parses `[ ]` and `[x]` task-list item markers. */
    readonly taskLists?: boolean;
    /** Parses formula spans and blocks, including dollar and LaTeX delimiters
     * and `formula` fenced blocks. */
    readonly formulas?: boolean;
    /** Parses the directive grammar: `:name` inline, `::name` leaf and
     * `:::name` container at the head of a line, each with its `[label]` and
     * `{attributes}`. */
    readonly directives?: boolean;
    /** Parses cross-links written as `[[reference]]`. */
    readonly crossLinks?: boolean;
    /** Parses embeds written as `![[reference]]`. */
    readonly embeds?: boolean;
}
