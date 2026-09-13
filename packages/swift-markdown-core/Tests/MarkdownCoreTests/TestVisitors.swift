import MarkdownCore

struct KindVisitor: MarkupVisitor {
    var kinds: [String] = []

    private mutating func record(_ kind: String, phase: MarkupVisitPhase) {
        if phase == .enter { kinds.append(kind) }
    }

    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) { record("heading:\(node.level)", phase: phase) }
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Table, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }

    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Code, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: MarkdownCore.Comment, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: MarkdownCore.CrossLink, phase: MarkupVisitPhase) {
        record(kindName(node), phase: phase)
    }
    mutating func visit(_ node: MarkdownCore.CrossEmbedded, phase: MarkupVisitPhase) {
        record(kindName(node), phase: phase)
    }
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Span, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Link, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) { record(kindName(node), phase: phase) }
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) { record("row", phase: phase) }
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) { record("cell", phase: phase) }
}

func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}

struct RecordingWalkingVisitor: MarkupVisitor {
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) { record(node, phase) }
    private let recordEvents: Bool
    var events: [String] = []
    var tableRowKinds: [Int] = []
    var entered = 0
    var exited = 0

    init(recordEvents: Bool = true) {
        self.recordEvents = recordEvents
    }

    private mutating func record(_ node: any Markup, _ phase: MarkupVisitPhase) {
        record(kindName(node), phase)
    }

    private mutating func record(_ name: String, _ phase: MarkupVisitPhase) {
        switch phase {
        case .enter: entered += 1
        case .exit: exited += 1
        }
        if recordEvents { events.append("\(phase):\(name)") }
    }

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Table, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Code, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.Comment, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.CrossLink, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.CrossEmbedded, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Span, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Link, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) {
        record(node, phase)
        if phase == .enter { tableRowKinds.append(Int(node.scope.start.line)) }
    }
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) { record(node, phase) }
}
