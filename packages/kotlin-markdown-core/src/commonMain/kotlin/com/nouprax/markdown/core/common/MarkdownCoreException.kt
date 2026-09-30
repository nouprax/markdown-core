package com.nouprax.markdown.core

/** Why a call of the library failed. */
public enum class ErrorCode {
    /** The engine could not allocate, or the input exceeds a capacity: the 1 GiB source limit or an array's. */
    ALLOCATION_FAILED,

    /**
     * The source is too short for the node, a position's line or column is below 1, or a session rejects an
     * edit's range: it starts after its end, ends past the text, overlaps another edit of its batch, or, in
     * UTF-16, has an offset between the two halves of a surrogate pair.
     */
    OUT_OF_BOUNDS,

    /** A value was read as another kind: the engine's code, shared by every binding; typed nodes never reach it. */
    KIND_MISMATCH,
}

/**
 * The library's one error: a call that cannot answer without crashing or
 * reading memory it does not own fails with this and its [code], and nothing
 * else.
 */
public class MarkdownCoreException(
    public val code: ErrorCode,
) : RuntimeException(code.name)
