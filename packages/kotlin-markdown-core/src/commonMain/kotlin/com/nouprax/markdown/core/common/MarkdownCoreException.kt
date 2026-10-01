package com.nouprax.markdown.core

/** Why a call of the library failed. */
public enum class ErrorCode {
    /** The engine could not allocate, or the input exceeds a capacity: the 1 GiB source limit or an array's. */
    ALLOCATION_FAILED,

    /**
     * The source is too short for the node, a position's line or column is below 1, or a session rejects an
     * edit's range: it starts after its end, ends past the text, or overlaps another edit of its batch.
     */
    OUT_OF_BOUNDS,

    /** A value was read as another kind: the engine's code, shared by every binding; typed nodes never reach it. */
    KIND_MISMATCH,

    /**
     * A session edit's offset falls inside a scalar: at a continuation byte in UTF-8, or between the two halves
     * of a surrogate pair in UTF-16.
     */
    INSIDE_SCALAR,
}

/**
 * The library's one error: a call that cannot answer without crashing or
 * reading memory it does not own fails with this and its [code], and nothing
 * else.
 */
public class MarkdownCoreException(
    public val code: ErrorCode,
) : RuntimeException(code.name)
