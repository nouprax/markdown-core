package com.nouprax.markdown.core

public enum class ParseErrorCode { INVALID_ARGUMENT, ALLOCATION_FAILED, INTERNAL }

/**
 * A parse failure, and NOTHING ELSE.
 *
 * It carries no scope: an input the parser could not turn into a document has
 * no extent to point at, and a failure the author could act on would have been
 * a diagnostic instead.
 */
public class ParseException(
    public val code: ParseErrorCode,
    override val message: String,
) : RuntimeException(message)
