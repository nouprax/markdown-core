import MarkdownCoreC

/// Whether a list is bulleted or numbered.
///
/// It does not record WHICH marker was used: `-`, `*` and `+` are all
/// ``bullet``, and `1.` and `1)` are both ``ordered``.
public enum ListFlavor: String, Sendable {
    /// `-`, `*` or `+`.
    case bullet
    /// A number followed by `.` or `)`.
    case ordered
}

/// The punctuation after an ordered-list number.
public enum OrderedListDelimiter: String, Sendable {
    /// A period, as in `1.`.
    case period
    /// A parenthesis, as in `1)`.
    case parenthesis
}

/// The status a list item's task check gives it.
public enum TaskStatus: String, Sendable {
    /// `[ ]`.
    case open
    /// `[x]` or `[X]`.
    case done
    /// `[?]`.
    case question
    /// `[-]`.
    case cancelled
}

/// A bulleted or numbered list.
public struct List: Markup {
    let record: ListRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// A list owns `ListItem`s and nothing else.
    public var items: MarkupCollection<ListItem> { record.collection(record.children.indices) }
    /// Bulleted or numbered.
    public var flavor: ListFlavor { record.flavor }
    /// The first number an ordered list counts from, and `nil` for a bulleted
    /// one — which is the only reason it is optional.
    public var start: Int64? { record.start }
    /// The authored delimiter, or `nil` for a bullet list.
    public var delimiter: OrderedListDelimiter? { record.delimiter }
    /// Whether the source separated the items by blank lines. A loose list
    /// wraps each item's text in a ``Paragraph``; a tight one does not, so
    /// this is already visible in the tree and is stated here as well.
    public var tight: Bool { record.tight }
}

final class ListRecord: MarkupRecord, @unchecked Sendable {
    let flavor: ListFlavor
    let start: Int64?
    let delimiter: OrderedListDelimiter?
    let tight: Bool

    init(
        _ fields: InheritedFields,
        flavor: ListFlavor,
        start: Int64?,
        delimiter: OrderedListDelimiter?,
        tight: Bool,
        items: [MarkupRecord]
    ) {
        self.flavor = flavor
        self.start = start
        self.delimiter = delimiter
        self.tight = tight
        super.init(fields, children: items)
    }

    override var markup: any Markup { List(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: ListRecord.self)
        return flavor == other.flavor && start == other.start && delimiter == other.delimiter
            && tight == other.tight
    }
}

extension ListRecord {
    convenience init(from node: OpaquePointer, items: [MarkupRecord]) {
        var flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET
        var start = markdown_core_optional_i64()
        var delimiter = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
        var tight = false
        answered(markdown_core_node_list_properties(node, &flavor, &start, &delimiter, &tight))
        self.init(
            InheritedFields(from: node),
            flavor: flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? .ordered : .bullet,
            start: start.has_value ? start.value : nil,
            delimiter: start.has_value ? Self.delimiter(delimiter) : nil,
            tight: tight,
            items: items
        )
    }

    static func delimiter(_ value: markdown_core_ordered_list_delimiter) -> OrderedListDelimiter {
        // Any other value is MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD.
        value == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS ? .parenthesis : .period
    }
}

extension List: RecordBacked {
    var base: MarkupRecord { record }
}

/// One item of a ``List``.
public struct ListItem: Markup {
    let record: ListItemRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Where it is, relative to its neighbours. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The item's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// The status of the item's task check, or `nil` when this is not a task
    /// item.
    public var status: TaskStatus? { record.status }
}

final class ListItemRecord: MarkupRecord, @unchecked Sendable {
    let status: TaskStatus?

    init(_ fields: InheritedFields, status: TaskStatus?, content: [MarkupRecord]) {
        self.status = status
        super.init(fields, children: content)
    }

    override var markup: any Markup { ListItem(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        status == unsafeDowncast(other, to: ListItemRecord.self).status
    }
}

extension ListItemRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        var task = MARKDOWN_CORE_TASK_STATUS_NONE
        answered(markdown_core_node_list_item_status(node, &task))
        self.init(InheritedFields(from: node), status: TaskStatus(task), content: content)
    }
}

extension TaskStatus {
    /// The status the C core reports, `nil` for an item with no check.
    init?(_ status: markdown_core_task_status) {
        switch status {
        case MARKDOWN_CORE_TASK_STATUS_OPEN: self = .open
        case MARKDOWN_CORE_TASK_STATUS_DONE: self = .done
        case MARKDOWN_CORE_TASK_STATUS_QUESTION: self = .question
        case MARKDOWN_CORE_TASK_STATUS_CANCELLED: self = .cancelled
        default: return nil
        }
    }
}

extension ListItem: RecordBacked {
    var base: MarkupRecord { record }
}
