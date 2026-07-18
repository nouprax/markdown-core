package com.nouprax.markdown.core

internal expect fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray

/**
 * Raw handle to one native incremental session. Callers synchronize
 * externally, keep every call before [free], and call [free] exactly once;
 * [MarkupSession] is the only owner.
 */
internal expect class CSession(
    options: ParseOptions,
) {
    fun free()

    fun lineage(): ULong

    fun revision(): ULong

    fun length(): Long

    fun rootId(): ULong

    fun edit(
        byteStart: Long,
        byteEnd: Long,
        replacement: ByteArray,
    ): ByteArray

    fun commit(): ByteArray

    fun scopes(): ByteArray

    fun footnoteInfo(id: ULong): ByteArray

    fun footnotes(): ByteArray

    fun footnoteReferences(definition: ULong): ByteArray
}
