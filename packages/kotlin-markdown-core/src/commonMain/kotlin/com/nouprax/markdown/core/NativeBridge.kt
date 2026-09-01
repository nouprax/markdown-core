package com.nouprax.markdown.core

/**
 * The stream's native side, addressed by an opaque handle.
 *
 * A handle, unlike everything else this bridge moves, is a live native
 * pointer: [Document] owns it, feeds it, and frees it exactly once. `0` is
 * never a live handle. Feed and finish answer with one wire payload shape,
 * so every [Read] is decoded by exactly one path. Both take the frame
 * `request` (#162): [WireDecoder.DELTA_FRAME] asks for a delta against the
 * last payload the session wrote, which the native side answers only when
 * it can; anything else asks for [WireDecoder.FULL_FRAME].
 */
internal expect fun nativeSessionNew(options: ParseOptions): Long

internal expect fun nativeSessionFeed(
    session: Long,
    chunk: ByteArray,
    request: Int,
): ByteArray

/**
 * Feed whose read is DISCARDED BY CONTRACT -- the constructor's initial feed.
 * The native side takes the bytes without projecting or serializing a read
 * nothing would decode; the payload is the bare envelope, or the error in it.
 */
internal expect fun nativeSessionAdvance(
    session: Long,
    chunk: ByteArray,
): ByteArray

internal expect fun nativeSessionFinish(
    session: Long,
    request: Int,
): ByteArray

internal expect fun nativeSessionFree(session: Long)
