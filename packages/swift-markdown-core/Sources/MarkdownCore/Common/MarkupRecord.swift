import MarkdownCoreC

/// One owned relation of a record: a run of its children in canonical order,
/// and the name of the group line the dump draws for it, if any.
///
/// The extent of a relation's first node is relative to the owner's start, and
/// each later node's to the end of the node before it.
struct Relation {
    let name: String?
    let indices: Range<Int>
}

/// The one record behind every Markup value: an immutable node that holds its
/// scalars and references to its children's records, alive while ARC keeps it.
///
/// Every kind's record is a final subclass that adds the kind's own scalars.
/// All of a node's owned relations live here, in one array in canonical walk
/// order, in storage only this class can empty; a kind describes how its
/// relations partition it (``relation(at:)``).
///
/// NO OPERATION RECURSES OVER TREE EDGES. Release, equality, the walker,
/// conversion, scope queries and the dump each keep an explicit work stack,
/// so the call stack stays constant however deep the document is.
///
/// SENDABLE, UNCHECKED, BECAUSE OF ONE WRITE. The only mutation of a record is
/// `deinit` moving children out of a record that nothing else references, and
/// nothing else writes `children`. Every observer therefore sees an immutable
/// value, and records may be shared across isolation domains. Swift requires
/// each subclass to restate the conformance; this invariant is what they
/// restate, and a subclass adds only `let` scalars.
class MarkupRecord: @unchecked Sendable, Hashable {
    let id: MarkupID
    let extent: Extent
    let runs: [Run]
    let anchor: String?
    let attributes: Attributes
    /// Every owned child, in canonical walk order across the node's relations.
    private(set) final var children: [MarkupRecord]

    init(_ fields: InheritedFields, children: [MarkupRecord]) {
        id = fields.id
        extent = fields.extent
        runs = fields.runs
        anchor = fields.anchor
        attributes = fields.attributes
        self.children = children
    }

    /// Releases the subtree with a constant call stack. Each child taken out
    /// that nothing else references gives up its own children first, so its
    /// `deinit` has nothing left to release; a child a view still holds is
    /// only released, which ends at a count decrement.
    deinit {
        var queue: [MarkupRecord] = []
        swap(&queue, &children)
        while var child = queue.popLast() {
            if isKnownUniquelyReferenced(&child) {
                queue.append(contentsOf: child.children)
                child.children = []
            }
        }
    }

    /// The public value this record backs.
    var markup: any Markup {
        preconditionFailure("every record kind projects its own Markup value")
    }

    /// Whether the kind's own scalars equal `other`'s, a record of the same
    /// kind. The universal fields and the children are compared by `==`.
    func hasEqualFields(_ other: MarkupRecord) -> Bool {
        true
    }

    /// The relation at `step` in canonical order, or `nil` after the last. An
    /// optional relation that is absent is empty and unnamed.
    func relation(at step: Int) -> Relation? {
        step == 0 ? Relation(name: nil, indices: children.indices) : nil
    }

    /// Deep value equality, with an explicit work stack.
    static func == (lhs: MarkupRecord, rhs: MarkupRecord) -> Bool {
        var stack = [(lhs, rhs)]
        while let pair = stack.popLast() {
            let (one, other) = pair
            if one === other { continue }
            guard ObjectIdentifier(type(of: one)) == ObjectIdentifier(type(of: other)),
                one.id == other.id,
                one.extent == other.extent,
                one.runs == other.runs,
                one.anchor == other.anchor,
                one.attributes == other.attributes,
                one.children.count == other.children.count,
                one.hasEqualFields(other)
            else { return false }
            for index in one.children.indices {
                stack.append((one.children[index], other.children[index]))
            }
        }
        return true
    }

    /// Reads only the identifier, which equal records share.
    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
}

/// A Markup value that is a view of one record. Every kind conforms.
protocol RecordBacked {
    var base: MarkupRecord { get }
}

extension MarkupRecord {
    /// The record behind any node of the closed kind set.
    static func of(_ node: any Markup) -> MarkupRecord {
        // swift-format-ignore: NeverForceUnwrap
        // swiftlint:disable:next force_cast
        (node as! any RecordBacked).base
    }
}

/// The fields every kind inherits, as one record is built.
struct InheritedFields {
    let id: MarkupID
    let extent: Extent
    let runs: [Run]
    let anchor: String?
    let attributes: Attributes
}

extension InheritedFields {
    /// The fields as the native node states them.
    init(from node: OpaquePointer) {
        var runCount = 0
        let runs = markdown_core_node_runs(node, &runCount)
        self.init(
            id: MarkupID(markdown_core_node_id(node)),
            extent: Extent(markdown_core_node_extent(node)),
            runs: UnsafeBufferPointer(start: runs, count: runCount).map { Run($0) },
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}

extension Extent {
    init(_ extent: markdown_core_extent) {
        self.init(lead: extent.lead, span: extent.span)
    }
}

extension Run {
    init(_ run: markdown_core_run) {
        self.init(lead: run.lead, span: run.span, length: run.length)
    }
}
