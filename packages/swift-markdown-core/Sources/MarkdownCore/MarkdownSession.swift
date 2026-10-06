import MarkdownCoreC

/// A text and the document parsed from it, changed together by each edit.
///
/// Each document a session returns continues the previous one: a node that
/// continues an old node keeps its ``Markup/id``, and a node the edit left
/// unchanged equals its predecessor. The document is a value built whole after
/// each step; it borrows nothing from the session, so it is safe to hold, copy
/// and send across isolation boundaries.
///
/// A session has one writer. It is not `Sendable`.
public final class MarkdownSession {
    private let session: OpaquePointer

    /// How the session counts: the offsets an edit passes in, and the columns
    /// the scope queries of its documents return.
    public let unit: TextUnit
    /// The document parsed from the current ``text``.
    public private(set) var document: Document

    /// Parses `source` into a new session.
    ///
    /// - Parameters:
    ///   - source: the Markdown to start from. It is read as UTF-8.
    ///   - unit: how the session counts offsets and columns.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/allocationFailed``
    ///   when an allocation fails or `source` exceeds 1 GiB of UTF-8.
    public init(_ source: String = "", unit: TextUnit = .utf16) throws {
        var session: OpaquePointer?
        var text = source
        let status = text.withUTF8 { bytes in
            markdown_core_session_new(bytes.baseAddress, bytes.count, unit.native, &session)
        }
        guard status == MARKDOWN_CORE_OK, let session else { throw MarkdownCoreError(status) }
        self.session = session
        self.unit = unit
        document = Document(native: markdown_core_session_document(session), unit: unit)
    }

    deinit {
        markdown_core_session_free(session)
    }

    /// The session's current text, read from the engine on each call.
    public var text: String {
        let size = markdown_core_session_text_size(session)
        // The bytes are decoded as UTF-8, as `String(decoding:as:)` decodes
        // them: a malformed sequence becomes U+FFFD.
        return String(unsafeUninitializedCapacity: size) { bytes in
            markdown_core_session_text(session, bytes.baseAddress)
            return size
        }
    }

    /// Applies `edits` to the text and parses it once.
    ///
    /// Every range is in the coordinates of the text before the batch. The
    /// edits are disjoint and listed in any order; two at one offset apply in
    /// the order listed. A `Range<String.Index>` counts against ``text`` as it
    /// is when `edit` is called. A single replacement is a batch of one edit.
    ///
    /// - Parameter edits: the replacements.
    /// - Returns: the new ``document``.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when a
    ///   range reaches outside the text or two ranges overlap; with
    ///   ``ErrorCode/insideScalar`` when an offset falls inside a scalar; with
    ///   ``ErrorCode/allocationFailed`` when an allocation fails or the text
    ///   would exceed 1 GiB of UTF-8.
    @discardableResult
    public func edit(_ edits: [TextEdit]) throws -> Document {
        lazy var current = self.text
        // Every replacement's bytes, end to end, so one buffer outlives the
        // single native call.
        var bytes: [UInt8] = []
        var replacements: [(bounds: Range<Int>, text: Range<Int>)] = []
        replacements.reserveCapacity(edits.count)
        for edit in edits {
            let bounds: Range<Int>
            switch edit.location {
            case .offsets(let offsets): bounds = offsets
            case .indices(let indices): bounds = unit.offsets(of: indices, in: current)
            }
            let start = bytes.count
            bytes.append(contentsOf: edit.text.utf8)
            replacements.append((bounds: bounds, text: start..<bytes.count))
        }
        var native: OpaquePointer?
        let status = bytes.withUnsafeBufferPointer { buffer in
            let natives = replacements.map { replacement in
                markdown_core_text_edit(
                    start: replacement.bounds.lowerBound,
                    end: replacement.bounds.upperBound,
                    text: buffer.baseAddress.map { $0 + replacement.text.lowerBound },
                    size: replacement.text.count
                )
            }
            return markdown_core_session_edit(session, natives, natives.count, &native)
        }
        return try publish(status, native)
    }

    /// Appends `text`: the edit at the end of the text.
    ///
    /// - Parameter text: the Markdown to append. It is read as UTF-8.
    /// - Returns: the new ``document``.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/allocationFailed``
    ///   when an allocation fails or the text would exceed 1 GiB of UTF-8.
    @discardableResult
    public func append(_ text: String) throws -> Document {
        var native: OpaquePointer?
        var appended = text
        let status = appended.withUTF8 { bytes in
            markdown_core_session_append(session, bytes.baseAddress, bytes.count, &native)
        }
        return try publish(status, native)
    }

    /// Builds the document a step returned, which borrows from the session
    /// until its next step.
    private func publish(_ status: markdown_core_status, _ native: OpaquePointer?) throws -> Document {
        guard status == MARKDOWN_CORE_OK, let native else { throw MarkdownCoreError(status) }
        document = Document(native: native, unit: unit)
        return document
    }
}

/// One replacement of a session's text: the text in a range becomes `text`.
public struct TextEdit: Sendable {
    /// Where an edit's range is written.
    enum Location: Sendable {
        /// Offsets in the session's ``TextUnit``.
        case offsets(Range<Int>)
        /// Indices of the session's ``MarkdownSession/text``, which carry no
        /// unit.
        case indices(Range<String.Index>)
    }

    let location: Location
    let text: String

    /// Replaces the text in `range`, counted in the session's ``TextUnit``.
    ///
    /// - Parameters:
    ///   - range: offsets into the text before the batch, each on a scalar
    ///     boundary.
    ///   - text: the replacement. It is read as UTF-8.
    public init(_ range: Range<Int>, with text: String) {
        location = .offsets(range)
        self.text = text
    }

    /// Replaces the text in `range`, indices of the session's
    /// ``MarkdownSession/text`` before the batch.
    ///
    /// - Parameters:
    ///   - range: indices on scalar boundaries; the session converts them to
    ///     offsets in its unit.
    ///   - text: the replacement. It is read as UTF-8.
    public init(_ range: Range<String.Index>, with text: String) {
        location = .indices(range)
        self.text = text
    }
}

extension TextUnit {
    /// The offsets of `indices` in `text`, counted in this unit from the start.
    func offsets(of indices: Range<String.Index>, in text: String) -> Range<Int> {
        switch self {
        case .utf8:
            let start = text.utf8.distance(from: text.startIndex, to: indices.lowerBound)
            return start..<start + text.utf8.distance(from: indices.lowerBound, to: indices.upperBound)
        case .utf16:
            let start = text.utf16.distance(from: text.startIndex, to: indices.lowerBound)
            return start..<start + text.utf16.distance(from: indices.lowerBound, to: indices.upperBound)
        }
    }
}
