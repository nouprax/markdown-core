/// The phase supplied to a markup visit.
public enum MarkupWalkPhase: Sendable {
    /// During a walk, the node is reached before its owned markup.
    case entering
    /// During a walk, all of the node's owned markup has been visited.
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
    mutating func visit(_ node: Document, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Callout, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Paragraph, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Heading, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: ThematicBreak, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: List, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: ListItem, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: CodeBlock, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: HTMLBlock, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: FormulaBlock, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Table, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: DirectiveBlock, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: DirectiveLabel, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Text, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: SoftBreak, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: LineBreak, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Code, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: HTML, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Comment, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: CrossLink, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: CrossEmbedded, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Formula, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Emphasis, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Strong, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Strikethrough, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Mark, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Insertion, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Span, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Superscript, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Subscript, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: DefinitionList, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Definition, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Link, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Embedded, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Directive, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Cite, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: TableCaption, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: TableRow, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: TableCell, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Citation, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Footnote, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Specimen, phase: MarkupWalkPhase) -> Result
    mutating func visit(_ node: Metadata, phase: MarkupWalkPhase) -> Result
}

extension Markup {
    /// Dispatches one entering visit without traversing owned markup.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result {
        accept(&visitor, phase: .entering)
    }

    /// Walks every owned node depth first, reporting both phases through the same visitor.
    /// An explicit stack keeps call-stack depth independent of document depth.
    /// Only a visitor without a result can be used for automatic traversal.
    public func walk<V: MarkupVisitor>(with visitor: inout V) where V.Result == Void {
        var schedule = OwnedMarkupSchedule(actions: [WalkAction(node: self, phase: .entering)])
        while let action = schedule.actions.popLast() {
            action.node.accept(&visitor, phase: action.phase)
            guard action.phase == .entering else { continue }
            schedule.actions.append(WalkAction(node: action.node, phase: .exiting))
            action.node.accept(&schedule)
        }
    }
}

private struct WalkAction {
    let node: any Markup
    let phase: MarkupWalkPhase
}

// Scheduling is itself a typed visit, so adding a kind must update its ownership rules.
// It records only pending events and never forwards client callbacks.
private struct OwnedMarkupSchedule: MarkupVisitor {
    var actions: [WalkAction]

    mutating func visit(_ node: Document, phase: MarkupWalkPhase) {
        for child in node.specimens.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        for child in node.footnotes.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        if let child = node.metadata { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Callout, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        if let nodes = node.title {
            for child in nodes.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        }
    }

    mutating func visit(_ node: Paragraph, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Heading, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: List, phase: MarkupWalkPhase) {
        for child in node.items.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: ListItem, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Table, phase: MarkupWalkPhase) {
        for child in node.foot.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        for child in node.head.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        if let child = node.caption { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableCaption, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableRow, phase: MarkupWalkPhase) {
        for child in node.cells.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: TableCell, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DirectiveBlock, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        if let child = node.label { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DirectiveLabel, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Emphasis, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Strong, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Strikethrough, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Mark, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Insertion, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Span, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Superscript, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Subscript, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Link, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Embedded, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Directive, phase: MarkupWalkPhase) {
        if let child = node.label { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Cite, phase: MarkupWalkPhase) {
        for child in node.citations.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: DefinitionList, phase: MarkupWalkPhase) {
        for child in node.definitions.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Definition, phase: MarkupWalkPhase) {
        for body in node.content.reversed() {
            for child in body.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        }
        for child in node.term.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Citation, phase: MarkupWalkPhase) {
        for child in node.suffix.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
        for child in node.prefix.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Footnote, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: Specimen, phase: MarkupWalkPhase) {
        for child in node.content.reversed() { actions.append(WalkAction(node: child, phase: .entering)) }
    }

    mutating func visit(_ node: ThematicBreak, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: CodeBlock, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: HTMLBlock, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: FormulaBlock, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: Text, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: SoftBreak, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: LineBreak, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: Code, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: HTML, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: CrossLink, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: CrossEmbedded, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: Comment, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: Formula, phase: MarkupWalkPhase) {}

    mutating func visit(_ node: Metadata, phase: MarkupWalkPhase) {}
}
