import MarkdownCoreC

/// The text a ``Markup/scope``'s line and column numbers are counted against.
///
/// A scope is a pair of BOUNDARIES — it says which line-and-column range an
/// element occupies, and no substring is taken with it. Those numbers are not
/// counted against the string you fed the ``Document``: they are counted
/// against the NORMALIZED source, which is what this carries,
/// and the two differ wherever the input held a NUL.
public struct Concrete: Sendable, Hashable {
    /// The NORMALIZED source: UTF-8 as fed, every NUL replaced by the three
    /// bytes of U+FFFD, every line ending a single `\n` and every line having
    /// one. Not the bytes the caller passed in.
    ///
    /// Bytes and not a `String`: the parser counts columns in bytes, and a
    /// `String` index would disagree with it on the first character outside
    /// ASCII.
    public let source: [UInt8]

    private let lineStarts: [Int32]

    init(source: [UInt8], lineStarts: [Int32]) {
        self.source = source
        self.lineStarts = lineStarts
    }

    /// How many lines the normalized source has.
    public var lines: Int { lineStarts.count }

    /// The byte offset in ``source`` where `line` begins, counting lines from
    /// 1, or `nil` when there is no such line. An OFFSET, not a boundary:
    /// this indexes bytes, which a ``Scope`` never does.
    public func offset(of line: Int) -> Int? {
        guard line >= 1, line <= lineStarts.count else { return nil }
        return Int(lineStarts[line - 1])
    }
}

extension Concrete {
    /// Copies the source and its index out of the native handle, which the
    /// caller frees as soon as this returns.
    init?(from document: OpaquePointer) {
        let text = markdown_core_document_source(document)
        guard let data = text.data else { return nil }
        let lineCount = markdown_core_document_line_count(document)

        var lineStarts = [Int32]()
        lineStarts.reserveCapacity(lineCount)
        var line = 1
        while line <= lineCount {
            var offset = 0
            guard markdown_core_document_line_start(document, line, &offset) else { return nil }
            lineStarts.append(Int32(offset))
            line += 1
        }

        self.init(source: Array(UnsafeBufferPointer(start: data, count: text.length)), lineStarts: lineStarts)
    }
}
