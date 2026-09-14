@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.markdown_core_kotlin_payload_encode
import com.nouprax.markdown.core.internal.capi.markdown_core_kotlin_payload_free
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.posix.size_tVar

/**
 * One C call parses the source and encodes the whole document as the
 * payload every Kotlin target decodes; the bytes are copied out of C once
 * and the C side is freed before the decoder runs, so no node crosses the
 * cinterop boundary on its own and nothing native outlives the call.
 */
internal actual fun parsePlatformDocument(source: ByteArray): Document =
    memScoped {
        val output = alloc<CPointerVar<UByteVar>>()
        val length = alloc<size_tVar>()
        val encoded =
            if (source.isEmpty()) {
                markdown_core_kotlin_payload_encode(null, 0u, output.ptr, length.ptr)
            } else {
                source.usePinned { pinned ->
                    markdown_core_kotlin_payload_encode(
                        pinned.addressOf(0).reinterpret(),
                        source.size.toULong(),
                        output.ptr,
                        length.ptr,
                    )
                }
            }
        if (!encoded) throw ParseException(ParseErrorCode.ALLOCATION_FAILED, "native AST payload allocation failed")
        val payload = requireNotNull(output.value) { "native payload encoder answered no bytes" }
        try {
            val size = length.value
            if (size > Int.MAX_VALUE.toULong()) {
                throw ParseException(ParseErrorCode.ALLOCATION_FAILED, "native AST exceeds the array limit")
            }
            PayloadDecoder.decode(payload.readBytes(size.toInt()))
        } finally {
            markdown_core_kotlin_payload_free(payload)
        }
    }
