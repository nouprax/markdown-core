/// The phase of a depth-first markup walk.
public enum WalkPhase: Sendable {
    /// The node has been reached, before any of its owned markup is visited.
    case entering
    /// Every owned markup relation of the node has been visited.
    case exiting
}

/// An exhaustive, node-kind-dispatched observer for a depth-first markup walk.
///
/// This protocol deliberately has no untyped callback and no default
/// implementation. Adding a ``Markup`` kind therefore breaks every walking
/// visitor until it handles the new kind explicitly.
///
/// The traversal does not model every markup-valued relation as `children`.
/// Each node-kind branch schedules its own typed relations: for example, a
/// directive label is visited as the directive's `label` field and remains
/// distinct from the directive's `content`.
///
/// The scoped values outside the markup union have cases of their own: a
/// ``Citation`` is reported between its cite's phases, before its prefix and
/// suffix content, and a ``Footnote`` after the document's content, before
/// the footnote's own content.
public protocol MarkupWalkingVisitor {
    mutating func visit(_ node: Document, phase: WalkPhase)
    mutating func visit(_ node: Callout, phase: WalkPhase)
    mutating func visit(_ node: Paragraph, phase: WalkPhase)
    mutating func visit(_ node: Heading, phase: WalkPhase)
    mutating func visit(_ node: ThematicBreak, phase: WalkPhase)
    mutating func visit(_ node: List, phase: WalkPhase)
    mutating func visit(_ node: ListItem, phase: WalkPhase)
    mutating func visit(_ node: CodeBlock, phase: WalkPhase)
    mutating func visit(_ node: HTMLBlock, phase: WalkPhase)
    mutating func visit(_ node: FormulaBlock, phase: WalkPhase)
    mutating func visit(_ node: Table, phase: WalkPhase)
    mutating func visit(_ node: DirectiveBlock, phase: WalkPhase)
    mutating func visit(_ node: DirectiveLabel, phase: WalkPhase)
    mutating func visit(_ node: Text, phase: WalkPhase)
    mutating func visit(_ node: SoftBreak, phase: WalkPhase)
    mutating func visit(_ node: LineBreak, phase: WalkPhase)
    mutating func visit(_ node: Code, phase: WalkPhase)
    mutating func visit(_ node: HTML, phase: WalkPhase)
    mutating func visit(_ node: Comment, phase: WalkPhase)
    mutating func visit(_ node: Formula, phase: WalkPhase)
    mutating func visit(_ node: Emphasis, phase: WalkPhase)
    mutating func visit(_ node: Strong, phase: WalkPhase)
    mutating func visit(_ node: Strikethrough, phase: WalkPhase)
    mutating func visit(_ node: Link, phase: WalkPhase)
    mutating func visit(_ node: Image, phase: WalkPhase)
    mutating func visit(_ node: Directive, phase: WalkPhase)
    mutating func visit(_ node: Cite, phase: WalkPhase)
    mutating func visit(_ node: TableRow, phase: WalkPhase)
    mutating func visit(_ node: TableCell, phase: WalkPhase)
    mutating func visit(_ value: Citation, phase: WalkPhase)
    mutating func visit(_ value: Footnote, phase: WalkPhase)
    mutating func visit(_ value: Specimen, phase: WalkPhase)
}

extension Markup {
    /// Walks this markup and all of its owned markup depth first.
    ///
    /// The implementation uses an explicit action stack, so native call-stack
    /// depth does not grow with document depth. Every node receives
    /// ``WalkPhase/entering`` before its typed relations and
    /// ``WalkPhase/exiting`` after them.
    public func walk<Visitor: MarkupWalkingVisitor>(with visitor: inout Visitor) {
        var driver = WalkingDriver(visitor: visitor)
        driver.walk(self)
        visitor = driver.visitor
    }
}

/// One pending step: a markup node or one of the scoped values, with the
/// phase to report.
private enum WalkAction {
    case enter(any Markup)
    case exit(any Markup)
    case enterCitation(Citation)
    case exitCitation(Citation)
    case enterFootnote(Footnote)
    case exitFootnote(Footnote)
    case enterSpecimen(Specimen)
    case exitSpecimen(Specimen)
}

/// Node-kind callbacks own the relation schedule. The action stack is only a
/// traversal mechanism; it is not a public iterator or child projection.
private struct WalkingDriver<WalkingVisitor: MarkupWalkingVisitor>: MarkupVisitor {
    typealias Result = Void

    var visitor: WalkingVisitor
    private var actions: [WalkAction] = []
    private var phase = WalkPhase.entering

    init(visitor: WalkingVisitor) {
        self.visitor = visitor
    }

    mutating func walk(_ root: any Markup) {
        actions.append(.enter(root))
        while let action = actions.popLast() {
            switch action {
            case let .enter(node):
                phase = .entering
                node.accept(&self)
            case let .exit(node):
                phase = .exiting
                node.accept(&self)
            case let .enterCitation(value):
                phase = .entering
                visitCitation(value)
            case let .exitCitation(value):
                phase = .exiting
                visitCitation(value)
            case let .enterFootnote(value):
                phase = .entering
                visitFootnote(value)
            case let .exitFootnote(value):
                phase = .exiting
                visitFootnote(value)
            case let .enterSpecimen(value):
                phase = .entering
                visitSpecimen(value)
            case let .exitSpecimen(value):
                phase = .exiting
                visitSpecimen(value)
            }
        }
    }

    private mutating func scheduleExit(_ node: any Markup) {
        if phase == .entering { actions.append(.exit(node)) }
    }

    /// A citation's prefix is visited before its suffix, between its phases.
    private mutating func visitCitation(_ value: Citation) {
        visitor.visit(value, phase: phase)
        if phase == .entering {
            actions.append(.exitCitation(value))
            for child in value.suffix.reversed() { actions.append(.enter(child)) }
            for child in value.prefix.reversed() { actions.append(.enter(child)) }
        }
    }

    private mutating func visitFootnote(_ value: Footnote) {
        visitor.visit(value, phase: phase)
        if phase == .entering {
            actions.append(.exitFootnote(value))
            for child in value.content.reversed() { actions.append(.enter(child)) }
        }
    }

    private mutating func visitSpecimen(_ value: Specimen) {
        visitor.visit(value, phase: phase)
        if phase == .entering {
            actions.append(.exitSpecimen(value))
            for child in value.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Document) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            // The footnotes are visited after the content, in their order.
            for specimen in node.specimens.reversed() { actions.append(.enterSpecimen(specimen)) }
            for footnote in node.footnotes.reversed() { actions.append(.enterFootnote(footnote)) }
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Callout) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
            // The title is a node-valued field, visited before the content.
            if let title = node.title {
                for child in title.reversed() { actions.append(.enter(child)) }
            }
        }
    }

    mutating func visit(_ node: Paragraph) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Heading) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: ThematicBreak) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: MarkdownCore.List) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for item in node.items.reversed() { actions.append(.enter(item)) }
        }
    }

    mutating func visit(_ node: ListItem) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: CodeBlock) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: HTMLBlock) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: FormulaBlock) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: Table) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for row in node.foot.reversed() { actions.append(.enter(row)) }
            for row in node.content.reversed() { actions.append(.enter(row)) }
            for row in node.head.reversed() { actions.append(.enter(row)) }
        }
    }

    mutating func visit(_ node: DirectiveBlock) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
            if let label = node.label { actions.append(.enter(label)) }
        }
    }

    mutating func visit(_ node: DirectiveLabel) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Text) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: SoftBreak) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: LineBreak) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: Code) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: HTML) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: Comment) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: Formula) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: Emphasis) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Strong) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Strikethrough) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Link) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Image) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: Directive) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering, let label = node.label { actions.append(.enter(label)) }
    }

    mutating func visit(_ node: Cite) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for citation in node.citations.reversed() { actions.append(.enterCitation(citation)) }
        }
    }

    mutating func visit(_ node: TableRow) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for cell in node.cells.reversed() { actions.append(.enter(cell)) }
        }
    }

    mutating func visit(_ node: TableCell) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }
}
