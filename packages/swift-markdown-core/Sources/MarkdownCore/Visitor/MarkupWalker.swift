// The walker runs over the store's records, not over values: a pending visit
// is an index and a phase on two parallel stacks, the record's tag chooses the
// kind with one switch, and the relations to visit next are read from the
// record itself. The visitor still receives the typed view of every node.
// Adding a kind requires specifying its traversal here.
struct MarkupWalker<V: MarkupVisitor> {
    private var indices: [Int] = []
    private var phases: [MarkupVisitPhase] = []

    mutating func walk(_ node: any Markup, with visitor: inout V) {
        // The root is the one value read as a value: it names its record, and
        // the walk goes on from there. Metadata is a value with no relations.
        if let view = node as? any StoredMarkupView {
            let location = view.storedLocation
            walk(store: location.store, root: location.index, with: &visitor)
        } else if let metadata = node as? Metadata {
            visitor.visit(metadata, phase: .enter)
            visitor.visit(metadata, phase: .exit)
        } else {
            preconditionFailure("Markup conformances are limited to the canonical node kinds")
        }
    }

    private mutating func walk(store: MarkupStore, root: Int, with visitor: inout V) {
        push(root, .enter)
        while let index = indices.popLast() {
            let phase = phases.removeLast()
            if phase == .enter { push(index, .exit) }
            dispatch(store, index, visitor: &visitor, phase: phase)
        }
    }

    @inline(__always)
    private mutating func push(_ index: Int, _ phase: MarkupVisitPhase) {
        indices.append(index)
        phases.append(phase)
    }

    private mutating func schedule<Element: Sendable>(_ references: MarkupReferences<Element>) {
        for index in references.indices.reversed() { push(index, .enter) }
    }

    private mutating func schedule<Element: Sendable>(_ references: MarkupReferences<Element>?) {
        if let references { schedule(references) }
    }

    private mutating func schedule<Element: Sendable>(_ reference: MarkupReference<Element>?) {
        if let reference { push(reference.index, .enter) }
    }

    private mutating func schedule<Element: Sendable>(_ groups: MarkupGroupReferences<Element>) {
        for group in groups.indices.reversed() {
            for index in group.reversed() { push(index, .enter) }
        }
    }

    // The AST projection audit checks dispatch against the complete kind inventory.
    // swiftlint:disable:next cyclomatic_complexity function_body_length
    private mutating func dispatch(_ store: MarkupStore, _ index: Int, visitor: inout V, phase: MarkupVisitPhase) {
        switch store.records[index] {
        case let .document(fields):
            visitor.visit(Document(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.specimens)
            schedule(fields.footnotes)
            schedule(fields.content)
            schedule(fields.metadata)
        case let .callout(fields):
            visitor.visit(Callout(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
            schedule(fields.title)
        case let .paragraph(fields):
            visitor.visit(Paragraph(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .heading(fields):
            visitor.visit(Heading(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .list(fields):
            visitor.visit(List(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.items)
        case let .listItem(fields):
            visitor.visit(ListItem(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .table(fields):
            visitor.visit(Table(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.foot)
            schedule(fields.content)
            schedule(fields.head)
            schedule(fields.caption)
        case let .tableCaption(fields):
            visitor.visit(TableCaption(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .tableRow(fields):
            visitor.visit(TableRow(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.cells)
        case let .tableCell(fields):
            visitor.visit(TableCell(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .directiveBlock(fields):
            visitor.visit(DirectiveBlock(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
            schedule(fields.label)
        case let .directiveLabel(fields):
            visitor.visit(DirectiveLabel(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .emphasis(fields):
            visitor.visit(Emphasis(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .strong(fields):
            visitor.visit(Strong(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .strikethrough(fields):
            visitor.visit(Strikethrough(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .mark(fields):
            visitor.visit(Mark(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .insertion(fields):
            visitor.visit(Insertion(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .span(fields):
            visitor.visit(Span(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .superscript(fields):
            visitor.visit(Superscript(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .`subscript`(fields):
            visitor.visit(Subscript(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .link(fields):
            visitor.visit(Link(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .embedded(fields):
            visitor.visit(Embedded(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .directive(fields):
            visitor.visit(Directive(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.label)
        case let .cite(fields):
            visitor.visit(Cite(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.citations)
        case let .definitionList(fields):
            visitor.visit(DefinitionList(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.definitions)
        case let .definition(fields):
            visitor.visit(Definition(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
            schedule(fields.term)
        case let .citation(fields):
            visitor.visit(Citation(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.suffix)
            schedule(fields.prefix)
        case let .footnote(fields):
            visitor.visit(Footnote(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case let .specimen(fields):
            visitor.visit(Specimen(fields: Stored(store: store, index: index)), phase: phase)
            guard phase == .enter else { return }
            schedule(fields.content)
        case .thematicBreak:
            visitor.visit(ThematicBreak(fields: Stored(store: store, index: index)), phase: phase)
        case .codeBlock:
            visitor.visit(CodeBlock(fields: Stored(store: store, index: index)), phase: phase)
        case .htmlBlock:
            visitor.visit(HTMLBlock(fields: Stored(store: store, index: index)), phase: phase)
        case .formulaBlock:
            visitor.visit(FormulaBlock(fields: Stored(store: store, index: index)), phase: phase)
        case .text:
            visitor.visit(Text(fields: Stored(store: store, index: index)), phase: phase)
        case .softBreak:
            visitor.visit(SoftBreak(fields: Stored(store: store, index: index)), phase: phase)
        case .lineBreak:
            visitor.visit(LineBreak(fields: Stored(store: store, index: index)), phase: phase)
        case .code:
            visitor.visit(Code(fields: Stored(store: store, index: index)), phase: phase)
        case .html:
            visitor.visit(HTML(fields: Stored(store: store, index: index)), phase: phase)
        case .crossLink:
            visitor.visit(CrossLink(fields: Stored(store: store, index: index)), phase: phase)
        case .crossEmbedded:
            visitor.visit(CrossEmbedded(fields: Stored(store: store, index: index)), phase: phase)
        case .comment:
            visitor.visit(Comment(fields: Stored(store: store, index: index)), phase: phase)
        case .formula:
            visitor.visit(Formula(fields: Stored(store: store, index: index)), phase: phase)
        case let .metadata(node):
            visitor.visit(node, phase: phase)
        }
    }
}
