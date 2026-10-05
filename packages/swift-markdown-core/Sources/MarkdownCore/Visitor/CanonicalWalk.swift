/// The canonical walk of a document's tree, in pre-order, with every node's
/// absolute byte range and the dump's line layout.
///
/// It mirrors the C walk: each relation's first node is placed from its owner's
/// start and each later node from the end of the one before it, and a named
/// relation yields a line of its own before its nodes. A node with runs is an
/// inline root: its first relation is its content, whose nodes and everything
/// under them are placed in that content from 0, and the walk holds the root's
/// runs while it is in it. Roots never nest. The frames are the tree's depth;
/// the call stack stays constant.
struct CanonicalWalk {
    /// One line of the walk: a node with its range, or a named relation.
    struct Item {
        /// The node, or `nil` for a named relation's line.
        let record: MarkupRecord?
        /// A named relation's name, or `nil` for a node.
        let name: String?
        /// The number of nodes a named relation holds.
        let count: Int
        /// The node's absolute byte range: in the content of the inline root
        /// whose runs the walk holds when `content`, and in the source
        /// otherwise.
        let start: Int
        let end: Int
        let content: Bool
        /// The nesting level the line is drawn at, the root's being 0.
        let level: Int
        /// Whether another line follows at this level under the same owner.
        let hasNext: Bool
    }

    /// One run of the held inline root's content in absolute offsets: the
    /// content offset its bytes start at, how many there are, and the source
    /// range they were read from.
    private struct SourceRun {
        let content: Int
        let size: Int
        let start: Int
        let end: Int

        /// Whether it reads each content byte from one source byte, rather
        /// than all of its content from all of its source.
        var copied: Bool { end - start == size }
    }

    private struct Frame {
        let record: MarkupRecord
        let level: Int
        let start: Int
        /// Whether the relation in hand is in an inline root's content, and
        /// whether the frame's node is that root.
        var content: Bool
        var root: Bool
        var step = 0
        var relation: Relation?
        var index = 0
        var anchor = 0
        var named = false
    }

    private var root: MarkupRecord?
    private var frames: [Frame] = []
    /// The runs of the inline root whose content the walk is in.
    private var runs: [SourceRun] = []

    /// Walks `root`, a document's root, whose extent is relative to offset 0.
    init(root: MarkupRecord) {
        self.root = root
    }

    mutating func next() -> Item? {
        if let root {
            self.root = nil
            return enter(root, level: 0, start: Int(root.extent.lead), content: false, hasNext: false)
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
                    content: false,
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
                return enter(
                    child,
                    level: frame.level + (named ? 2 : 1),
                    start: start,
                    content: frame.content,
                    hasNext: hasNext
                )
            }
            frames[top].relation = nil
            // A root's later relations are not its content.
            if frames[top].root {
                frames[top].root = false
                frames[top].content = false
            }
        }
        return nil
    }

    /// Starts the next relation of the frame at `top`; false when it has none.
    private mutating func open(_ top: Int) -> Bool {
        guard let relation = frames[top].record.relation(at: frames[top].step) else { return false }
        frames[top].step += 1
        frames[top].relation = relation
        frames[top].index = relation.indices.lowerBound
        // A root's first relation is its content, which runs from 0.
        frames[top].anchor = frames[top].root ? 0 : frames[top].start
        frames[top].named = relation.name != nil
        return true
    }

    /// The source ranges of `item`, the node the walk returned last, in
    /// source order, in place of what `places` held: the source its content
    /// range was read from, its pieces, or its one range.
    func places(of item: Item, into places: inout [(start: Int, end: Int)]) {
        places.removeAll(keepingCapacity: true)
        let pieces = item.record?.pieces ?? []
        if item.content {
            read(from: item.start, to: item.end, into: &places)
        } else if !pieces.isEmpty {
            var cursor = item.start
            for piece in pieces {
                let start = cursor + Int(piece.lead)
                cursor = start + Int(piece.span)
                places.append((start, cursor))
            }
        } else {
            places.append((item.start, item.end))
        }
    }

    /// Pushes the frame that walks `record`'s relations, and returns its line.
    /// An inline root's frame reads its runs, which its first relation's
    /// nodes are placed by.
    private mutating func enter(_ record: MarkupRecord, level: Int, start: Int, content: Bool, hasNext: Bool) -> Item {
        let root = !record.runs.isEmpty
        if root { hold(record.runs, from: start) }
        frames.append(Frame(record: record, level: level, start: start, content: content || root, root: root))
        let end = start + Int(record.extent.span)
        return Item(
            record: record,
            name: nil,
            count: 0,
            start: start,
            end: end,
            content: content,
            level: level,
            hasNext: hasNext
        )
    }

    /// Holds `runs`, measured from `start`, as the runs of the content the
    /// walk enters.
    private mutating func hold(_ runs: [Run], from start: Int) {
        self.runs.removeAll(keepingCapacity: true)
        var content = 0
        var cursor = start
        for run in runs {
            let start = cursor + Int(run.lead)
            cursor = start + Int(run.span)
            self.runs.append(SourceRun(content: content, size: Int(run.length), start: start, end: cursor))
            content += Int(run.length)
        }
    }

    /// The source the content range `start..<end` was read from: the parts of
    /// the runs it covers, touching parts joined, in source order; an empty
    /// range is one empty range where its offset is read from.
    private func read(from start: Int, to end: Int, into places: inout [(start: Int, end: Int)]) {
        if end <= start {
            let spot = place(of: start)
            places.append((spot, spot))
            return
        }
        var index = self.run(at: start)
        while index < runs.count && runs[index].content < end {
            let run = runs[index]
            index += 1
            let lower = max(start, run.content)
            let upper = min(end, run.content + run.size)
            if lower >= upper { continue }
            let part =
                run.copied
                ? (start: run.start + (lower - run.content), end: run.start + (upper - run.content))
                : (start: run.start, end: run.end)
            if let last = places.last, last.end == part.start {
                places[places.count - 1].end = part.end
            } else {
                places.append(part)
            }
        }
    }

    /// The run content offset `offset` is in: the last that starts at or
    /// before it.
    private func run(at offset: Int) -> Int {
        var lower = 0
        var upper = runs.count
        while upper - lower > 1 {
            let middle = lower + (upper - lower) / 2
            if runs[middle].content <= offset {
                lower = middle
            } else {
                upper = middle
            }
        }
        return lower
    }

    /// Where content offset `offset` is read from: its source byte, the start
    /// of the run that reads it whole, or, past the content, where the content
    /// ends.
    private func place(of offset: Int) -> Int {
        let run = runs[self.run(at: offset)]
        if offset >= run.content + run.size { return run.end }
        return run.copied ? run.start + (offset - run.content) : run.start
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
