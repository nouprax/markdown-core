/// The canonical walk of a document's tree, in pre-order, with every node's
/// absolute source and the dump's line layout.
///
/// It mirrors the C walk: each relation's first node leads from the start of
/// its owner's source and each later node from the end of the source of the
/// one before it, and a named relation yields a line of its own before its
/// nodes. A node's source runs from where its first run starts to where its
/// last ends. The frames are the tree's depth; the call stack stays constant.
struct CanonicalWalk {
    /// One line of the walk: a node with its source, or a named relation.
    struct Item {
        /// The node, or `nil` for a named relation's line.
        let record: MarkupRecord?
        /// A named relation's name, or `nil` for a node.
        let name: String?
        /// The number of nodes a named relation holds.
        let count: Int
        /// The node's absolute source in bytes.
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
        /// Where the node's source starts, which its relations' first nodes
        /// lead from.
        let start: Int
        var step = 0
        var relation: Relation?
        var index = 0
        var anchor = 0
        var named = false
    }

    private var root: MarkupRecord?
    private var frames: [Frame] = []

    /// Walks `root`, a document's root, whose source leads from offset 0.
    init(root: MarkupRecord) {
        self.root = root
    }

    mutating func next() -> Item? {
        if let root {
            self.root = nil
            return enter(root, level: 0, anchor: 0, hasNext: false)
        }
        while !frames.isEmpty {
            let top = frames.count - 1
            if frames[top].relation == nil && !open(top) {
                frames.removeLast()
                continue
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
                frames[top].index += 1
                let hasNext = frame.index + 1 < relation.indices.upperBound || (!named && Self.more(after: frame))
                let item = enter(child, level: frame.level + (named ? 2 : 1), anchor: frame.anchor, hasNext: hasNext)
                frames[top].anchor = item.end
                return item
            }
            frames[top].relation = nil
        }
        return nil
    }

    /// Starts the next relation of the frame at `top`; false when it has none.
    private mutating func open(_ top: Int) -> Bool {
        guard let relation = frames[top].record.relation(at: frames[top].step) else { return false }
        frames[top].step += 1
        frames[top].relation = relation
        frames[top].index = relation.indices.lowerBound
        frames[top].anchor = frames[top].start
        frames[top].named = relation.name != nil
        return true
    }

    /// The source ranges of `item`, the node the walk returned last, in
    /// source order, in place of what `places` held: its runs. A named
    /// relation's line has none.
    func places(of item: Item, into places: inout [(start: Int, end: Int)]) {
        places.removeAll(keepingCapacity: true)
        guard let runs = item.record?.runs, let first = runs.first else { return }
        var cursor = item.start - Int(first.lead)
        for run in runs {
            let start = cursor + Int(run.lead)
            cursor = start + Int(run.span)
            places.append((start, cursor))
        }
    }

    /// Pushes the frame that walks `record`'s relations, and returns its line.
    /// Its source leads from `anchor`.
    private mutating func enter(_ record: MarkupRecord, level: Int, anchor: Int, hasNext: Bool) -> Item {
        let source = Self.source(of: record, from: anchor)
        frames.append(Frame(record: record, level: level, start: source.start))
        return Item(
            record: record,
            name: nil,
            count: 0,
            start: source.start,
            end: source.end,
            level: level,
            hasNext: hasNext
        )
    }

    /// The source of `record` whose lead is from `anchor`: from where its
    /// first run starts to where its last ends, each run starting its `lead`
    /// past the end of the run before.
    private static func source(of record: MarkupRecord, from anchor: Int) -> (start: Int, end: Int) {
        var cursor = anchor
        var start = anchor
        for (index, run) in record.runs.enumerated() {
            cursor += Int(run.lead)
            if index == 0 { start = cursor }
            cursor += Int(run.span)
        }
        return (start, cursor)
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
