package com.nouprax.markdown.core

internal expect fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray

/**
 * The stream's native side, addressed by an opaque handle.
 *
 * A handle, unlike everything else this bridge moves, is a live native
 * pointer: [Session] owns it, feeds it, and frees it exactly once. `0` is
 * never a live handle. Feed and finish answer with the SAME wire payload
 * [nativeParse] answers with, so a streamed document is decoded by exactly
 * the one-shot path.
 */
internal expect fun nativeSessionNew(options: ParseOptions): Long

internal expect fun nativeSessionFeed(
    session: Long,
    chunk: ByteArray,
): ByteArray

internal expect fun nativeSessionFinish(session: Long): ByteArray

internal expect fun nativeSessionFree(session: Long)
