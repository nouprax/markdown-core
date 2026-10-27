package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads

/**
 * A text and the document parsed from it, extended together by each append:
 * the new document continues the previous one, so a node that continues an
 * old node keeps its identifier, and a node that continues none takes one the
 * session has never issued. Every column a scope query of its documents takes
 * or returns counts in [unit].
 *
 * Until [finalize], each document is a snapshot of the text so far: a lookup
 * of a reference, footnote or specimen sees only the definitions that start
 * before the inline root asking.
 *
 * The caller ends the text with [finalize], which returns the final
 * document; nothing ends a session implicitly. Afterwards [document] and
 * [text] stay readable, and [append] and [finalize] throw
 * [MarkdownCoreException] with [ErrorCode.SESSION_FINALIZED].
 *
 * The session also answers the queries of an editor: the [context] at its
 * cursor, which [locate] moves, the [path] of nodes that hold a byte, and the
 * [scope] of a node's fields and its [markers]. Each asks the engine's parse
 * of the session's text, which the session holds, so none parses the text
 * again or takes the source, and the documents stay values.
 *
 * A session has one writer at a time. [close] releases the engine's session,
 * its context with it: a step, a query, a spelling or a read of [text] after
 * it throws [IllegalStateException], and every document it returned stays a
 * complete value.
 *
 * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the engine
 *   cannot allocate, or [source] exceeds 1 GiB of UTF-8 or its tree a byte
 *   array's capacity.
 */
public class MarkdownSession
    @JvmOverloads
    constructor(
        source: String = "",
        /** How the session's documents count columns. */
        public val unit: TextUnit = TextUnit.UTF16,
    ) : AutoCloseable {
        private val session = newPlatformSession(source.encodeToByteArray(), unit)
        private var closed = false

        /** Counts the steps that replace [context]: a slot spells only in the turn that read it. */
        private var turn = 0

        /**
         * The syntactic context at the session's cursor, which [locate]
         * moves: the session's state, which it updates in place. It is a
         * context with nothing open until the first locate, and an append or
         * [finalize] empties it again, since they change the text it was read
         * from.
         */
        public var context: Context = blank
            private set

        /** The document the latest step published: parsed from [source], or from the last append. */
        public val document: Document
            get() = session.document

        /** The session's current text, which every append extends and [document] is parsed from. */
        public val text: String
            get() = open().text().decodeToString()

        /**
         * Appends [text] and parses the result.
         *
         * @return the new [document], a snapshot.
         * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the
         *   engine cannot allocate, the text would exceed 1 GiB of UTF-8, or the
         *   tree a byte array's capacity; [ErrorCode.SESSION_FINALIZED] after
         *   [finalize].
         */
        public fun append(text: String): Document = step { append(text.encodeToByteArray()) }

        /**
         * Ends the text and returns the final document, which equals
         * [Document.parse] of [text] in everything but identifiers.
         *
         * @return the final [document].
         * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the
         *   engine cannot allocate or the tree exceeds a byte array's capacity;
         *   [ErrorCode.SESSION_FINALIZED] when the session was already
         *   finalized.
         */
        public fun finalize(): Document = step { end() }

        /**
         * Moves the session's cursor to [position] of [text], which stands
         * between characters: its column may also stand at its line's end or
         * at the text's end. It counts columns in the session's [unit]. The
         * session's [context] becomes the context there.
         *
         * The query parses the text before the cursor and confirms what it
         * leaves open against the session's current [document], whose engine
         * parse the session holds. A snapshot confirms as it resolves: a label
         * sees only the definitions before the inline root asking.
         *
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when the
         *   position names no place of the text or its line or column is
         *   below 1, and [ErrorCode.ALLOCATION_FAILED] when an allocation
         *   fails; either leaves [context] as it was.
         */
        public fun locate(position: Position) {
            if (position.line < 1 || position.column < 1) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
            val message = open().locate(position.line, position.column)
            // A failure's answer throws here, before the turn moves on.
            context = AnswerDecoder.context(message, this, turn + 1)
            turn++
        }

        /**
         * The parser's spelling of each of [values] in [slot] of [context], in
         * order.
         *
         * A spelling writes the value in the field's own syntax: it escapes
         * what the field reads as syntax, and writes in `<...>`, in braces or
         * in quotes a value whose bare spelling the field does not read whole.
         * It is null for a value the field holds in no spelling there, such as
         * a line ending, a `|` in a cross target or a space in a key.
         *
         * The session spells from its context as the last locate left it, so
         * a spelling parses nothing.
         *
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [slot]
         *   is not a slot of [context]: another session's, or one a later
         *   locate, append or finalize replaced the context of, and
         *   [ErrorCode.ALLOCATION_FAILED] when an allocation fails.
         * @throws IllegalStateException when the session is closed.
         */
        public fun spell(
            slot: Slot,
            values: kotlin.collections.List<String>,
        ): kotlin.collections.List<Slot.Insertion?> {
            val platform = open()
            if (slot.session !== this || slot.turn != turn) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
            return values.map { AnswerDecoder.insertion(platform.spell(slot.index, it.encodeToByteArray()), unit) }
        }

        /** The parser's spelling of [value] in [slot] of [context], as the list form spells each value. */
        public fun spell(
            slot: Slot,
            value: String,
        ): Slot.Insertion? = spell(slot, listOf(value))[0]

        /**
         * Every node of [document] one of whose source ranges holds the byte
         * at [position] of [text], in canonical walk order, so the root first
         * and the node [Document.node] finds last, with the column counted in
         * the session's [unit]. None when no node holds the byte or the
         * position names no byte of the text at a scalar boundary.
         *
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when the
         *   position's line or column is below 1.
         */
        public fun path(position: Position): kotlin.collections.List<Markup> {
            if (position.line < 1 || position.column < 1) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
            val offset = SourceLines(open().text()).offset(position, unit) ?: return emptyList()
            return document.holders(offset)
        }

        /**
         * The source ranges of [node]'s fields of slot kind [field], one per
         * run of each, in source order: the bytes of each value as written,
         * escapes included, without the quotes, the angle brackets or the
         * sigil around it. Null asks for the fields no slot kind names, such
         * as a title or a literal. A field the parser makes rather than reads,
         * such as a heading's computed anchor, has none.
         *
         * A node's fields and its [markers] divide its own source: its runs
         * less the runs of the nodes it holds. Each query reads the node's own
         * source with the scanners its element read it with, from the engine
         * parse the session holds.
         *
         * [node] is a node of the current [document], or of an earlier one
         * when the current document holds it unchanged: a node an append left
         * alone keeps its id and equals its predecessor.
         *
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when the
         *   current document does not hold [node], and
         *   [ErrorCode.ALLOCATION_FAILED] when an allocation fails.
         */
        public fun scope(
            node: Markup,
            field: SlotKind?,
        ): kotlin.collections.List<Scope> =
            parts(node) { line, column -> field(node.id.value, line, column, field?.rawValue ?: 0) }

        /**
         * The source ranges of [node]'s markers: the rest of its own source,
         * the syntax no field holds, such as the `**` of a strong emphasis, a
         * heading's `#` run, a list item's marker or a fence, and the spaces
         * and line endings between them; see [scope].
         *
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when the
         *   current document does not hold [node], and
         *   [ErrorCode.ALLOCATION_FAILED] when an allocation fails.
         */
        public fun markers(node: Markup): kotlin.collections.List<Scope> =
            parts(node) { line, column -> markers(node.id.value, line, column) }

        /** Releases the engine's session and its context; a second call does nothing. */
        override fun close() {
            if (!closed) {
                closed = true
                session.free()
            }
        }

        /**
         * A step of the text. A step of a session not yet finalized empties
         * the engine's context whether or not it succeeds, and so [context].
         */
        private inline fun step(run: PlatformSession.() -> Document): Document {
            val platform = open()
            var finalized = false
            try {
                return platform.run()
            } catch (failure: MarkdownCoreException) {
                finalized = failure.code == ErrorCode.SESSION_FINALIZED
                throw failure
            } finally {
                if (!finalized) {
                    turn++
                    context = blank
                }
            }
        }

        /**
         * A field or marker query of [node] against the engine's parse, which
         * the current [document] copies: the engine finds the node by its id
         * among those that hold the first byte [node] holds. A node that holds
         * no byte owns no source.
         */
        private inline fun parts(
            node: Markup,
            query: PlatformSession.(Int, Int) -> ByteArray,
        ): kotlin.collections.List<Scope> {
            val engine = open()
            val lead = document.lead(node)?.toInt() ?: return emptyList()
            val start = SourceLines(engine.text()).scope(lead, lead + 1, unit).start
            return AnswerDecoder.scopes(engine.query(start.line, start.column))
        }

        /** The engine's session, which a closed session no longer has. */
        private fun open(): PlatformSession {
            check(!closed) { "The session is closed." }
            return session
        }
    }
