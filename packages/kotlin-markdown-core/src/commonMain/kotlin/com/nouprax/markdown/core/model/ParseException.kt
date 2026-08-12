package com.nouprax.markdown.core

public enum class ParseErrorCode { INVALID_ARGUMENT, ALLOCATION_FAILED, INTERNAL }

/**
 * A call into the engine failed: thrown by the [Document] constructor and by
 * [Document.edit].
 *
 * Never a verdict on the Markdown. Every byte sequence is a valid document,
 * so no text a caller can hand over produces one of these; what is left is
 * one of three — see [ParseErrorCode]:
 *
 * - a rejected argument
 * - an allocation that failed
 * - a bug
 */
public class ParseException(
    public val code: ParseErrorCode,
    /** The engine's own account of the failure: one English string, never
     * localized. */
    override val message: String,
) : RuntimeException(message)
