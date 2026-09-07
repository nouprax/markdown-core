@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class)

package com.nouprax.markdown.core

import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS
import com.nouprax.markdown.core.internal.capi.MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
import com.nouprax.markdown.core.internal.capi.markdown_core_ordered_list_delimiter
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs

class NativeListDelimiterTest {
    @Test
    fun allNativeDelimiterBranchesDecodeWithoutLosingTheirPayload(): Unit =
        memScoped {
            val value = alloc<markdown_core_ordered_list_delimiter>()
            value.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD
            value.closed = false
            assertEquals(OrderedListDelimiter.Period, decodeNativeListDelimiter(value))
            value.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT
            assertEquals(OrderedListDelimiter.Default, decodeNativeListDelimiter(value))
            value.kind = MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS
            for (closed in listOf(false, true)) {
                value.closed = closed
                assertEquals(
                    closed,
                    assertIs<OrderedListDelimiter.Parenthesis>(decodeNativeListDelimiter(value)).closed,
                )
            }
            value.kind = 99u
            assertFailsWith<IllegalStateException> { decodeNativeListDelimiter(value) }
        }
}
