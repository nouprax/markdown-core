// The walker keeps an explicit stack of records. A record's children are
// already in canonical traversal order across its relations, so scheduling is
// the same for every kind; only the callback dispatch names each kind.
struct MarkupWalker<V: MarkupVisitor> {
    private var actions: [(record: MarkupRecord, phase: MarkupVisitPhase)] = []

    mutating func walk(_ root: MarkupRecord, with visitor: inout V) {
        actions.append((record: root, phase: .enter))
        while let action = actions.popLast() {
            if action.phase == .enter {
                actions.append((record: action.record, phase: .exit))
                for child in action.record.children.reversed() { actions.append((record: child, phase: .enter)) }
            }
            dispatch(action.record.markup, to: &visitor, phase: action.phase)
        }
    }
}

// The AST projection audit checks dispatch against the complete kind inventory.
// swiftlint:disable:next cyclomatic_complexity
func dispatch<V: MarkupVisitor>(_ node: any Markup, to visitor: inout V, phase: MarkupVisitPhase) {
    switch node {
    case let node as Document: visitor.visit(node, phase: phase)
    case let node as Callout: visitor.visit(node, phase: phase)
    case let node as Paragraph: visitor.visit(node, phase: phase)
    case let node as Heading: visitor.visit(node, phase: phase)
    case let node as List: visitor.visit(node, phase: phase)
    case let node as ListItem: visitor.visit(node, phase: phase)
    case let node as Table: visitor.visit(node, phase: phase)
    case let node as TableCaption: visitor.visit(node, phase: phase)
    case let node as TableRow: visitor.visit(node, phase: phase)
    case let node as TableCell: visitor.visit(node, phase: phase)
    case let node as DirectiveBlock: visitor.visit(node, phase: phase)
    case let node as DirectiveLabel: visitor.visit(node, phase: phase)
    case let node as Emphasis: visitor.visit(node, phase: phase)
    case let node as Strong: visitor.visit(node, phase: phase)
    case let node as Strikethrough: visitor.visit(node, phase: phase)
    case let node as Mark: visitor.visit(node, phase: phase)
    case let node as Insertion: visitor.visit(node, phase: phase)
    case let node as Span: visitor.visit(node, phase: phase)
    case let node as Superscript: visitor.visit(node, phase: phase)
    case let node as Subscript: visitor.visit(node, phase: phase)
    case let node as Link: visitor.visit(node, phase: phase)
    case let node as Embedded: visitor.visit(node, phase: phase)
    case let node as Directive: visitor.visit(node, phase: phase)
    case let node as Cite: visitor.visit(node, phase: phase)
    case let node as DefinitionList: visitor.visit(node, phase: phase)
    case let node as Definition: visitor.visit(node, phase: phase)
    case let node as Citation: visitor.visit(node, phase: phase)
    case let node as Footnote: visitor.visit(node, phase: phase)
    case let node as Specimen: visitor.visit(node, phase: phase)
    case let node as ThematicBreak: visitor.visit(node, phase: phase)
    case let node as CodeBlock: visitor.visit(node, phase: phase)
    case let node as HTMLBlock: visitor.visit(node, phase: phase)
    case let node as FormulaBlock: visitor.visit(node, phase: phase)
    case let node as Text: visitor.visit(node, phase: phase)
    case let node as SoftBreak: visitor.visit(node, phase: phase)
    case let node as LineBreak: visitor.visit(node, phase: phase)
    case let node as Code: visitor.visit(node, phase: phase)
    case let node as HTML: visitor.visit(node, phase: phase)
    case let node as CrossLink: visitor.visit(node, phase: phase)
    case let node as CrossEmbedded: visitor.visit(node, phase: phase)
    case let node as Comment: visitor.visit(node, phase: phase)
    case let node as Formula: visitor.visit(node, phase: phase)
    case let node as Metadata: visitor.visit(node, phase: phase)
    default: break
    }
}
