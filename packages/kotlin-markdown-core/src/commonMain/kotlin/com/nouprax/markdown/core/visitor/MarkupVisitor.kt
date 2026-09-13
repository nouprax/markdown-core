package com.nouprax.markdown.core

/** Required callbacks for each concrete markup kind, driven by [Markup.walk]. */
public interface MarkupVisitor {
    public fun visit(
        document: Document,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        callout: Callout,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        paragraph: Paragraph,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        heading: Heading,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        thematicBreak: ThematicBreak,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        list: List,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        listItem: ListItem,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        codeBlock: CodeBlock,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        htmlBlock: HTMLBlock,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        formulaBlock: FormulaBlock,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        table: Table,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        tableCaption: TableCaption,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        tableRow: TableRow,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        tableCell: TableCell,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        directiveBlock: DirectiveBlock,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        directiveLabel: DirectiveLabel,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        text: Text,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        softBreak: SoftBreak,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        lineBreak: LineBreak,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        code: Code,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        html: HTML,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        comment: Comment,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        crossLink: CrossLink,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        crossEmbedded: CrossEmbedded,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        formula: Formula,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        emphasis: Emphasis,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        strong: Strong,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        strikethrough: Strikethrough,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        mark: Mark,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        insertion: Insertion,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        span: Span,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        superscript: Superscript,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        subscript: Subscript,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        definitionList: DefinitionList,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        definition: Definition,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        link: Link,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        embedded: Embedded,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        directive: Directive,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        cite: Cite,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        citation: Citation,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        footnote: Footnote,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        specimen: Specimen,
        phase: MarkupVisitPhase,
    ): Unit

    public fun visit(
        metadata: Metadata,
        phase: MarkupVisitPhase,
    ): Unit
}

/** The phase supplied to a markup visit. */
public enum class MarkupVisitPhase {
    /** During a walk, the node is reached before its descendants. */
    ENTER,

    /** During a walk, all of the node's descendants has been visited. */
    EXIT,
}

/**
 * Walks markup depth first, reporting both phases through the same visitor.
 * The explicit stack keeps call-stack depth independent of document depth.
 */
public fun Markup.walk(visitor: MarkupVisitor) {
    MarkupWalker(visitor).walk(this)
}
