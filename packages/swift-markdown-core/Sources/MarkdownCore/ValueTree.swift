/// A read-only, random-access relation in an immutable Swift value tree.
///
/// Count, indexing, and collection views are constant time. Use Array(...) to
/// request an independent array. A retained element keeps the immutable Swift
/// storage alive; no native parse, cache, lock, or recursive ownership is kept.
public struct MarkupCollection<Element: Sendable>: RandomAccessCollection, Sendable {
    /// Zero-based position within this relation.
    public typealias Index = Int
    let tree: ValueTree
    let recordIndices: [Int]

    /// The first valid position, or `endIndex` for an empty relation.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last element.
    public var endIndex: Int { recordIndices.endIndex }
    /// The value at a valid position, without copying its descendants.
    public subscript(position: Int) -> Element {
        tree.value(at: recordIndices[position], as: Element.self)
    }
}

/// Records contain scalar values and integer edges only. Consequently ARC
/// destruction has bounded stack depth, including after extracting a subtree.
enum StoredValue: Sendable {
    // Only the root owns Metadata. Box that payload once so its inline
    // capacity is not paid by every record, including every Text leaf.
    indirect case markupDocument(Document.Fields)
    case markupCallout(Callout.Fields)
    case markupDefinitionList(DefinitionList.Fields)
    case markupDefinition(Definition.Fields)
    case markupParagraph(Paragraph.Fields)
    case markupHeading(Heading.Fields)
    case markupThematicBreak(ThematicBreak)
    case markupList(List.Fields)
    case markupListItem(ListItem.Fields)
    case markupCodeBlock(CodeBlock)
    case markupHTMLBlock(HTMLBlock)
    case markupFormulaBlock(FormulaBlock)
    case markupTable(Table.Fields)
    case markupDirectiveBlock(DirectiveBlock.Fields)
    case markupText(Text)
    case markupSoftBreak(SoftBreak)
    case markupLineBreak(LineBreak)
    case markupCode(Code)
    case markupHTML(HTML)
    case markupComment(Comment)
    case markupCrossLink(CrossLink)
    case markupCrossEmbedded(CrossEmbedded)
    case markupFormula(Formula)
    case markupEmphasis(Emphasis.Fields)
    case markupStrong(Strong.Fields)
    case markupStrikethrough(Strikethrough.Fields)
    case markupMark(Mark.Fields)
    case markupInsertion(Insertion.Fields)
    case markupSpan(Span.Fields)
    case markupSuperscript(Superscript.Fields)
    case markupSubscript(Subscript.Fields)
    case markupLink(Link.Fields)
    case markupMedia(Media.Fields)
    case markupDirective(Directive.Fields)
    case markupCite(Cite.Fields)
    case markupTableCaption(TableCaption.Fields)
    case markupTableRow(TableRow.Fields)
    case markupTableCell(TableCell.Fields)
    case markupDirectiveLabel(DirectiveLabel.Fields)
    case valueFootnote(Footnote.Fields)
    case valueSpecimen(Specimen.Fields)
    case valueCitation(Citation.Fields)
    case definitionBody([Int])
}

final class ValueTree: Sendable {
    let records: [StoredValue]

    init(records: [StoredValue]) {
        self.records = records
    }

    // Exhaustive dispatch preserves the one-to-one stored-kind projection.
    // swiftlint:disable:next cyclomatic_complexity
    func value<Value: Sendable>(at index: Int, as type: Value.Type) -> Value {
        let value: any Sendable
        switch records[index] {
        case .markupDocument: value = Document(tree: self, index: index)
        case .markupCallout: value = Callout(tree: self, index: index)
        case .markupDefinitionList: value = DefinitionList(tree: self, index: index)
        case .markupDefinition: value = Definition(tree: self, index: index)
        case .markupParagraph: value = Paragraph(tree: self, index: index)
        case .markupHeading: value = Heading(tree: self, index: index)
        case let .markupThematicBreak(node): value = node
        case .markupList: value = List(tree: self, index: index)
        case .markupListItem: value = ListItem(tree: self, index: index)
        case let .markupCodeBlock(node): value = node
        case let .markupHTMLBlock(node): value = node
        case let .markupFormulaBlock(node): value = node
        case .markupTable: value = Table(tree: self, index: index)
        case .markupDirectiveBlock: value = DirectiveBlock(tree: self, index: index)
        case let .markupText(node): value = node
        case let .markupSoftBreak(node): value = node
        case let .markupLineBreak(node): value = node
        case let .markupCode(node): value = node
        case let .markupHTML(node): value = node
        case let .markupComment(node): value = node
        case let .markupCrossLink(node): value = node
        case let .markupCrossEmbedded(node): value = node
        case let .markupFormula(node): value = node
        case .markupEmphasis: value = Emphasis(tree: self, index: index)
        case .markupStrong: value = Strong(tree: self, index: index)
        case .markupStrikethrough: value = Strikethrough(tree: self, index: index)
        case .markupMark: value = Mark(tree: self, index: index)
        case .markupInsertion: value = Insertion(tree: self, index: index)
        case .markupSpan: value = Span(tree: self, index: index)
        case .markupSuperscript: value = Superscript(tree: self, index: index)
        case .markupSubscript: value = Subscript(tree: self, index: index)
        case .markupLink: value = Link(tree: self, index: index)
        case .markupMedia: value = Media(tree: self, index: index)
        case .markupDirective: value = Directive(tree: self, index: index)
        case .markupCite: value = Cite(tree: self, index: index)
        case .markupTableCaption: value = TableCaption(tree: self, index: index)
        case .markupTableRow: value = TableRow(tree: self, index: index)
        case .markupTableCell: value = TableCell(tree: self, index: index)
        case .markupDirectiveLabel: value = DirectiveLabel(tree: self, index: index)
        case .valueFootnote: value = Footnote(tree: self, index: index)
        case .valueSpecimen: value = Specimen(tree: self, index: index)
        case .valueCitation: value = Citation(tree: self, index: index)
        case let .definitionBody(indices): value = MarkupCollection<any Markup>(tree: self, recordIndices: indices)
        }
        guard let result = value as? Value else {
            preconditionFailure("Invalid value kind in a typed relation")
        }
        return result
    }
}
