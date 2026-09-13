/// The phase supplied to a markup visit.
public enum MarkupVisitPhase: Sendable {
    /// During a walk, the node is reached before its descendants.
    case enter
    /// During a walk, all of the node's descendants have been visited.
    case exit
}

/// A visitor over every ``Markup`` kind.
///
/// The protocol names ALL of them and none has a default implementation, so a
/// conformance that forgets a kind does not compile. That is the point: the
/// kind set is closed, and a kind added tomorrow breaks every visitor loudly
/// rather than being silently skipped.
public protocol MarkupVisitor {
    mutating func visit(_ node: Document, phase: MarkupVisitPhase)
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase)
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase)
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase)
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase)
    mutating func visit(_ node: List, phase: MarkupVisitPhase)
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase)
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase)
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase)
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase)
    mutating func visit(_ node: Table, phase: MarkupVisitPhase)
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase)
    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase)
    mutating func visit(_ node: Text, phase: MarkupVisitPhase)
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase)
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase)
    mutating func visit(_ node: Code, phase: MarkupVisitPhase)
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase)
    mutating func visit(_ node: Comment, phase: MarkupVisitPhase)
    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase)
    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase)
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase)
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase)
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase)
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase)
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase)
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase)
    mutating func visit(_ node: Span, phase: MarkupVisitPhase)
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase)
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase)
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase)
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase)
    mutating func visit(_ node: Link, phase: MarkupVisitPhase)
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase)
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase)
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase)
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase)
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase)
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase)
    mutating func visit(_ node: Citation, phase: MarkupVisitPhase)
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase)
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase)
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase)
}

extension Markup {
    /// Walks the node and its descendants depth first, reporting both phases through the same visitor.
    /// Traversal keeps call-stack depth independent of document depth.
    public func walk<V: MarkupVisitor>(with visitor: inout V) {
        var walker = MarkupWalker<V>()
        walker.walk(self, with: &visitor)
    }
}
