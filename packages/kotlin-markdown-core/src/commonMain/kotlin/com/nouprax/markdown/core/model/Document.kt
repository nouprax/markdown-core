package com.nouprax.markdown.core

/**
 * The living document: the one entry into this parser, fed in pieces and
 * answering with [Read] values (docs/STREAMING.md §4 D5, under 3.0's names).
 *
 * ```kotlin
 * // whole text at once
 * val read = Document("# Hello").seal()
 *
 * // streamed
 * Document().use { document ->
 *     val updated = document.feed(chunk)
 *     val sealed = document.seal()
 * }
 * ```
 *
 * [feed] returns THE READ AFTER THOSE BYTES: an immutable value the caller
 * owns outright and keeps -- it retains nothing native, so it stays readable
 * after every later feed and after this document is gone. There is no ask and
 * no snapshot handle; the return value is the only answer there is.
 *
 * What a mid-stream read is: the projection of the parse as it stands. A
 * trailing line whose ending has not arrived is not yet in it, and an open
 * construct is projected as it stands -- a list still open has not settled
 * its tightness.
 *
 * [seal] ends the stream: the pending line is processed, every construct
 * closes, and the SEALED read comes back -- the whole text's, identical for
 * the same bytes however they were fed. Sealing also releases the native
 * shell: a sealed document IS a closed one, and every later call throws
 * [IllegalStateException]. [close] exists for the stream that is abandoned
 * instead of sealed; it is idempotent, and it takes nothing else with it --
 * every read this document returned remains a plain value.
 */
public class Document(
    options: ParseOptions = ParseOptions(),
) : AutoCloseable {
    private var handle: Long = nativeSessionNew(options)

    /**
     * Opens a document and feeds it [markdown] in one step -- exactly
     * `Document(options)` followed by one [feed] whose returned read is
     * discarded (and, being discarded, never decoded), so the whole-text
     * parse is `Document(markdown).seal()`.
     */
    public constructor(markdown: String, options: ParseOptions = ParseOptions()) : this(options) {
        WireDecoder.decodeDiscarded(nativeSessionFeed(live(), markdown.encodeToByteArray()))
    }

    /**
     * Feeds exactly [chunk]'s UTF-8 bytes and returns the read after them. A
     * chunk may end anywhere -- mid-line, mid-character -- and an empty chunk
     * is a legal feed: the read as it stands.
     */
    public fun feed(chunk: ByteArray): Read = WireDecoder.decodeRead(nativeSessionFeed(live(), chunk))

    /** Feeds [chunk] as its UTF-8 bytes. */
    public fun feed(chunk: String): Read = feed(chunk.encodeToByteArray())

    /**
     * Ends the stream, returns the sealed read, and releases the native
     * shell: after this, the document is closed and every call throws
     * [IllegalStateException].
     */
    public fun seal(): Read {
        val sealed = WireDecoder.decodeRead(nativeSessionFinish(live()))
        close()
        return sealed
    }

    /**
     * Frees the native shell of a stream abandoned before [seal]. Idempotent,
     * and it takes nothing else with it: every read this document returned
     * remains a plain value.
     */
    override fun close() {
        if (handle != 0L) {
            nativeSessionFree(handle)
            handle = 0L
        }
    }

    private fun live(): Long {
        check(handle != 0L) { "the document is sealed or closed" }
        return handle
    }
}
