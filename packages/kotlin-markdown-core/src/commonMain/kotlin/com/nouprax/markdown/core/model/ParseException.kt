package com.nouprax.markdown.core

public enum class ParseErrorCode { INVALID_ARGUMENT, ALLOCATION_FAILED, INTERNAL }

public class ParseException(
    public val code: ParseErrorCode,
    override val message: String,
    public val scope: Scope?,
) : RuntimeException(message)
