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
    // How an index becomes an element, decided once where the relation's
    // static type is known: the one kind a typed relation holds, or any kind
    // for `any Markup`. No element is cast at runtime.
    let resolve: @Sendable (MarkupStore, Int) -> Element

    /// The first valid position, or `endIndex` for an empty relation.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last element.
    public var endIndex: Int { recordIndices.endIndex }
    /// The value at a valid position, without copying its descendants.
    public subscript(position: Int) -> Element {
        resolve(store, recordIndices[position])
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
    let resolve: @Sendable (MarkupStore, Int) -> Value

    /// The first valid position, or `endIndex` for no groups.
    public var startIndex: Int { recordIndices.startIndex }
    /// The position immediately after the last group.
    public var endIndex: Int { recordIndices.endIndex }
    /// The group at a valid position, without copying its elements.
    public subscript(position: Int) -> MarkupCollection<Value> {
        MarkupCollection(store: store, recordIndices: recordIndices[position], resolve: resolve)
    }
}

/// A typed index into the store containing its owner. A relation's stored
/// representation never retains the store it is resolved against.
struct MarkupReference<Value: Sendable>: Sendable {
    let index: Int
}

/// Ordered node indices; creating the collection view does not copy their storage.
struct MarkupReferences<Element: Sendable>: Sendable {
    let indices: [Int]
}

/// Grouped node indices, including empty groups.
struct MarkupGroupReferences<Element: Sendable>: Sendable {
    let indices: [[Int]]
}

/// The fields of one stored kind: each knows the one record case that holds
/// it, so a field is read by a tag check, not by a runtime cast.
protocol StoredFields: Sendable {
    static func project(_ record: StoredMarkup) -> Self
}

/// A kind that is read out of the store by index: the one construction a typed
/// relation resolves its elements through, so no element is cast at runtime.
protocol MarkupElement: Markup {
    static func stored(at index: Int, in store: MarkupStore) -> Self
}

/// Where a stored view lives: the store and the record it reads.
struct StoredLocation {
    let store: MarkupStore
    let index: Int
}

/// A view over one stored record. A walk starts from it and continues over the
/// store's records, never over values.
protocol StoredMarkupView {
    var storedLocation: StoredLocation { get }
}

/// A typed field view. Values pass through; relations resolve against the same store.
/// Only this view retains the store, keeping stored records free of ownership cycles.
@dynamicMemberLookup
struct Stored<Fields: StoredFields>: Sendable {
    let store: MarkupStore
    let index: Int

    init(store: MarkupStore, index: Int) {
        self.store = store
        self.index = index
    }

    var location: StoredLocation { StoredLocation(store: store, index: index) }

    /// One field, projected in place from the record: the record is read
    /// where it lies and only the field the caller asked for leaves it.
    @inline(__always)
    func read<Value>(_ body: (Fields) -> Value) -> Value {
        store.records.withUnsafeBufferPointer { records in body(Fields.project(records[index])) }
    }

    // Relations resolve to views over the same store. The element resolution
    // is chosen here, by the relation's static type, once per collection.
    @inline(__always)
    func children(_ body: (Fields) -> MarkupReferences<any Markup>) -> MarkupCollection<any Markup> {
        MarkupCollection(store: store, recordIndices: read(body).indices, resolve: MarkupStore.anyMarkup)
    }

    @inline(__always)
    func optionalChildren(_ body: (Fields) -> MarkupReferences<any Markup>?) -> MarkupCollection<any Markup>? {
        read(body).map { MarkupCollection(store: store, recordIndices: $0.indices, resolve: MarkupStore.anyMarkup) }
    }

    @inline(__always)
    func elements<Element: MarkupElement>(_ body: (Fields) -> MarkupReferences<Element>) -> MarkupCollection<Element> {
        MarkupCollection(store: store, recordIndices: read(body).indices, resolve: { Element.stored(at: $1, in: $0) })
    }

    @inline(__always)
    func optionalElement<Element: MarkupElement>(_ body: (Fields) -> MarkupReference<Element>?) -> Element? {
        read(body).map { Element.stored(at: $0.index, in: store) }
    }

    @inline(__always)
    func groups(_ body: (Fields) -> MarkupGroupReferences<any Markup>) -> MarkupGroups<any Markup> {
        MarkupGroups(store: store, recordIndices: read(body).indices, resolve: MarkupStore.anyMarkup)
    }

    // Key-path access for tests and tools; the node types read through the
    // closures above so the field is a direct load.
    subscript<Value>(dynamicMember keyPath: KeyPath<Fields, Value>) -> Value {
        read { $0[keyPath: keyPath] }
    }

    subscript(dynamicMember keyPath: KeyPath<Fields, MarkupReferences<any Markup>>) -> MarkupCollection<any Markup> {
        children { $0[keyPath: keyPath] }
    }

    subscript(dynamicMember keyPath: KeyPath<Fields, MarkupReferences<any Markup>?>) -> MarkupCollection<any Markup>? {
        optionalChildren { $0[keyPath: keyPath] }
    }

    subscript<Element: MarkupElement>(
        dynamicMember keyPath: KeyPath<Fields, MarkupReferences<Element>>
    ) -> MarkupCollection<Element> {
        elements { $0[keyPath: keyPath] }
    }

    subscript<Element: MarkupElement>(dynamicMember keyPath: KeyPath<Fields, MarkupReference<Element>?>) -> Element? {
        optionalElement { $0[keyPath: keyPath] }
    }

    subscript(dynamicMember keyPath: KeyPath<Fields, MarkupGroupReferences<any Markup>>) -> MarkupGroups<any Markup> {
        groups { $0[keyPath: keyPath] }
    }
}

/// Records contain scalar values and integer edges only. Consequently ARC
/// destruction has bounded stack depth, including after extracting a subtree.
/// Every kind but metadata is its fields; a view over the record is 16 bytes
/// and fits the existential inline buffer, so no `any Markup` is boxed.
enum StoredMarkup: Sendable {
    case callout(Callout.Fields)
    case citation(Citation.Fields)
    case cite(Cite.Fields)
    case code(Code.Fields)
    case codeBlock(CodeBlock.Fields)
    case comment(Comment.Fields)
    case crossEmbedded(CrossEmbedded.Fields)
    case crossLink(CrossLink.Fields)
    case definition(Definition.Fields)
    case definitionList(DefinitionList.Fields)
    case directive(Directive.Fields)
    case directiveBlock(DirectiveBlock.Fields)
    case directiveLabel(DirectiveLabel.Fields)
    case document(Document.Fields)
    case embedded(Embedded.Fields)
    case emphasis(Emphasis.Fields)
    case footnote(Footnote.Fields)
    case formula(Formula.Fields)
    case formulaBlock(FormulaBlock.Fields)
    case heading(Heading.Fields)
    case html(HTML.Fields)
    case htmlBlock(HTMLBlock.Fields)
    case insertion(Insertion.Fields)
    case lineBreak(LineBreak.Fields)
    case link(Link.Fields)
    case list(List.Fields)
    case listItem(ListItem.Fields)
    case mark(Mark.Fields)
    case paragraph(Paragraph.Fields)
    case softBreak(SoftBreak.Fields)
    case span(Span.Fields)
    case specimen(Specimen.Fields)
    case strikethrough(Strikethrough.Fields)
    case strong(Strong.Fields)
    case `subscript`(Subscript.Fields)
    case superscript(Superscript.Fields)
    case table(Table.Fields)
    case tableCaption(TableCaption.Fields)
    case tableCell(TableCell.Fields)
    case tableRow(TableRow.Fields)
    case text(Text.Fields)
    case thematicBreak(ThematicBreak.Fields)

    // Boxed leaf node.
    // Keep the large metadata payload out of every other record.
    indirect case metadata(Metadata)
}

final class MarkupStore: Sendable {
    let records: [StoredMarkup]

    init(records: [StoredMarkup]) {
        self.records = records
    }

    /// The resolution of an `any Markup` relation: the record's kind chooses the
    /// view, upcast where it is made, with no cast at any read.
    static func anyMarkup(_ store: MarkupStore, _ index: Int) -> any Markup {
        store.markup(at: index)
    }

    // Exhaustive dispatch preserves the one-to-one stored-kind projection.
    // swiftlint:disable:next cyclomatic_complexity
    func markup(at index: Int) -> any Markup {
        switch records[index] {
        case .callout: return Callout(fields: Stored(store: self, index: index))
        case .citation: return Citation(fields: Stored(store: self, index: index))
        case .cite: return Cite(fields: Stored(store: self, index: index))
        case .code: return Code(fields: Stored(store: self, index: index))
        case .codeBlock: return CodeBlock(fields: Stored(store: self, index: index))
        case .comment: return Comment(fields: Stored(store: self, index: index))
        case .crossEmbedded: return CrossEmbedded(fields: Stored(store: self, index: index))
        case .crossLink: return CrossLink(fields: Stored(store: self, index: index))
        case .definition: return Definition(fields: Stored(store: self, index: index))
        case .definitionList: return DefinitionList(fields: Stored(store: self, index: index))
        case .directive: return Directive(fields: Stored(store: self, index: index))
        case .directiveBlock: return DirectiveBlock(fields: Stored(store: self, index: index))
        case .directiveLabel: return DirectiveLabel(fields: Stored(store: self, index: index))
        case .document: return Document(fields: Stored(store: self, index: index))
        case .embedded: return Embedded(fields: Stored(store: self, index: index))
        case .emphasis: return Emphasis(fields: Stored(store: self, index: index))
        case .footnote: return Footnote(fields: Stored(store: self, index: index))
        case .formula: return Formula(fields: Stored(store: self, index: index))
        case .formulaBlock: return FormulaBlock(fields: Stored(store: self, index: index))
        case .heading: return Heading(fields: Stored(store: self, index: index))
        case .html: return HTML(fields: Stored(store: self, index: index))
        case .htmlBlock: return HTMLBlock(fields: Stored(store: self, index: index))
        case .insertion: return Insertion(fields: Stored(store: self, index: index))
        case .lineBreak: return LineBreak(fields: Stored(store: self, index: index))
        case .link: return Link(fields: Stored(store: self, index: index))
        case .list: return List(fields: Stored(store: self, index: index))
        case .listItem: return ListItem(fields: Stored(store: self, index: index))
        case .mark: return Mark(fields: Stored(store: self, index: index))
        case .paragraph: return Paragraph(fields: Stored(store: self, index: index))
        case .softBreak: return SoftBreak(fields: Stored(store: self, index: index))
        case .span: return Span(fields: Stored(store: self, index: index))
        case .specimen: return Specimen(fields: Stored(store: self, index: index))
        case .strikethrough: return Strikethrough(fields: Stored(store: self, index: index))
        case .strong: return Strong(fields: Stored(store: self, index: index))
        case .`subscript`: return Subscript(fields: Stored(store: self, index: index))
        case .superscript: return Superscript(fields: Stored(store: self, index: index))
        case .table: return Table(fields: Stored(store: self, index: index))
        case .tableCaption: return TableCaption(fields: Stored(store: self, index: index))
        case .tableCell: return TableCell(fields: Stored(store: self, index: index))
        case .tableRow: return TableRow(fields: Stored(store: self, index: index))
        case .text: return Text(fields: Stored(store: self, index: index))
        case .thematicBreak: return ThematicBreak(fields: Stored(store: self, index: index))
        case let .metadata(node): return node
        }
    }
}
