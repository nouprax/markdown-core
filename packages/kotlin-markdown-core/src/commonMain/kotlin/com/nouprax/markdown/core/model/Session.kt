package com.nouprax.markdown.core

/**
 * THE STREAM (docs/STREAMING.md §4 D5): a session, [feed], and the document's
 * two total views -- the same semantic tree and [Document.concrete] every
 * parse publishes.
 *
 * ```kotlin
 * val session = Session()
 * val updated = session.feed(chunk)
 * ```
 *
 * [feed] returns THE DOCUMENT AFTER THOSE BYTES: an immutable value the
 * caller owns outright and keeps -- like every [Document], it retains nothing
 * native, so it stays readable after every later feed and after the session
 * itself is gone. There is no ask and no snapshot handle; the return value is
 * the only answer there is.
 *
 * What a mid-stream document is: the projection of the parse as it stands. A
 * trailing line whose ending has not arrived is not yet in it, and an open
 * construct is projected as it stands -- a list still open has not settled
 * its tightness.
 *
 * [finish] ends the stream: the pending line is processed, every construct
 * closes, and the SEALED document comes back -- identical to what
 * [Document.parse] returns for the same bytes. It also ends the session's
 * parse: [feed] and [finish] after it throw [ParseException] with
 * [ParseErrorCode.INVALID_ARGUMENT]. What outlives the parse is only the
 * native shell the session is a handle to, and [close] takes that back; a
 * closed session refuses every call with [IllegalStateException].
 */
public class Session(
    options: ParseOptions = ParseOptions(),
) : AutoCloseable {
    private var handle: Long = nativeSessionNew(options)

    /**
     * Feeds exactly [chunk]'s UTF-8 bytes and returns the document after
     * them. A chunk may end anywhere -- mid-line, mid-character -- and an
     * empty chunk is a legal feed: the document as it stands.
     */
    public fun feed(chunk: ByteArray): Document = WireDecoder.decodeDocument(nativeSessionFeed(live(), chunk))

    /** Feeds [chunk] as its UTF-8 bytes. */
    public fun feed(chunk: String): Document = feed(chunk.encodeToByteArray())

    /** Ends the stream and returns the sealed document. */
    public fun finish(): Document = WireDecoder.decodeDocument(nativeSessionFinish(live()))

    /**
     * Frees the native shell. Idempotent, and it takes nothing else with it:
     * every document this session returned remains a plain value.
     */
    override fun close() {
        if (handle != 0L) {
            nativeSessionFree(handle)
            handle = 0L
        }
    }

    private fun live(): Long {
        check(handle != 0L) { "the session is closed" }
        return handle
    }
}
