import MarkdownCoreC

/// A text and the document parsed from it, extended together by each append.
///
/// Each document a session returns continues the previous one: a node that
/// continues an old node keeps its ``Markup/id``, and a node the append left
/// unchanged equals its predecessor. The document is a value built whole after
/// each step; it borrows nothing from the session, so it is safe to hold, copy
/// and send across isolation boundaries.
///
/// Until ``finalize()``, each document is a snapshot of the text so far: a
/// lookup of a reference, footnote or specimen sees only the definitions that
/// start before the inline root asking.
///
/// The caller ends the text with ``finalize()``, which returns the final
/// document; nothing ends a session implicitly. Afterwards ``document`` and
/// ``text`` stay readable, and ``append(_:)`` and ``finalize()`` throw
/// ``ErrorCode/sessionFinalized``.
///
/// A session has one writer. It is not `Sendable`.
public final class MarkdownSession {
    private let session: OpaquePointer

    /// How the scope queries of the session's documents count columns.
    public let unit: TextUnit
    /// The document parsed from the current ``text``.
    public private(set) var document: Document

    /// Parses `source` into a new session, as its first append; its
    /// ``document`` is a snapshot.
    ///
    /// - Parameters:
    ///   - source: the Markdown to start from. It is read as UTF-8.
    ///   - unit: how the session's documents count columns.
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

    /// Appends `text` and parses the result.
    ///
    /// - Parameter text: the Markdown to append. It is read as UTF-8.
    /// - Returns: the new ``document``, a snapshot.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/allocationFailed``
    ///   when an allocation fails or the text would exceed 1 GiB of UTF-8,
    ///   and with ``ErrorCode/sessionFinalized`` after ``finalize()``.
    @discardableResult
    public func append(_ text: String) throws -> Document {
        var native: OpaquePointer?
        var appended = text
        let status = appended.withUTF8 { bytes in
            markdown_core_session_append(session, bytes.baseAddress, bytes.count, &native)
        }
        return try publish(status, native)
    }

    /// Ends the text and returns the final document, which equals
    /// ``Document/parse(_:unit:)`` of ``text`` in everything but ids.
    ///
    /// - Returns: the final ``document``.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/allocationFailed``
    ///   when an allocation fails, and with ``ErrorCode/sessionFinalized``
    ///   when the session was already finalized.
    @discardableResult
    public func finalize() throws -> Document {
        var native: OpaquePointer?
        let status = markdown_core_session_finalize(session, &native)
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
