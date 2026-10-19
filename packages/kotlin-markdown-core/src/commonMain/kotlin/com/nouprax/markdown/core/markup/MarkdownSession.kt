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
 * A session has one writer at a time. [close] releases the engine's session:
 * a step or a read of [text] after it throws [IllegalStateException], and
 * every document it returned stays a complete value.
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
        public fun append(text: String): Document = open().append(text.encodeToByteArray())

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
        public fun finalize(): Document = open().end()

        /** Releases the engine's session; a second call does nothing. */
        override fun close() {
            if (!closed) {
                closed = true
                session.free()
            }
        }

        /** The engine's session, which a closed session no longer has. */
        private fun open(): PlatformSession {
            check(!closed) { "The session is closed." }
            return session
        }
    }
