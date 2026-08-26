import MarkdownCoreC

/// A parse fed in pieces.
///
/// A session owns one native parser for its whole life. `feed(chunk:)` hands
/// it the next bytes and returns THE DOCUMENT AFTER THOSE BYTES — a mid-stream
/// projection: a trailing line whose ending has not arrived is not yet in it,
/// and an open construct is projected as it stands (a list still open has not
/// settled its tightness). ``finish()`` ends the stream: the pending line is
/// processed, every construct closes, and the sealed document comes back —
/// equal, concrete view and all, to what ``Document/parse(_:options:)``
/// returns for the same bytes.
///
/// Every document either call returns is built exactly the way `parse` builds
/// one: the native handle is released before the call returns, so the result
/// is a value that borrows nothing. It stays readable after every later feed,
/// after ``finish()``, and after the session itself is gone.
///
/// The session itself is a native resource mid-parse, not a value: it is
/// deliberately not `Sendable`, and feeding one session from two isolation
/// domains is the caller's race. The documents it returns are `Sendable` like
/// every other ``Markup`` tree.
public final class Session {
    // The native session, private the way `parse`'s handle is scoped to
    // `parse`: no native anything is part of the public surface.
    private let native: OpaquePointer

    /// Opens a session.
    ///
    /// - Parameter options: which constructs to recognise, read exactly as
    ///   ``Document/parse(_:options:)`` reads them. Everything, by default.
    /// - Throws: ``ParseError`` when the native session cannot be created,
    ///   which is an allocation failure and nothing finer.
    public init(options: ParseOptions = .init()) throws {
        var nativeOptions = options.native
        var nativeError: OpaquePointer?
        let session = markdown_core_session_new(&nativeOptions, &nativeError)
        guard let session else { throw ParseError.take(nativeError) }
        native = session
    }

    deinit {
        markdown_core_session_free(native)
    }

    /// Feeds exactly the bytes of `chunk` and returns the document after them.
    ///
    /// Bytes, not a `String`, because a chunk boundary owes the text nothing:
    /// it may fall inside a UTF-8 sequence or between a CR and its LF, and the
    /// stream repairs both — a split no `String` could even spell. An empty
    /// chunk is a legal feed: the document as it stands.
    ///
    /// - Parameter chunk: the next bytes of the stream, read as UTF-8.
    /// - Returns: the document after those bytes, as a value the caller keeps.
    ///   An incomplete trailing line is not yet in it.
    /// - Throws: ``ParseError`` — `.invalidArgument` once ``finish()`` has
    ///   sealed the stream, `.allocationFailed` when the projection could not
    ///   be built. Text is never a failure: it produces a document.
    public func feed(chunk: [UInt8]) throws -> Document {
        var nativeError: OpaquePointer?
        let nativeDocument = chunk.withUnsafeBufferPointer { buffer in
            markdown_core_session_feed(native, buffer.baseAddress, buffer.count, &nativeError)
        }
        guard let nativeDocument else {
            throw ParseError.take(nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }
        return try Document(copiedFrom: nativeDocument)
    }

    /// Feeds a chunk that is whole text — its UTF-8 bytes, exactly as the byte
    /// form takes them. A producer of `String` pieces never splits a scalar,
    /// so the byte form's one extra power is not needed here; everything else
    /// is the same call.
    ///
    /// - Parameter chunk: the next piece of the stream.
    /// - Returns: the document after those bytes, as a value the caller keeps.
    /// - Throws: ``ParseError``, exactly as the byte form throws it.
    public func feed(chunk: String) throws -> Document {
        try feed(chunk: Array(chunk.utf8))
    }

    /// Ends the stream and returns the sealed document.
    ///
    /// The pending line is processed and every construct closes, so the result
    /// equals ``Document/parse(_:options:)`` of the concatenated chunks. It
    /// also ends the session's parse: after it returns, `feed(chunk:)` and a
    /// second `finish()` throw `.invalidArgument`, and letting the session go
    /// is all that remains.
    ///
    /// - Returns: the sealed document.
    /// - Throws: ``ParseError`` — `.invalidArgument` for a session already
    ///   finished, `.allocationFailed` when the final projection could not be
    ///   built.
    public func finish() throws -> Document {
        var nativeError: OpaquePointer?
        guard let nativeDocument = markdown_core_session_finish(native, &nativeError) else {
            throw ParseError.take(nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }
        return try Document(copiedFrom: nativeDocument)
    }
}
