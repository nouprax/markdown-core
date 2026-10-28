import MarkdownCoreC

/// The syntactic context at a cursor: the open state of a parse of the text
/// before it, confirmed against the document parsed from the whole text.
///
/// A session keeps its context as its state at its cursor:
/// ``MarkdownSession/locate(_:)`` moves the cursor and updates
/// ``MarkdownSession/context``, and ``MarkdownSession/spell(_:in:)`` spells
/// from that state as it stands.
///
/// A context lists the blocks the text before the cursor leaves open, the
/// inline constructs it leaves open in the inline content that holds the
/// cursor, and the fields an open construct reads at the cursor, which the
/// whole document confirms.
public struct Context: Sendable {
    /// The blocks the text before the cursor leaves open, outermost first,
    /// each with the scope of its opening line up to the cursor.
    public let blocks: [Opener]
    /// The constructs the text before the cursor leaves open in the inline
    /// content that holds it, earliest first, each with the scope of its
    /// opening syntax.
    public let inlines: [Opener]
    /// The fields the cursor may be writing. There is more than one only
    /// where the grammar itself leaves the text before the cursor ambiguous.
    public let slots: [Slot]

    /// The context of a session not yet located: nothing is open.
    static let blank = Context(blocks: [], inlines: [], slots: [])
}

/// A construct a context finds open.
public struct Opener: Sendable, Hashable {
    /// The kind it opens, or `nil` while that is still open too: a `[` may
    /// open a link, a span or a cite.
    public let kind: NodeKind?
    /// Its opening syntax, or for a block its opening line up to the cursor.
    public let scope: Scope
}

/// A field of kind ``kind`` of a construct of kind ``owner`` that the cursor
/// may be writing.
///
/// A slot names a field of its session's ``MarkdownSession/context`` as the
/// locate that read it left it: it spells only while that context stands.
/// It is not `Equatable`: how it spells depends on the syntax of the text it
/// was read from, such as the `<...>` around a destination, which its fields
/// do not show.
public struct Slot: Sendable {
    /// The field's role.
    public let kind: Kind
    /// The construct the field belongs to.
    public let owner: NodeKind
    /// The field's source from its start to the cursor: what the user has
    /// written of it. It does not leave the cursor's line.
    public let before: Scope
    /// The field's continuation from the cursor, as the construct's own
    /// grammar reads it. It does not leave the cursor's line.
    public let after: Scope
    /// The earlier fields the slot depends on, as written, such as the note
    /// before a cross link's anchor.
    public let prefix: [String]
    /// Whether the whole document finishes the construct after the cursor;
    /// otherwise it reads the construct's opener as text.
    public let closed: Bool
    /// The session whose context holds the slot, by a token the slot keeps
    /// alive so that no later session shares it, the slot's place among the
    /// context's slots, and the session's turn that read it.
    let session: Token
    let index: Int
    let turn: Int

    /// What replaces a slot's ``Slot/before`` and ``Slot/after`` to write a
    /// value in it.
    public struct Insertion: Sendable, Hashable {
        /// The value in the field's own syntax, and the shortest closer that
        /// finishes the construct when it is not ``Slot/closed``.
        public let text: String
        /// Where the caret lands: the columns of ``text`` before it, in the
        /// session's ``TextUnit``. It stands after the value, before any
        /// closer.
        public let cursor: Int
    }

    /// The bytes after which a slot may begin with nothing written yet, in
    /// ascending order: the last byte of a slot's opener, such as the second
    /// `[` of `[[` or the `(` of `](`; a separator before a field, such as
    /// the space before an attribute record; and a line ending, before a
    /// properties member's name. An editor that asks for completions when one
    /// is typed asks wherever a slot opens empty.
    public static let cues: [Character] = {
        let cues = markdown_core_slot_cues()
        return UnsafeBufferPointer(start: cues.data, count: cues.length).map { Character(Unicode.Scalar($0)) }
    }()
}

extension Metadata {
    /// The field names the properties envelope reads, in the order of the
    /// dialect spec. The envelope drops a member with any other name.
    public static let fields: [String] = (0..<markdown_core_metadata_field_count()).map { index in
        answer(markdown_core_string()) { markdown_core_metadata_field_at(index, $0) }.required
    }
}

extension MarkdownSession {
    /// Moves the session's cursor to `position` of ``text``, which stands
    /// between characters: its column may also stand at its line's end or at
    /// the text's end. It counts columns in the session's ``unit``. The
    /// session's ``context`` becomes the context there.
    ///
    /// The query parses the text before the cursor and confirms what it
    /// leaves open against the session's current ``document``, whose native
    /// parse the session holds. A snapshot confirms as it resolves: a label
    /// sees only the definitions before the inline root asking.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   the position names no place of the text or its line or column is
    ///   below 1, and ``ErrorCode/allocationFailed`` when an allocation fails;
    ///   either leaves ``context`` as it was.
    public func locate(_ position: Position) throws {
        guard position.line >= 1, position.column >= 1 else { throw MarkdownCoreError(code: .outOfBounds) }
        let status = markdown_core_session_locate(session, position.native)
        guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
        turn += 1
        context = read()
    }

    /// The parser's spelling of each of `values` in `slot` of ``context``, in
    /// order.
    ///
    /// A spelling writes the value in the field's own syntax: it escapes what
    /// the field reads as syntax, and writes in `<...>`, in braces or in
    /// quotes a value whose bare spelling the field does not read whole. It
    /// is `nil` for a value the field holds in no spelling there, such as a
    /// line ending, a `|` in a cross target or a space in a key.
    ///
    /// The session spells from its context as the last locate left it, so a
    /// spelling parses nothing.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `slot` is not a slot of ``context``: another session's, or one a
    ///   later locate, append or flush replaced the context of, and
    ///   ``ErrorCode/allocationFailed`` when an allocation fails.
    public func spell(_ values: [String], in slot: Slot) throws -> [Slot.Insertion?] {
        guard slot.session === token, slot.turn == turn else {
            throw MarkdownCoreError(code: .outOfBounds)
        }
        let context = markdown_core_session_context(session)
        return try values.map { value -> Slot.Insertion? in
            var text = value
            var made: UnsafeMutablePointer<markdown_core_insertion>?
            let status = text.withUTF8 { bytes in
                markdown_core_slot_spell(context, slot.index, bytes.baseAddress, bytes.count, &made)
            }
            guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
            guard let made else { return nil }
            defer { markdown_core_insertion_free(made) }
            let bytes = UnsafeBufferPointer(start: made.pointee.text, count: made.pointee.size)
            // A spelling is the value's bytes and the field's syntax, so it
            // is UTF-8 whenever the value is.
            // swiftlint:disable:next optional_data_string_conversion
            let spelled = String(decoding: bytes, as: UTF8.self)
            return Slot.Insertion(
                text: spelled,
                cursor: SourceLines.columns(bytes, from: 0, to: made.pointee.cursor, unit: unit)
            )
        }
    }

    /// The parser's spelling of `value` in `slot` of ``context``, as the list
    /// form spells each value.
    public func spell(_ value: String, in slot: Slot) throws -> Slot.Insertion? {
        try spell([value], in: slot)[0]
    }

    /// The session's native context as it stands, read for this turn.
    private func read() -> Context {
        let context = markdown_core_session_context(session)
        let blocks = (0..<markdown_core_context_block_count(context)).map { index in
            Opener(answer(markdown_core_opener()) { markdown_core_context_block_at(context, index, $0) })
        }
        let inlines = (0..<markdown_core_context_inline_count(context)).map { index in
            Opener(answer(markdown_core_opener()) { markdown_core_context_inline_at(context, index, $0) })
        }
        let slots = (0..<markdown_core_context_slot_count(context)).map { index in
            let slot = answer(markdown_core_slot()) { markdown_core_context_slot_at(context, index, $0) }
            let prefix = (0..<slot.prefix_count).map { part in
                answer(markdown_core_string()) { markdown_core_context_slot_prefix_at(context, index, part, $0) }
                    .required
            }
            return Slot(slot, prefix: prefix, session: token, index: index, turn: turn)
        }
        return Context(blocks: blocks, inlines: inlines, slots: slots)
    }
}

/// A session's identity for its slots: unlike the session's address, it is
/// not reused while a slot holds it.
final class Token: Sendable {}

extension Opener {
    init(_ opener: markdown_core_opener) {
        self.init(kind: NodeKind(rawValue: Int32(opener.kind.rawValue)), scope: Scope(opener.scope))
    }
}

extension Slot {
    init(_ slot: markdown_core_slot, prefix: [String], session: Token, index: Int, turn: Int) {
        // A context's slot always has a kind and an owner.
        guard let kind = Kind(rawValue: Int32(slot.kind.rawValue)),
            let owner = NodeKind(rawValue: Int32(slot.owner.rawValue))
        else { preconditionFailure("the context answered a slot without a kind or an owner") }
        self.init(
            kind: kind,
            owner: owner,
            before: Scope(slot.before),
            after: Scope(slot.after),
            prefix: prefix,
            closed: slot.closed,
            session: session,
            index: index,
            turn: turn
        )
    }
}

extension Slot.Kind {
    var native: markdown_core_slot_kind {
        markdown_core_slot_kind(rawValue: UInt32(rawValue))
    }
}

extension Scope {
    init(_ scope: markdown_core_scope) {
        self.init(start: Position(scope.start), end: Position(scope.end))
    }
}

extension Position {
    init(_ position: markdown_core_position) {
        self.init(line: position.line, column: position.column)
    }

    var native: markdown_core_position {
        markdown_core_position(line: line, column: column)
    }
}
