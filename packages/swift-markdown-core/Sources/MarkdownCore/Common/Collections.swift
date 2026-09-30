/// A read-only, random-access relation of a node.
///
/// It shares its owner's child records: count, indexing, and slicing are
/// constant time and copy no descendants. Use Array(...) to request an
/// independent array. A retained element keeps only its own subtree alive.
public struct MarkupCollection<Element: Sendable>: RandomAccessCollection, Sendable {
    /// Zero-based position within this relation.
    public typealias Index = Int
    let records: ArraySlice<MarkupRecord>

    /// The first valid position, or `endIndex` for an empty relation.
    public var startIndex: Int { 0 }
    /// The position immediately after the last element.
    public var endIndex: Int { records.count }
    /// The value at a valid position, without copying its descendants.
    public subscript(position: Int) -> Element {
        guard let value = records[records.startIndex + position].markup as? Element else {
            preconditionFailure("A relation holds only its declared kind")
        }
        return value
    }
}

/// A read-only, random-access collection of grouped relations.
///
/// Every group is a relation of the same owner. Obtaining this view or an inner
/// collection is constant time and shares the owner's child records. Empty
/// groups remain present.
public struct MarkupGroups<Value: Sendable>: RandomAccessCollection, Sendable {
    /// Zero-based position of a group.
    public typealias Index = Int
    let owner: MarkupRecord
    /// Boundaries in the owner's children: group `n` runs from `bounds[n]` to
    /// `bounds[n + 1]`.
    let bounds: [Int]

    /// The first valid position, or `endIndex` for no groups.
    public var startIndex: Int { 0 }
    /// The position immediately after the last group.
    public var endIndex: Int { bounds.count - 1 }
    /// The group at a valid position, without copying its elements.
    public subscript(position: Int) -> MarkupCollection<Value> {
        MarkupCollection(records: owner.children[bounds[position]..<bounds[position + 1]])
    }
}

extension MarkupRecord {
    /// The children at `indices` as a relation of `Element`.
    func collection<Element: Sendable>(_ indices: Range<Int>) -> MarkupCollection<Element> {
        MarkupCollection(records: children[indices])
    }
}
