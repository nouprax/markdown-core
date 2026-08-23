import MarkdownCoreC

/// What a region's bytes are to their owner.
public enum RegionRole: Int32, Sendable, Hashable {
    /// Syntax the owner is made of: a fence, a bullet, a heading's `#`s.
    case marker = 0
    /// Bytes the owner's meaning is made of.
    case content = 1
    /// Bytes the owner consumed and kept nothing of.
    case discarded = 2
}

/// One region of the concrete view: a byte range of the normalized source with
/// exactly one owner and exactly one role.
///
/// `start` and `length` index ``Concrete/source``, which is bytes and not a
/// `String`: the parser counts positions in bytes, and a `String` index would
/// disagree with it on the first character outside ASCII.
public struct Region: Sendable, Hashable {
    public let start: Int
    public let length: Int
    public let role: RegionRole
    /// The owner, as the path of child indices from the semantic root: `[]` is
    /// the root and `[0, 2]` is the third child of the first.
    ///
    /// A pointer names a node only while the native handle is alive, and this
    /// value type outlives it, so the path is the locator rather than the node.
    public let owner: [Int32]

    public init(start: Int, length: Int, role: RegionRole, owner: [Int32]) {
        self.start = start
        self.length = length
        self.role = role
        self.owner = owner
    }
}

/// The concrete view of a parse: the normalized source, its line index, and
/// every region of it.
///
/// Total, and that is the point of the pair: every byte of ``source`` lies in
/// exactly one region and every region has exactly one owner in the semantic
/// tree, so nothing the parser read is reachable through neither view.
///
/// The regions are stored as parallel arrays and a ``Region`` is built when it
/// is asked for. Measured on this repository's own design document -- one
/// region per 17 bytes of prose -- a stored `Region` per region costs several
/// times the source it describes, and the arrays cost about 25 bytes each.
public struct Concrete: Sendable, Hashable {
    /// The NORMALIZED source: UTF-8 validated, NUL replaced, every line ending
    /// a single `\n`. Not the bytes the caller passed in.
    public let source: [UInt8]

    private let lineStarts: [Int32]
    private let regionStarts: [Int32]
    private let regionLengths: [Int32]
    private let regionRoles: [UInt8]
    private let ownerPaths: [Int32]
    private let ownerOffsets: [UInt32]

    init(
        source: [UInt8],
        lineStarts: [Int32],
        regionStarts: [Int32],
        regionLengths: [Int32],
        regionRoles: [UInt8],
        ownerPaths: [Int32],
        ownerOffsets: [UInt32]
    ) {
        self.source = source
        self.lineStarts = lineStarts
        self.regionStarts = regionStarts
        self.regionLengths = regionLengths
        self.regionRoles = regionRoles
        self.ownerPaths = ownerPaths
        self.ownerOffsets = ownerOffsets
    }

    /// How many lines the normalized source has.
    public var lineCount: Int { lineStarts.count }

    /// Where `line` begins in ``source``, counting lines from 1, or `nil` when
    /// there is no such line.
    public func lineStart(_ line: Int) -> Int? {
        guard line >= 1, line <= lineStarts.count else { return nil }
        return Int(lineStarts[line - 1])
    }

    /// How many regions the view has. They are in source order.
    public var regionCount: Int { regionStarts.count }

    /// The region at `index`, counting from 0, or `nil` when there is none.
    public func region(at index: Int) -> Region? {
        guard index >= 0, index < regionStarts.count else { return nil }
        let pathFrom = Int(ownerOffsets[index])
        let pathTo = Int(ownerOffsets[index + 1])
        return Region(
            start: Int(regionStarts[index]),
            length: Int(regionLengths[index]),
            role: RegionRole(rawValue: Int32(regionRoles[index])) ?? .content,
            owner: Array(ownerPaths[pathFrom..<pathTo])
        )
    }
}

/// One parse under two total views.
///
/// ``semantic`` is the tree with policy applied, which may omit bytes -- a
/// fence, a bullet and a reference definition's punctuation are in no literal
/// anywhere. ``concrete`` omits nothing. Every byte of the source is in exactly
/// one region of the concrete view and every region has exactly one owner in
/// the semantic one, so the pair is complete.
public struct Document: Sendable {
    public let semantic: DocumentRoot
    public let concrete: Concrete

    public static func parse(_ source: String, options: ParseOptions = .init()) throws -> Document {
        var nativeOptions = markdown_core_parse_options(
            smart_punctuation: options.smartPunctuation,
            footnotes: options.footnotes,
            strip_html_comments: options.stripHTMLComments,
            tables: options.tables,
            strikethrough: options.strikethrough,
            autolinks: options.autolinks,
            task_lists: options.taskLists,
            formulas: options.formulas,
            directives: options.directives
        )
        var nativeError: OpaquePointer?
        let bytes = Array(source.utf8)
        let nativeDocument = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_document_parse(buffer.baseAddress, buffer.count, &nativeOptions, &nativeError)
        }
        guard let nativeDocument else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }

        guard let root = markdown_core_document_semantic(nativeDocument),
            let semantic = markup(from: root) as? DocumentRoot,
            let concrete = Concrete(from: nativeDocument)
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree", scope: nil)
        }
        return Document(semantic: semantic, concrete: concrete)
    }
}

extension Document {
    /// The node a region's ``Region/owner`` path names, or `nil` when the path
    /// names no node in this tree.
    ///
    /// The path counts children the way the C tree holds them, and the value
    /// tree splits some of those runs into named fields -- a directive's label
    /// and its content, a table's header and its rows -- so descending it is
    /// not `content[i]` at every step. This is the descent.
    public func owner(of region: Region) -> (any Markup)? {
        var node: any Markup = semantic
        for step in region.owner {
            var visitor = ChildrenVisitor()
            let children = node.accept(&visitor)
            guard step >= 0, Int(step) < children.count else { return nil }
            node = children[Int(step)]
        }
        return node
    }
}

extension Concrete {
    /// Copies the whole view out of the native handle, which the caller frees
    /// as soon as this returns.
    init?(from document: OpaquePointer) {
        let text = markdown_core_document_source(document)
        guard let data = text.data else { return nil }
        let lineCount = markdown_core_document_line_count(document)
        let regionCount = markdown_core_document_region_count(document)

        var lineStarts = [Int32]()
        lineStarts.reserveCapacity(lineCount)
        var line = 1
        while line <= lineCount {
            var offset = 0
            guard markdown_core_document_line_start(document, line, &offset) else { return nil }
            lineStarts.append(Int32(offset))
            line += 1
        }

        var starts = [Int32](repeating: 0, count: regionCount)
        var lengths = [Int32](repeating: 0, count: regionCount)
        var roles = [UInt8](repeating: 0, count: regionCount)
        for index in 0..<regionCount {
            var region = markdown_core_region()
            guard markdown_core_document_region_at(document, index, &region) else { return nil }
            starts[index] = Int32(region.start)
            lengths[index] = Int32(region.length)
            roles[index] = UInt8(region.role.rawValue)
        }

        // Sizing first: the same call fills the offsets it refuses to write
        // paths for, so `offsets[regionCount]` is how many the paths need.
        var offsets = [UInt32](repeating: 0, count: regionCount + 1)
        _ = markdown_core_document_region_owner_paths(document, nil, 0, &offsets, offsets.count)
        var paths = [Int32](repeating: 0, count: Int(offsets[regionCount]))
        let filled = paths.withUnsafeMutableBufferPointer { buffer in
            markdown_core_document_region_owner_paths(
                document, buffer.baseAddress, buffer.count, &offsets, offsets.count
            )
        }
        guard filled else { return nil }

        self.init(
            source: Array(UnsafeBufferPointer(start: data, count: text.length)),
            lineStarts: lineStarts,
            regionStarts: starts,
            regionLengths: lengths,
            regionRoles: roles,
            ownerPaths: paths,
            ownerOffsets: offsets
        )
    }
}
