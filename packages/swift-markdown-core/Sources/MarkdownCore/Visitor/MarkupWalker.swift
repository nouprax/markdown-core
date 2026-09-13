// The walker uses an explicit stack and schedules each kind's child fields in traversal order.
// Adding a kind requires specifying its traversal here.
struct MarkupWalker<V: MarkupVisitor> {
    private var actions: [(node: any Markup, phase: MarkupVisitPhase)] = []

    mutating func walk(_ node: any Markup, with visitor: inout V) {
        actions.append((node: node, phase: .enter))
        while let action = actions.popLast() {
            if action.phase == .enter { actions.append((node: action.node, phase: .exit)) }
            dispatch(action.node, visitor: &visitor, phase: action.phase)
        }
    }

    // The AST projection audit checks dispatch against the complete kind inventory.
    // swiftlint:disable:next cyclomatic_complexity
    private mutating func dispatch(_ node: any Markup, visitor: inout V, phase: MarkupVisitPhase) {
        switch node {
        case let node as Document: dispatch(document: node, visitor: &visitor, phase: phase)
        case let node as Callout: dispatch(callout: node, visitor: &visitor, phase: phase)
        case let node as Paragraph: dispatch(paragraph: node, visitor: &visitor, phase: phase)
        case let node as Heading: dispatch(heading: node, visitor: &visitor, phase: phase)
        case let node as List: dispatch(list: node, visitor: &visitor, phase: phase)
        case let node as ListItem: dispatch(listItem: node, visitor: &visitor, phase: phase)
        case let node as Table: dispatch(table: node, visitor: &visitor, phase: phase)
        case let node as TableCaption: dispatch(tableCaption: node, visitor: &visitor, phase: phase)
        case let node as TableRow: dispatch(tableRow: node, visitor: &visitor, phase: phase)
        case let node as TableCell: dispatch(tableCell: node, visitor: &visitor, phase: phase)
        case let node as DirectiveBlock: dispatch(directiveBlock: node, visitor: &visitor, phase: phase)
        case let node as DirectiveLabel: dispatch(directiveLabel: node, visitor: &visitor, phase: phase)
        case let node as Emphasis: dispatch(emphasis: node, visitor: &visitor, phase: phase)
        case let node as Strong: dispatch(strong: node, visitor: &visitor, phase: phase)
        case let node as Strikethrough: dispatch(strikethrough: node, visitor: &visitor, phase: phase)
        case let node as Mark: dispatch(mark: node, visitor: &visitor, phase: phase)
        case let node as Insertion: dispatch(insertion: node, visitor: &visitor, phase: phase)
        case let node as Span: dispatch(span: node, visitor: &visitor, phase: phase)
        case let node as Superscript: dispatch(superscript: node, visitor: &visitor, phase: phase)
        case let node as Subscript: dispatch(subscript: node, visitor: &visitor, phase: phase)
        case let node as Link: dispatch(link: node, visitor: &visitor, phase: phase)
        case let node as Embedded: dispatch(embedded: node, visitor: &visitor, phase: phase)
        case let node as Directive: dispatch(directive: node, visitor: &visitor, phase: phase)
        case let node as Cite: dispatch(cite: node, visitor: &visitor, phase: phase)
        case let node as DefinitionList: dispatch(definitionList: node, visitor: &visitor, phase: phase)
        case let node as Definition: dispatch(definition: node, visitor: &visitor, phase: phase)
        case let node as Citation: dispatch(citation: node, visitor: &visitor, phase: phase)
        case let node as Footnote: dispatch(footnote: node, visitor: &visitor, phase: phase)
        case let node as Specimen: dispatch(specimen: node, visitor: &visitor, phase: phase)
        case let node as ThematicBreak: dispatch(thematicBreak: node, visitor: &visitor, phase: phase)
        case let node as CodeBlock: dispatch(codeBlock: node, visitor: &visitor, phase: phase)
        case let node as HTMLBlock: dispatch(htmlBlock: node, visitor: &visitor, phase: phase)
        case let node as FormulaBlock: dispatch(formulaBlock: node, visitor: &visitor, phase: phase)
        case let node as Text: dispatch(text: node, visitor: &visitor, phase: phase)
        case let node as SoftBreak: dispatch(softBreak: node, visitor: &visitor, phase: phase)
        case let node as LineBreak: dispatch(lineBreak: node, visitor: &visitor, phase: phase)
        case let node as Code: dispatch(code: node, visitor: &visitor, phase: phase)
        case let node as HTML: dispatch(html: node, visitor: &visitor, phase: phase)
        case let node as CrossLink: dispatch(crossLink: node, visitor: &visitor, phase: phase)
        case let node as CrossEmbedded: dispatch(crossEmbedded: node, visitor: &visitor, phase: phase)
        case let node as Comment: dispatch(comment: node, visitor: &visitor, phase: phase)
        case let node as Formula: dispatch(formula: node, visitor: &visitor, phase: phase)
        case let node as Metadata: dispatch(metadata: node, visitor: &visitor, phase: phase)
        default:
            preconditionFailure("Markup conformances are limited to the canonical node kinds")
        }
    }
}

extension MarkupWalker {
    private mutating func dispatch(document node: Document, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.specimens.reversed() { actions.append((node: child, phase: .enter)) }
        for child in node.footnotes.reversed() { actions.append((node: child, phase: .enter)) }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
        if let child = node.metadata { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(callout node: Callout, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
        if let nodes = node.title {
            for child in nodes.reversed() { actions.append((node: child, phase: .enter)) }
        }
    }

    private mutating func dispatch(paragraph node: Paragraph, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(heading node: Heading, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(list node: List, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.items.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(listItem node: ListItem, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(table node: Table, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.foot.reversed() { actions.append((node: child, phase: .enter)) }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
        for child in node.head.reversed() { actions.append((node: child, phase: .enter)) }
        if let child = node.caption { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(tableCaption node: TableCaption, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(tableRow node: TableRow, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.cells.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(tableCell node: TableCell, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(directiveBlock node: DirectiveBlock, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
        if let child = node.label { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(directiveLabel node: DirectiveLabel, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }
}

extension MarkupWalker {
    private mutating func dispatch(emphasis node: Emphasis, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(strong node: Strong, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(strikethrough node: Strikethrough, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(mark node: Mark, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(insertion node: Insertion, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(span node: Span, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(superscript node: Superscript, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(`subscript` node: Subscript, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(link node: Link, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(embedded node: Embedded, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(directive node: Directive, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        if let child = node.label { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(cite node: Cite, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.citations.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(definitionList node: DefinitionList, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.definitions.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(definition node: Definition, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for body in node.content.reversed() {
            for child in body.reversed() { actions.append((node: child, phase: .enter)) }
        }
        for child in node.term.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(citation node: Citation, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.suffix.reversed() { actions.append((node: child, phase: .enter)) }
        for child in node.prefix.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(footnote node: Footnote, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(specimen node: Specimen, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
        guard phase == .enter else { return }
        for child in node.content.reversed() { actions.append((node: child, phase: .enter)) }
    }

    private mutating func dispatch(thematicBreak node: ThematicBreak, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(codeBlock node: CodeBlock, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(htmlBlock node: HTMLBlock, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(formulaBlock node: FormulaBlock, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(text node: Text, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(softBreak node: SoftBreak, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(lineBreak node: LineBreak, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(code node: Code, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(html node: HTML, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(crossLink node: CrossLink, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(crossEmbedded node: CrossEmbedded, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(comment node: Comment, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(formula node: Formula, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }

    private mutating func dispatch(metadata node: Metadata, visitor: inout V, phase: MarkupVisitPhase) {
        visitor.visit(node, phase: phase)
    }
}
