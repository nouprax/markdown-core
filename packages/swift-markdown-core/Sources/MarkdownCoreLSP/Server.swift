import MarkdownCore
import MarkdownCoreLSPC

/// The language engine (docs/plans/2026-10-23-lsp-completion-engine.md,
/// section 4): the semantics of the Language Server Protocol 3.17 over
/// Markdown Core, called in process. Each method is one protocol method, and
/// answers as the C engine `markdown-core-lsp` does, so this server, the
/// other bindings and the JSON-RPC adapter `markdown-core-server` give the
/// same answers.
///
/// A server holds the open documents of one client. It does no I/O: the host
/// hands it every text. It has one writer and is not `Sendable`; the values
/// it returns are.
///
/// A method that fails throws ``ServerError``, whose code is the protocol's
/// error code, and changes nothing, except an ``update(_:version:edits:)``
/// that fails with ``ServerError/Code/internalError``, which closes the
/// document.
public final class Server {
    let server: OpaquePointer

    /// A new server, in the ``Phase/fresh`` phase.
    ///
    /// - Throws: ``ServerError`` with ``ServerError/Code/internalError`` when
    ///   it cannot be allocated.
    public init() throws {
        var server: OpaquePointer?
        let status = markdown_core_lsp_server_new(&server)
        guard status == MARKDOWN_CORE_LSP_OK, let server else { throw ServerError(status) }
        self.server = server
    }

    deinit {
        markdown_core_lsp_server_free(server)
    }

    /// Where the server is in the protocol's lifecycle. A host that reads
    /// `exit` ends with code 0 when it is ``Phase/halted``, and 1 otherwise.
    public var phase: Phase {
        switch markdown_core_lsp_server_phase(server) {
        case MARKDOWN_CORE_LSP_FRESH: .fresh
        case MARKDOWN_CORE_LSP_ACTIVE: .active
        default: .halted
        }
    }

    /// The initialize request. `units` are the position encodings the client
    /// offers, which the protocol's `general.positionEncodings` lists. The
    /// server becomes ``Phase/active``.
    ///
    /// - Returns: the position encoding, UTF-8 when the client offers it and
    ///   UTF-16 otherwise, and the completion trigger characters.
    /// - Throws: ``ServerError`` with ``ServerError/Code/invalidRequest``
    ///   after the first initialize.
    public func initialize(_ units: [TextUnit]) throws -> Capabilities {
        let native = units.map { $0 == .utf8 ? MARKDOWN_CORE_TEXT_UNIT_UTF8 : MARKDOWN_CORE_TEXT_UNIT_UTF16 }
        var capabilities = markdown_core_lsp_capabilities()
        let status = native.withUnsafeBufferPointer { buffer in
            markdown_core_lsp_initialize(server, buffer.baseAddress, buffer.count, &capabilities)
        }
        guard status == MARKDOWN_CORE_LSP_OK else { throw ServerError(status) }
        let cues = UnsafeBufferPointer(start: capabilities.cues.data, count: capabilities.cues.length)
        return Capabilities(
            unit: capabilities.unit == MARKDOWN_CORE_TEXT_UNIT_UTF8 ? .utf8 : .utf16,
            cues: String(decoding: cues, as: UTF8.self)
        )
    }

    /// The shutdown request. The server becomes ``Phase/halted`` and takes no
    /// more calls.
    public func shutdown() throws {
        try check(markdown_core_lsp_shutdown(server))
    }

    /// The didOpen notification: the document at `uri` holds `text` at
    /// `version`. A document opened again replaces the one open at its URI.
    /// The server parses the text into the document's session, which each
    /// later change edits.
    public func open(_ uri: String, version: Int32, text: String) throws {
        var uri = uri
        var text = text
        let status = uri.withUTF8 { name in
            text.withUTF8 { bytes in
                markdown_core_lsp_open(server, name.baseAddress, name.count, version, bytes.baseAddress, bytes.count)
            }
        }
        try check(status)
    }

    /// The didChange notification: `edits`, the content changes, applied to
    /// the document in order, each to the text the one before it made, which
    /// give it `version`.
    ///
    /// - Throws: ``ServerError`` with ``ServerError/Code/invalidParams`` when
    ///   the document is not open or an edit's start is after its end,
    ///   which every edit is checked for before the first applies; the
    ///   document then keeps its text and version.
    ///   ``ServerError/Code/internalError`` when an edit cannot allocate,
    ///   and then the server closes the document, since it holds some of the
    ///   edits and cannot tell the client's text.
    public func update(_ uri: String, version: Int32, edits: [Edit]) throws {
        // Every edit's text stays alive in one buffer while the engine reads it.
        var joined = [UInt8]()
        var starts = [Int]()
        for edit in edits {
            starts.append(joined.count)
            joined.append(contentsOf: edit.text.utf8)
        }
        var uri = uri
        let status = uri.withUTF8 { name in
            joined.withUnsafeBufferPointer { buffer in
                let native = edits.indices.map { index -> markdown_core_lsp_edit in
                    let end = index + 1 < starts.count ? starts[index + 1] : joined.count
                    let scope = edits[index].scope ?? Scope.whole
                    return markdown_core_lsp_edit(
                        scope: scope.native,
                        text: buffer.baseAddress.map { $0 + starts[index] },
                        size: end - starts[index]
                    )
                }
                return native.withUnsafeBufferPointer { list in
                    let first = list.baseAddress
                    return markdown_core_lsp_update(server, name.baseAddress, name.count, version, first, list.count)
                }
            }
        }
        try check(status)
    }

    /// The didClose notification.
    public func close(_ uri: String) throws {
        var uri = uri
        try check(uri.withUTF8 { name in markdown_core_lsp_close(server, name.baseAddress, name.count) })
    }

    /// The completion request at `position` in the document at `uri`. It
    /// locates the document's session at the cursor and reads the context
    /// there.
    ///
    /// - Returns: the completions, computed for the document's current
    ///   version. A host drops completions whose version is older than the
    ///   document's, as the protocol's `ContentModified` does.
    public func complete(_ uri: String, at position: Position) throws -> Completions {
        var uri = uri
        var list: OpaquePointer?
        let status = uri.withUTF8 { name in
            markdown_core_lsp_complete(server, name.baseAddress, name.count, position.native, &list)
        }
        guard status == MARKDOWN_CORE_LSP_OK, let list else { throw ServerError(status) }
        defer { markdown_core_lsp_list_free(list) }
        return Completions(
            version: markdown_core_lsp_list_version(list),
            count: markdown_core_lsp_list_count(list)
        )
    }

    private func check(_ status: markdown_core_lsp_status) throws {
        guard status == MARKDOWN_CORE_LSP_OK else { throw ServerError(status) }
    }
}

/// Where a ``Server`` is in the protocol's lifecycle: fresh until initialize,
/// active until shutdown, and halted afterwards.
public enum Phase: Sendable, Hashable {
    case fresh
    case active
    case halted
}

/// What a ``Server`` answers initialize with.
public struct Capabilities: Sendable, Hashable {
    /// The position encoding every ``Position`` counts in.
    public let unit: TextUnit
    /// The completion trigger characters, one per character: the parser's
    /// slot cues, which are ASCII.
    public let cues: String
}

/// A protocol position: `line` and `character` counted from 0, in the
/// negotiated ``Capabilities/unit``. A character past its line's end stands
/// at the line's end, one inside a character stands at the character's
/// start, and a line past the text's last stands at the text's end.
public struct Position: Sendable, Hashable {
    /// The line, counted from 0.
    public var line: UInt32
    /// The character in the line, counted from 0 in the negotiated unit.
    public var character: UInt32

    /// The position at `character` of `line`.
    public init(line: UInt32, character: UInt32) {
        self.line = line
        self.character = character
    }

    var native: markdown_core_lsp_position {
        markdown_core_lsp_position(line: line, character: character)
    }
}

/// A protocol range: from `start` up to `end`, which it does not hold.
public struct Scope: Sendable, Hashable {
    /// Where the range starts, which it holds.
    public var start: Position
    /// Where the range ends, which it does not hold.
    public var end: Position

    /// The range from `start` up to `end`.
    public init(start: Position, end: Position) {
        self.start = start
        self.end = end
    }

    /// The whole text, which an edit that names no range replaces.
    static let whole = Scope(
        start: Position(line: 0, character: 0),
        end: Position(line: .max, character: .max)
    )

    var native: markdown_core_lsp_scope {
        markdown_core_lsp_scope(start: start.native, end: end.native)
    }
}

/// A content change of didChange: `text` replaces `scope`, or the whole text
/// when `scope` is nil.
public struct Edit: Sendable, Hashable {
    /// The range the edit replaces, or `nil` for the whole text.
    public var scope: Scope?
    /// The text that replaces the range.
    public var text: String

    /// The edit that writes `text` over `scope`, or over the whole text when it is `nil`.
    public init(scope: Scope?, text: String) {
        self.scope = scope
        self.text = text
    }
}

/// The answer to one completion request: the version of the document it was
/// computed for, and the number of its items. The items come with the
/// providers of the plan's section 5.
public struct Completions: Sendable, Hashable {
    /// The version of the document the completions were computed for.
    public let version: Int32
    /// The number of completion items.
    public let count: Int
}

/// Why a ``Server`` method failed.
public struct ServerError: Error, Sendable, Hashable {
    /// The protocol's error code for the one reason there is no answer.
    public enum Code: Int32, Sendable, Hashable {
        /// The call came before initialize.
        case notInitialized = -32002
        /// The call is not one the server's phase takes: a second
        /// initialize, or any call after shutdown.
        case invalidRequest = -32600
        /// The call names a document that is not open, or an edit whose
        /// start is after its end.
        case invalidParams = -32602
        /// An allocation failed, or a text exceeds what the parser can hold.
        case internalError = -32603
    }

    /// The protocol's error code.
    public let code: Code

    /// The error of `code`.
    public init(code: Code) {
        self.code = code
    }

    init(_ status: markdown_core_lsp_status) {
        switch status {
        case MARKDOWN_CORE_LSP_SERVER_NOT_INITIALIZED: self.init(code: .notInitialized)
        case MARKDOWN_CORE_LSP_INVALID_REQUEST: self.init(code: .invalidRequest)
        case MARKDOWN_CORE_LSP_INVALID_PARAMS: self.init(code: .invalidParams)
        // A C enum switch is never exhaustive in Swift; the one failure left
        // is MARKDOWN_CORE_LSP_INTERNAL_ERROR.
        default: self.init(code: .internalError)
        }
    }
}
