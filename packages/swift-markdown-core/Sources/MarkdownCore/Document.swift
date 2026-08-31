import MarkdownCoreC

/// Which constructs a parse recognises.
///
/// Every switch is ATTACHMENT and nothing finer: an extension is on or it is
/// not, and there is no second knob that changes what an attached extension
/// means. 1.0.3 had `dollarFormulaDelimiters` and `latexFormulaDelimiters`
/// beside ``formulas``; they are gone, because an option that changes a
/// grammar rather than enabling one is a second parser hiding in the first.
///
/// Every default is `true`.
public struct ParseOptions: Sendable, Hashable {
    /// Turn straight quotes, `--` and `...` into their typographic forms.
    public let smartPunctuation: Bool
    /// Recognise `[^label]` calls and `[^label]:` definitions.
    public let footnotes: Bool
    /// Drop HTML comments from the tree instead of keeping them as ``HTML``.
    public let stripHTMLComments: Bool
    /// Recognise GFM tables.
    public let tables: Bool
    /// Recognise `~~struck~~`.
    public let strikethrough: Bool
    /// Recognise bare URLs and `www.` prefixes as links.
    public let autolinks: Bool
    /// Recognise `- [ ]` and `- [x]` list items, which gives
    /// ``ListItem/checked`` a value other than `nil`.
    public let taskLists: Bool
    /// Recognise formulas — five inline forms and four block forms.
    public let formulas: Bool
    /// Recognise directives — `:name`, `::name` and `:::name` fences.
    public let directives: Bool

    /// Creates an option set. Every parameter defaults to `true`, so
    /// `ParseOptions()` recognises everything this parser knows.
    public init(
        smartPunctuation: Bool = true,
        footnotes: Bool = true,
        stripHTMLComments: Bool = true,
        tables: Bool = true,
        strikethrough: Bool = true,
        autolinks: Bool = true,
        taskLists: Bool = true,
        formulas: Bool = true,
        directives: Bool = true
    ) {
        self.smartPunctuation = smartPunctuation
        self.footnotes = footnotes
        self.stripHTMLComments = stripHTMLComments
        self.tables = tables
        self.strikethrough = strikethrough
        self.autolinks = autolinks
        self.taskLists = taskLists
        self.formulas = formulas
        self.directives = directives
    }
}

/// Why a parse produced no document.
///
/// These are FAILURES, not findings about the text. Anything the author could
/// act on is a diagnostic on a document that exists, not an error instead of
/// one.
public enum ParseErrorCode: Int32, Sendable {
    /// The call itself was wrong — a null source, or a length that does not
    /// describe it.
    case invalidArgument = 1
    /// An allocation failed. The parse is abandoned rather than returning a
    /// document with something missing from it.
    case allocationFailed = 2
    /// The parser reached a state it does not otherwise account for.
    case `internal` = 3
}

/// A parse failure, and nothing else.
///
/// It carries no scope: an input the parser could not turn into a document has
/// no extent to point at, and a failure the author could act on would have been
/// a diagnostic instead.
public struct ParseError: Error, Sendable {
    /// Which failure it was.
    public let code: ParseErrorCode
    /// A fixed English sentence naming the failure. It is for a log, not for
    /// an end user, and it is not localised.
    public let message: String
}

/// The living document: the one entry into this parser, fed in pieces and
/// answering with ``Read`` values (docs/STREAMING.md §4 D5, under 3.0's
/// names).
///
/// `feed(chunk:)` hands it the next bytes and returns THE READ AFTER
/// THOSE BYTES — a mid-stream projection: a trailing line whose ending has not
/// arrived is not yet in it, and an open construct is projected as it stands
/// (a list still open has not settled its tightness). ``seal()`` ends the
/// stream: the pending line is processed, every construct closes, and the
/// sealed read comes back — the whole text's, identical for the same bytes
/// however they were fed — and the native shell goes with it: a sealed
/// document refuses every later call.
///
/// Every read either call returns is built the same way: the native handle is
/// released before the call returns, so the result is a value that borrows
/// nothing. It stays readable after every later feed, after ``seal()``, and
/// after the document itself is gone.
///
/// The document itself is a native resource mid-parse, not a value: it is
/// deliberately not `Sendable`, and feeding one document from two isolation
/// domains is the caller's race. The reads it returns are `Sendable` like
/// every other value this module hands out. A stream abandoned instead of
/// sealed needs no explicit call: `deinit` frees the shell.
public final class Document {
    // The native session, private the way every native anything is: no
    // handle is part of the public surface. `nil` once sealed, which is the
    // same "gone" `deinit` leaves behind.
    private var native: OpaquePointer?

    /// Opens a document.
    ///
    /// - Parameter options: which constructs to recognise. Everything, by
    ///   default.
    /// - Throws: ``ParseError`` when the native session cannot be created,
    ///   which is an allocation failure and nothing finer.
    public init(options: ParseOptions = .init()) throws {
        var nativeOptions = options.native
        var nativeError: OpaquePointer?
        let session = markdown_core_session_new(&nativeOptions, &nativeError)
        guard let session else { throw ParseError.take(nativeError) }
        native = session
    }

    /// Opens a document and feeds it `markdown` in one step — exactly
    /// ``init(options:)`` followed by one feed whose returned read is
    /// discarded (and, being discarded, never copied out), so the whole-text
    /// parse is `Document(markdown: text).seal()`.
    ///
    /// - Parameters:
    ///   - markdown: the first piece of the stream, or the whole text.
    ///   - options: which constructs to recognise. Everything, by default.
    /// - Throws: ``ParseError``, exactly as ``init(options:)`` and
    ///   `feed(chunk:)` throw it.
    public convenience init(markdown: String, options: ParseOptions = .init()) throws {
        try self.init(options: options)
        var nativeError: OpaquePointer?
        let bytes = Array(markdown.utf8)
        // The read this feed would produce is discarded by this very
        // contract, so the bytes go through `advance`, which takes them and
        // answers nothing — `session_feed` derived, copied, and returned a
        // whole document just to be freed unread (#144). ES and Kotlin
        // construct through their advance entries the same way, and the C
        // header names this constructor as advance's one legitimate caller.
        let advanced = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_session_advance(native, buffer.baseAddress, buffer.count, &nativeError)
        }
        guard advanced else { throw ParseError.take(nativeError) }
    }

    deinit {
        if let native { markdown_core_session_free(native) }
    }

    /// Feeds exactly the bytes of `chunk` and returns the read after them.
    ///
    /// Bytes, not a `String`, because a chunk boundary owes the text nothing:
    /// it may fall inside a UTF-8 sequence or between a CR and its LF, and the
    /// stream repairs both — a split no `String` could even spell. An empty
    /// chunk is a legal feed: the read as it stands.
    ///
    /// - Parameter chunk: the next bytes of the stream, read as UTF-8.
    /// - Returns: the read after those bytes, as a value the caller keeps.
    ///   An incomplete trailing line is not yet in it.
    /// - Throws: ``ParseError`` — `.invalidArgument` once ``seal()`` has
    ///   ended the stream, `.allocationFailed` when the projection could not
    ///   be built. Text is never a failure: it produces a read.
    public func feed(chunk: [UInt8]) throws -> Read {
        let session = try live()
        var nativeError: OpaquePointer?
        let nativeDocument = chunk.withUnsafeBufferPointer { buffer in
            markdown_core_session_feed(session, buffer.baseAddress, buffer.count, &nativeError)
        }
        guard let nativeDocument else {
            throw ParseError.take(nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }
        return try Read(copiedFrom: nativeDocument)
    }

    /// Feeds a chunk that is whole text — its UTF-8 bytes, exactly as the byte
    /// form takes them. A producer of `String` pieces never splits a scalar,
    /// so the byte form's one extra power is not needed here; everything else
    /// is the same call.
    ///
    /// - Parameter chunk: the next piece of the stream.
    /// - Returns: the read after those bytes, as a value the caller keeps.
    /// - Throws: ``ParseError``, exactly as the byte form throws it.
    public func feed(chunk: String) throws -> Read {
        try feed(chunk: Array(chunk.utf8))
    }

    /// Ends the stream, returns the sealed read, and releases the native
    /// shell.
    ///
    /// The pending line is processed and every construct closes, so the
    /// result is identical for the same bytes however they were fed. Sealing
    /// is the end of the object: after it returns, `feed(chunk:)` and
    /// a second ``seal()`` throw `.invalidArgument`.
    ///
    /// - Returns: the sealed read.
    /// - Throws: ``ParseError`` — `.invalidArgument` for a document already
    ///   sealed, `.allocationFailed` when the final projection could not be
    ///   built (the shell then remains, and `deinit` still frees it).
    public func seal() throws -> Read {
        let session = try live()
        var nativeError: OpaquePointer?
        guard let nativeDocument = markdown_core_session_finish(session, &nativeError) else {
            throw ParseError.take(nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }
        let sealed = try Read(copiedFrom: nativeDocument)
        markdown_core_session_free(session)
        native = nil
        return sealed
    }

    private func live() throws -> OpaquePointer {
        guard let native else {
            throw ParseError(code: .invalidArgument, message: "the document is sealed")
        }
        return native
    }
}
