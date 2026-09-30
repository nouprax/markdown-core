/// The canonical walk of a subtree, in pre-order, with every node's absolute
/// byte range and the dump's line layout.
///
/// It mirrors the C walk: each relation's first node is placed from its owner's
/// start and each later node from the end of the one before it, and a named
/// relation yields a line of its own before its nodes. The frames are the
/// tree's depth; the call stack stays constant.
struct CanonicalWalk {
    /// One line of the walk: a node with its range, or a named relation.
    struct Item {
        /// The node, or `nil` for a named relation's line.
        let record: MarkupRecord?
        /// A named relation's name, or `nil` for a node.
        let name: String?
        /// The number of nodes a named relation holds.
        let count: Int
        /// The node's absolute byte range in the source.
        let start: Int
        let end: Int
        /// The nesting level the line is drawn at, the root's being 0.
        let level: Int
        /// Whether another line follows at this level under the same owner.
        let hasNext: Bool
    }

    private struct Frame {
        let record: MarkupRecord
        let level: Int
        let start: Int
        var step = 0
        var relation: Relation?
        var index = 0
        var anchor = 0
        var named = false
    }

    private var root: MarkupRecord?
    private let rootAnchor: Int
    private var frames: [Frame] = []

    /// Walks `root`, whose extent is relative to the absolute offset `anchor`.
    init(root: MarkupRecord, anchor: Int) {
        self.root = root
        rootAnchor = anchor
    }

    mutating func next() -> Item? {
        if let root {
            self.root = nil
            return enter(root, level: 0, start: rootAnchor + Int(root.extent.lead), hasNext: false)
        }
        while !frames.isEmpty {
            let top = frames.count - 1
            if frames[top].relation == nil {
                guard let relation = frames[top].record.relation(at: frames[top].step) else {
                    frames.removeLast()
                    continue
                }
                frames[top].step += 1
                frames[top].relation = relation
                frames[top].index = relation.indices.lowerBound
                frames[top].anchor = frames[top].start
                frames[top].named = relation.name != nil
            }
            let frame = frames[top]
            guard let relation = frame.relation else { continue }
            let named = relation.name != nil
            if frame.named {
                // The relation's own nodes follow its line one level down, so
                // what follows it at its own level is the owner's next relation.
                frames[top].named = false
                let more = Self.more(after: frame)
                return Item(
                    record: nil,
                    name: relation.name,
                    count: relation.indices.count,
                    start: 0,
                    end: 0,
                    level: frame.level + 1,
                    hasNext: more
                )
            }
            if frame.index < relation.indices.upperBound {
                let child = frame.record.children[frame.index]
                let start = frame.anchor + Int(child.extent.lead)
                frames[top].index += 1
                frames[top].anchor = start + Int(child.extent.span)
                let hasNext = frame.index + 1 < relation.indices.upperBound || (!named && Self.more(after: frame))
                return enter(child, level: frame.level + (named ? 2 : 1), start: start, hasNext: hasNext)
            }
            frames[top].relation = nil
        }
        return nil
    }

    /// Pushes the frame that walks `record`'s relations, and returns its line.
    private mutating func enter(_ record: MarkupRecord, level: Int, start: Int, hasNext: Bool) -> Item {
        frames.append(Frame(record: record, level: level, start: start))
        let end = start + Int(record.extent.span)
        return Item(record: record, name: nil, count: 0, start: start, end: end, level: level, hasNext: hasNext)
    }

    /// Whether the owner has another line at its own level after the current
    /// relation: a later named relation, or a later relation with nodes.
    private static func more(after frame: Frame) -> Bool {
        var step = frame.step
        while let relation = frame.record.relation(at: step) {
            if relation.name != nil || !relation.indices.isEmpty { return true }
            step += 1
        }
        return false
    }
}
