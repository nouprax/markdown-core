/// The canonical walk of a document's tree, in pre-order, with every node's
/// absolute byte range and the dump's line layout.
///
/// It mirrors the C walk: each relation's first node is placed from its owner's
/// start and each later node from the end of the one before it, and a named
/// relation yields a line of its own before its nodes. A node whose runs read
/// content, some run's length being positive, is an inline root: its first
/// relation is its content, whose nodes and everything under them are placed
/// in that content from 0, and the walk holds the root's runs while it is in
/// it. Roots never nest. The frames are the tree's depth; the call stack stays
/// constant.
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

    /// One run of a node in absolute offsets: the content offset its bytes
    /// start at, how many there are, and the source range they were read
    /// from. A run of size 0 is source the node reads without content.
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
    /// The runs of the block whose places were asked for last.
    private var own: [SourceRun] = []

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
    /// source order, in place of what `places` held: a window less the gaps
    /// between the runs that place it. In an inline root's content the window
    /// is the source its content range was read from, cut by the root's runs;
    /// otherwise it is the node's range, cut by its own runs.
    mutating func places(of item: Item, into places: inout [(start: Int, end: Int)]) {
        places.removeAll(keepingCapacity: true)
        if item.content {
            let start = place(of: item.start)
            let end = item.end > item.start ? placeEnd(of: item.end) : start
            Self.cut(start..<end, by: runs, into: &places)
        } else {
            Self.absolute(item.record?.runs ?? [], from: item.start, into: &own)
            Self.cut(item.start..<item.end, by: own, into: &places)
        }
    }

    /// Pushes the frame that walks `record`'s relations, and returns its line.
    /// An inline root, a node whose runs read content, has a frame that holds
    /// its runs, which its first relation's nodes are placed by.
    private mutating func enter(_ record: MarkupRecord, level: Int, start: Int, content: Bool, hasNext: Bool) -> Item {
        let root = record.runs.contains { $0.length > 0 }
        if root { Self.absolute(record.runs, from: start, into: &runs) }
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

    /// `runs`, measured from `start`, in absolute offsets, in place of what
    /// `into` held. Each starts `lead` past the end of the run before, or past
    /// `start` for the first.
    private static func absolute(_ runs: [Run], from start: Int, into held: inout [SourceRun]) {
        held.removeAll(keepingCapacity: true)
        var content = 0
        var cursor = start
        for run in runs {
            let start = cursor + Int(run.lead)
            cursor = start + Int(run.span)
            held.append(SourceRun(content: content, size: Int(run.length), start: start, end: cursor))
            content += Int(run.length)
        }
    }

    /// Appends the source ranges of `window` less the gaps between `runs`, in
    /// source order: the source between two runs is not the node's. An empty
    /// window is one empty range.
    private static func cut(_ window: Range<Int>, by runs: [SourceRun], into places: inout [(start: Int, end: Int)]) {
        if window.isEmpty {
            places.append((window.lowerBound, window.lowerBound))
            return
        }
        // The first run that ends inside the window or past it.
        var lower = 0
        var upper = runs.count
        while lower < upper {
            let middle = lower + (upper - lower) / 2
            if runs[middle].end <= window.lowerBound {
                lower = middle + 1
            } else {
                upper = middle
            }
        }
        var from = window.lowerBound
        var index = lower
        while index + 1 < runs.count && runs[index].end < window.upperBound {
            let stop = runs[index].end
            let past = runs[index + 1].start
            index += 1
            if past <= stop { continue }
            if stop > from { places.append((from, stop)) }
            from = max(from, past)
        }
        if from < window.upperBound { places.append((from, window.upperBound)) }
    }

    /// The held run content offset `offset` is in: the last that starts at or
    /// before it and reads content, or the first run.
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
        while lower > 0 && runs[lower].size == 0 { lower -= 1 }
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

    /// Where the content byte before `offset`, which is past 0, is read to:
    /// past its source byte, or the end of the run that reads it whole.
    private func placeEnd(of offset: Int) -> Int {
        let run = runs[self.run(at: offset - 1)]
        if offset - 1 >= run.content + run.size || !run.copied { return run.end }
        return run.start + (offset - run.content)
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
