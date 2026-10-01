package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads

/**
 * A text and the document parsed from it, changed together by each edit: the
 * new document continues the previous one, so a node that continues an old
 * node keeps its identifier, and a node that continues none takes one the
 * session has never issued. Every offset an edit passes in and every column a
 * scope query of its documents returns counts in [unit].
 *
 * A session has one writer at a time. [close] releases the engine's session:
 * an edit, an append or a read of [text] after it throws
 * [IllegalStateException], and every document it returned stays a complete
 * value.
 *
 * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the engine
 *   cannot allocate, or [source] exceeds 1 GiB of UTF-8 or its tree a byte
 *   array's capacity.
 */
public class MarkdownSession
    @JvmOverloads
    constructor(
        source: String = "",
        /** How the session counts edit offsets, and how its documents count columns. */
        public val unit: TextUnit = TextUnit.UTF16,
    ) : AutoCloseable {
        private val session = newPlatformSession(source.encodeToByteArray(), unit)
        private var closed = false

        /** The document the latest step published: parsed from [source], or from the last edit or append. */
        public val document: Document
            get() = session.document

        /** The session's current text, which every step changes and [document] is parsed from. */
        public val text: String
            get() = open().text().decodeToString()

        /**
         * Replaces each edit's range with its text and parses the result once;
         * a single replacement is a batch of one. The edits are disjoint ranges
         * in [unit] of the text before the batch, listed in any order, and two
         * edits at one offset apply in the order listed.
         *
         * @return the new [document].
         * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when an edit
         *   starts after its end or ends past the text, or two edits overlap;
         *   [ErrorCode.INSIDE_SCALAR] when an offset falls inside a scalar;
         *   [ErrorCode.ALLOCATION_FAILED] when the engine cannot allocate,
         *   the text would exceed 1 GiB of UTF-8, or the tree a byte array's
         *   capacity.
         */
        public fun edit(edits: kotlin.collections.List<TextEdit>): Document {
            val encoded = edits.map { it.text.encodeToByteArray() }
            // Three fields per edit: start, end, and the byte size of its text,
            // whose bytes follow one another in `texts` in the order listed.
            val fields = IntArray(edits.size * 3)
            val texts = ByteArray(encoded.sumOf { it.size })
            var offset = 0
            for ((index, edit) in edits.withIndex()) {
                val bytes = encoded[index]
                fields[index * 3] = edit.start
                fields[index * 3 + 1] = edit.end
                fields[index * 3 + 2] = bytes.size
                bytes.copyInto(texts, offset)
                offset += bytes.size
            }
            return open().edit(fields, texts)
        }

        /**
         * Appends [text] and parses the result: the edit at the end of the text.
         *
         * @return the new [document].
         * @throws MarkdownCoreException [ErrorCode.ALLOCATION_FAILED] when the
         *   engine cannot allocate, the text would exceed 1 GiB of UTF-8, or the
         *   tree a byte array's capacity.
         */
        public fun append(text: String): Document = open().append(text.encodeToByteArray())

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

/**
 * One replacement of a [MarkdownSession.edit] batch: the text in
 * [[start], [end]), offsets in the session's unit of the text before the
 * batch, becomes [text].
 */
public data class TextEdit(
    public val start: Int,
    public val end: Int,
    public val text: String,
)
