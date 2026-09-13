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

/// A read-only, random-access collection of grouped markup relations.
///
/// Groups live in their owning node's fields. Obtaining this view or an inner
/// collection is constant time and shares the stored index arrays. Empty
/// groups remain present, and retaining either view keeps the store alive.
public struct MarkupGroups<Value: Sendable>: RandomAccessCollection, Sendable {
    /// Zero-based position of a group.
    public typealias Index = Int
    let tree: ValueTree
    let recordIndices: [[Int]]

    /// The first valid position, or `endIndex` for no groups.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last group.
    public var endIndex: Int { recordIndices.endIndex }
    /// The group at a valid position, without copying its elements.
    public subscript(position: Int) -> MarkupCollection<Value> {
        MarkupCollection(tree: tree, recordIndices: recordIndices[position])
    }
}

/// Records contain scalar values and integer edges only. Consequently ARC
/// destruction has bounded stack depth, including after extracting a subtree.
enum StoredMarkup: Sendable {
    // Only the root owns Metadata. Box that payload once so its inline
    // capacity is not paid by every record, including every Text leaf.
    indirect case document(Document.Fields)
    case callout(Callout.Fields)
    case definitionList(DefinitionList.Fields)
    case definition(Definition.Fields)
    case paragraph(Paragraph.Fields)
    case heading(Heading.Fields)
    case thematicBreak(ThematicBreak)
    case list(List.Fields)
    case listItem(ListItem.Fields)
    case codeBlock(CodeBlock)
    case htmlBlock(HTMLBlock)
    case formulaBlock(FormulaBlock)
    case table(Table.Fields)
    case directiveBlock(DirectiveBlock.Fields)
    case text(Text)
    case softBreak(SoftBreak)
    case lineBreak(LineBreak)
    case code(Code)
    case html(HTML)
    case comment(Comment)
    case crossLink(CrossLink)
    case crossEmbedded(CrossEmbedded)
    case formula(Formula)
    case emphasis(Emphasis.Fields)
    case strong(Strong.Fields)
    case strikethrough(Strikethrough.Fields)
    case mark(Mark.Fields)
    case insertion(Insertion.Fields)
    case span(Span.Fields)
    case superscript(Superscript.Fields)
    case `subscript`(Subscript.Fields)
    case link(Link.Fields)
    case media(Media.Fields)
    case directive(Directive.Fields)
    case cite(Cite.Fields)
    case tableCaption(TableCaption.Fields)
    case tableRow(TableRow.Fields)
    case tableCell(TableCell.Fields)
    case directiveLabel(DirectiveLabel.Fields)
    case footnote(Footnote.Fields)
    case specimen(Specimen.Fields)
    case citation(Citation.Fields)
}

final class ValueTree: Sendable {
    let records: [StoredMarkup]

    init(records: [StoredMarkup]) {
        self.records = records
    }

    // Exhaustive dispatch preserves the one-to-one stored-kind projection.
    // swiftlint:disable:next cyclomatic_complexity
    func value<Value: Sendable>(at index: Int, as type: Value.Type) -> Value {
        let value: any Sendable
        switch records[index] {
        case .document: value = Document(tree: self, index: index)
        case .callout: value = Callout(tree: self, index: index)
        case .definitionList: value = DefinitionList(tree: self, index: index)
        case .definition: value = Definition(tree: self, index: index)
        case .paragraph: value = Paragraph(tree: self, index: index)
        case .heading: value = Heading(tree: self, index: index)
        case let .thematicBreak(node): value = node
        case .list: value = List(tree: self, index: index)
        case .listItem: value = ListItem(tree: self, index: index)
        case let .codeBlock(node): value = node
        case let .htmlBlock(node): value = node
        case let .formulaBlock(node): value = node
        case .table: value = Table(tree: self, index: index)
        case .directiveBlock: value = DirectiveBlock(tree: self, index: index)
        case let .text(node): value = node
        case let .softBreak(node): value = node
        case let .lineBreak(node): value = node
        case let .code(node): value = node
        case let .html(node): value = node
        case let .comment(node): value = node
        case let .crossLink(node): value = node
        case let .crossEmbedded(node): value = node
        case let .formula(node): value = node
        case .emphasis: value = Emphasis(tree: self, index: index)
        case .strong: value = Strong(tree: self, index: index)
        case .strikethrough: value = Strikethrough(tree: self, index: index)
        case .mark: value = Mark(tree: self, index: index)
        case .insertion: value = Insertion(tree: self, index: index)
        case .span: value = Span(tree: self, index: index)
        case .superscript: value = Superscript(tree: self, index: index)
        case .subscript: value = Subscript(tree: self, index: index)
        case .link: value = Link(tree: self, index: index)
        case .media: value = Media(tree: self, index: index)
        case .directive: value = Directive(tree: self, index: index)
        case .cite: value = Cite(tree: self, index: index)
        case .tableCaption: value = TableCaption(tree: self, index: index)
        case .tableRow: value = TableRow(tree: self, index: index)
        case .tableCell: value = TableCell(tree: self, index: index)
        case .directiveLabel: value = DirectiveLabel(tree: self, index: index)
        case .footnote: value = Footnote(tree: self, index: index)
        case .specimen: value = Specimen(tree: self, index: index)
        case .citation: value = Citation(tree: self, index: index)
        }
        guard let result = value as? Value else {
            preconditionFailure("Invalid value kind in a typed relation")
        }
        return result
    }
}
