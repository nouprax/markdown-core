/// A read-only, random-access relation in an immutable Swift value store.
///
/// Count, indexing, and collection views are constant time. Use Array(...) to
/// request an independent array. A retained element keeps the immutable Swift
/// storage alive; no native parse, cache, lock, or recursive ownership is kept.
public struct MarkupCollection<Element: Sendable>: RandomAccessCollection, Sendable {
    /// Zero-based position within this relation.
    public typealias Index = Int
    let store: MarkupStore
    let recordIndices: [Int]

    /// The first valid position, or `endIndex` for an empty relation.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last element.
    public var endIndex: Int { recordIndices.endIndex }
    /// The value at a valid position, without copying its descendants.
    public subscript(position: Int) -> Element {
        store.value(at: recordIndices[position], as: Element.self)
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
    let store: MarkupStore
    let recordIndices: [[Int]]

    /// The first valid position, or `endIndex` for no groups.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last group.
    public var endIndex: Int { recordIndices.endIndex }
    /// The group at a valid position, without copying its elements.
    public subscript(position: Int) -> MarkupCollection<Value> {
        MarkupCollection(store: store, recordIndices: recordIndices[position])
    }
}

/// A typed reference into one immutable store; payloads are queried, never cached.
@propertyWrapper
struct Stored<Fields: Sendable>: Sendable {
    private let store: MarkupStore
    private let index: Int

    fileprivate init(store: MarkupStore, index: Int) {
        self.store = store
        self.index = index
    }

    var wrappedValue: Fields { store.fields(at: index) }
    var projectedValue: MarkupStore { store }
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
    case embedded(Embedded.Fields)
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

final class MarkupStore: Sendable {
    let records: [StoredMarkup]

    init(records: [StoredMarkup]) {
        self.records = records
    }

    func collection<Element: Sendable>(_ indices: [Int]) -> MarkupCollection<Element> {
        MarkupCollection(store: self, recordIndices: indices)
    }

    func groups<Element: Sendable>(_ indices: [[Int]]) -> MarkupGroups<Element> {
        MarkupGroups(store: self, recordIndices: indices)
    }

    // swiftlint:disable:next cyclomatic_complexity
    func fields<Fields: Sendable>(at index: Int) -> Fields {
        let fields: Fields?
        switch records[index] {
        case let .document(value): fields = value as? Fields
        case let .callout(value): fields = value as? Fields
        case let .definitionList(value): fields = value as? Fields
        case let .definition(value): fields = value as? Fields
        case let .paragraph(value): fields = value as? Fields
        case let .heading(value): fields = value as? Fields
        case let .list(value): fields = value as? Fields
        case let .listItem(value): fields = value as? Fields
        case let .table(value): fields = value as? Fields
        case let .directiveBlock(value): fields = value as? Fields
        case let .emphasis(value): fields = value as? Fields
        case let .strong(value): fields = value as? Fields
        case let .strikethrough(value): fields = value as? Fields
        case let .mark(value): fields = value as? Fields
        case let .insertion(value): fields = value as? Fields
        case let .span(value): fields = value as? Fields
        case let .superscript(value): fields = value as? Fields
        case let .subscript(value): fields = value as? Fields
        case let .link(value): fields = value as? Fields
        case let .embedded(value): fields = value as? Fields
        case let .directive(value): fields = value as? Fields
        case let .cite(value): fields = value as? Fields
        case let .tableCaption(value): fields = value as? Fields
        case let .tableRow(value): fields = value as? Fields
        case let .tableCell(value): fields = value as? Fields
        case let .directiveLabel(value): fields = value as? Fields
        case let .footnote(value): fields = value as? Fields
        case let .specimen(value): fields = value as? Fields
        case let .citation(value): fields = value as? Fields
        case .thematicBreak, .codeBlock, .htmlBlock, .formulaBlock, .text, .softBreak, .lineBreak, .code, .html,
            .comment, .crossLink, .crossEmbedded, .formula:
            fields = nil
        }
        guard let fields else {
            preconditionFailure("Invalid stored fields for \(Fields.self)")
        }
        return fields
    }

    // Exhaustive dispatch preserves the one-to-one stored-kind projection.
    // swiftlint:disable:next cyclomatic_complexity
    func value<Value: Sendable>(at index: Int, as type: Value.Type) -> Value {
        let value: any Sendable
        switch records[index] {
        case .document: value = Document(fields: Stored(store: self, index: index))
        case .callout: value = Callout(fields: Stored(store: self, index: index))
        case .definitionList: value = DefinitionList(fields: Stored(store: self, index: index))
        case .definition: value = Definition(fields: Stored(store: self, index: index))
        case .paragraph: value = Paragraph(fields: Stored(store: self, index: index))
        case .heading: value = Heading(fields: Stored(store: self, index: index))
        case let .thematicBreak(node): value = node
        case .list: value = List(fields: Stored(store: self, index: index))
        case .listItem: value = ListItem(fields: Stored(store: self, index: index))
        case let .codeBlock(node): value = node
        case let .htmlBlock(node): value = node
        case let .formulaBlock(node): value = node
        case .table: value = Table(fields: Stored(store: self, index: index))
        case .directiveBlock: value = DirectiveBlock(fields: Stored(store: self, index: index))
        case let .text(node): value = node
        case let .softBreak(node): value = node
        case let .lineBreak(node): value = node
        case let .code(node): value = node
        case let .html(node): value = node
        case let .comment(node): value = node
        case let .crossLink(node): value = node
        case let .crossEmbedded(node): value = node
        case let .formula(node): value = node
        case .emphasis: value = Emphasis(fields: Stored(store: self, index: index))
        case .strong: value = Strong(fields: Stored(store: self, index: index))
        case .strikethrough: value = Strikethrough(fields: Stored(store: self, index: index))
        case .mark: value = Mark(fields: Stored(store: self, index: index))
        case .insertion: value = Insertion(fields: Stored(store: self, index: index))
        case .span: value = Span(fields: Stored(store: self, index: index))
        case .superscript: value = Superscript(fields: Stored(store: self, index: index))
        case .subscript: value = Subscript(fields: Stored(store: self, index: index))
        case .link: value = Link(fields: Stored(store: self, index: index))
        case .embedded: value = Embedded(fields: Stored(store: self, index: index))
        case .directive: value = Directive(fields: Stored(store: self, index: index))
        case .cite: value = Cite(fields: Stored(store: self, index: index))
        case .tableCaption: value = TableCaption(fields: Stored(store: self, index: index))
        case .tableRow: value = TableRow(fields: Stored(store: self, index: index))
        case .tableCell: value = TableCell(fields: Stored(store: self, index: index))
        case .directiveLabel: value = DirectiveLabel(fields: Stored(store: self, index: index))
        case .footnote: value = Footnote(fields: Stored(store: self, index: index))
        case .specimen: value = Specimen(fields: Stored(store: self, index: index))
        case .citation: value = Citation(fields: Stored(store: self, index: index))
        }
        guard let result = value as? Value else {
            preconditionFailure("Invalid value kind in a typed relation")
        }
        return result
    }
}
