/// The phase supplied to a markup visit.
public enum MarkupVisitPhase: Sendable {
    /// During a walk, the node is reached before its descendants.
    case entering
    /// During a walk, all of the node's descendants have been visited.
    case exiting
}

/// A visitor over every ``Markup`` kind.
///
/// The protocol names ALL of them and none has a default implementation, so a
/// conformance that forgets a kind does not compile. That is the point: the
/// kind set is closed, and a kind added tomorrow breaks every visitor loudly
/// rather than being silently skipped.
public protocol MarkupVisitor {
    associatedtype Result
    mutating func visit(_ node: Document, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: List, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Table, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Code, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Comment, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Span, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Link, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) -> Result
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) -> Result
}

extension Markup {
    /// Dispatches one entering visit without traversing descendants.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result {
        accept(&visitor, phase: .entering)
    }

    /// Walks the node and its descendants depth first, reporting both phases through the same visitor.
    /// Traversal keeps call-stack depth independent of document depth.
    /// Only a visitor without a result can be used for automatic traversal.
    public func walk<V: MarkupVisitor>(with visitor: inout V) where V.Result == Void {
        var walker = MarkupWalker()
        walker.walk(self, with: &visitor)
    }
}

// The walker uses an explicit stack and schedules each kind's child fields in traversal order.
// Adding a kind requires specifying its traversal here.
private struct MarkupWalker: MarkupVisitor {
    private var actions: [(node: any Markup, phase: MarkupVisitPhase)] = []

    mutating func walk<V: MarkupVisitor>(_ node: any Markup, with visitor: inout V) where V.Result == Void {
        actions.append((node: node, phase: .entering))
        while let action = actions.popLast() {
            action.node.accept(&visitor, phase: action.phase)
            guard action.phase == .entering else { continue }
            actions.append((node: action.node, phase: .exiting))
            action.node.accept(&self)
        }
    }

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) {
        for child in node.specimens.reversed() { actions.append((node: child, phase: .entering)) }
        for child in node.footnotes.reversed() { actions.append((node: child, phase: .entering)) }
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
        if let child = node.metadata { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
        if let nodes = node.title {
            for child in nodes.reversed() { actions.append((node: child, phase: .entering)) }
        }
    }

    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: List, phase: MarkupVisitPhase) {
        for child in node.items.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Table, phase: MarkupVisitPhase) {
        for child in node.foot.reversed() { actions.append((node: child, phase: .entering)) }
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
        for child in node.head.reversed() { actions.append((node: child, phase: .entering)) }
        if let child = node.caption { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) {
        for child in node.cells.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
        if let child = node.label { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Span, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Link, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) {
        if let child = node.label { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) {
        for child in node.citations.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) {
        for child in node.definitions.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) {
        for body in node.content.reversed() {
            for child in body.reversed() { actions.append((node: child, phase: .entering)) }
        }
        for child in node.term.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) {
        for child in node.suffix.reversed() { actions.append((node: child, phase: .entering)) }
        for child in node.prefix.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) {
        for child in node.content.reversed() { actions.append((node: child, phase: .entering)) }
    }

    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: Text, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: Code, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: Comment, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) {}

    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) {}
}
