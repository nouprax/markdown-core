/**
 * Why the engine could not produce a document.
 *
 * None of the three is a complaint about the Markdown. Every byte sequence is
 * a valid document, so a {@link ParseError} says the parse could not be RUN
 * at all: the engine
 *
 * - rejected an argument
 * - could not allocate
 * - broke its own invariant
 */
export type ParseErrorCode = "invalidArgument" | "allocationFailed" | "internal";

/**
 * A native failure — see {@link ParseErrorCode} for what can go wrong.
 *
 * A caller's own mistake, such as markdown that is not a string or options
 * that are not an object, is a `TypeError` thrown before the engine is
 * reached and never one of these.
 */
export class ParseError extends Error {
    readonly code: ParseErrorCode;

    constructor(code: ParseErrorCode, message: string) {
        super(message);
        this.name = "ParseError";
        this.code = code;
    }
}
