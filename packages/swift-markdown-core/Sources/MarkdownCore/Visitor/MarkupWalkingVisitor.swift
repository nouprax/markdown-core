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
public protocol MarkupWalkingVisitor {
    mutating func visit(_ node: Document, phase: WalkPhase)
    mutating func visit(_ node: BlockQuote, phase: WalkPhase)
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
    mutating func visit(_ node: FootnoteDefinition, phase: WalkPhase)
    mutating func visit(_ node: ReferenceDefinition, phase: WalkPhase)
    mutating func visit(_ node: LinkReference, phase: WalkPhase)
    mutating func visit(_ node: ImageReference, phase: WalkPhase)
    mutating func visit(_ node: Text, phase: WalkPhase)
    mutating func visit(_ node: SoftBreak, phase: WalkPhase)
    mutating func visit(_ node: LineBreak, phase: WalkPhase)
    mutating func visit(_ node: Code, phase: WalkPhase)
    mutating func visit(_ node: HTML, phase: WalkPhase)
    mutating func visit(_ node: Formula, phase: WalkPhase)
    mutating func visit(_ node: Emphasis, phase: WalkPhase)
    mutating func visit(_ node: Strong, phase: WalkPhase)
    mutating func visit(_ node: Strikethrough, phase: WalkPhase)
    mutating func visit(_ node: Link, phase: WalkPhase)
    mutating func visit(_ node: Image, phase: WalkPhase)
    mutating func visit(_ node: Directive, phase: WalkPhase)
    mutating func visit(_ node: FootnoteReference, phase: WalkPhase)
    mutating func visit(_ node: TableRow, phase: WalkPhase)
    mutating func visit(_ node: TableCell, phase: WalkPhase)
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

private enum WalkAction {
    case enter(any Markup)
    case exit(any Markup)
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
            let node: any Markup
            switch action {
            case let .enter(value):
                phase = .entering
                node = value
            case let .exit(value):
                phase = .exiting
                node = value
            }
            node.accept(&self)
        }
    }

    private mutating func scheduleExit(_ node: any Markup) {
        if phase == .entering { actions.append(.exit(node)) }
    }

    mutating func visit(_ node: Document) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: BlockQuote) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
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
            for row in node.rows.reversed() { actions.append(.enter(row)) }
            actions.append(.enter(node.header))
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

    mutating func visit(_ node: FootnoteDefinition) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: ReferenceDefinition) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
    }

    mutating func visit(_ node: LinkReference) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
        if phase == .entering {
            for child in node.content.reversed() { actions.append(.enter(child)) }
        }
    }

    mutating func visit(_ node: ImageReference) {
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

    mutating func visit(_ node: FootnoteReference) {
        visitor.visit(node, phase: phase)
        scheduleExit(node)
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
