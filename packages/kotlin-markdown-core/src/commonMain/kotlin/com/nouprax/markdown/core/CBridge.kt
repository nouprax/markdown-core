@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

internal expect fun cParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray

internal typealias CSessionHandle = Long

/**
 * Raw handle to one native incremental session. Callers synchronize
 * externally, keep every call before [free], and call [free] exactly once;
 * [MarkupSession] is the only owner.
 */
internal expect fun openCSession(options: ParseOptions): CSessionHandle

internal expect fun CSessionHandle.free()

internal expect fun CSessionHandle.lineage(): ULong

internal expect fun CSessionHandle.revision(): ULong

internal expect fun CSessionHandle.length(): Long

internal expect fun CSessionHandle.rootId(): ULong

internal expect fun CSessionHandle.edit(
    byteStart: Long,
    byteEnd: Long,
    replacement: ByteArray,
): ByteArray

internal expect fun CSessionHandle.commit(): ByteArray

internal expect fun CSessionHandle.scopes(): ByteArray

internal expect fun CSessionHandle.footnoteInfo(id: ULong): ByteArray

internal expect fun CSessionHandle.footnotes(): ByteArray

internal expect fun CSessionHandle.footnoteReferences(definition: ULong): ByteArray
