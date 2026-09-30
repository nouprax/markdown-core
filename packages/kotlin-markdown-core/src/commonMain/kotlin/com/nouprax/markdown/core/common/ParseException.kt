package com.nouprax.markdown.core

public enum class ParseErrorCode { ALLOCATION_FAILED }

/**
 * A parse failure, and NOTHING ELSE.
 *
 * It carries no scope: an input the parser could not turn into a document has
 * no document extent to point at.
 */
public class ParseException(
    public val code: ParseErrorCode,
    override val message: String,
) : RuntimeException(message)
