export type ParseErrorCode = "invalidArgument" | "allocationFailed" | "internal";

/**
 * A parse failure, and nothing else.
 *
 * It carries no scope: an input the parser could not turn into a document has
 * no document extent to point at.
 */
export class ParseError extends Error {
    readonly code: ParseErrorCode;

    constructor(code: ParseErrorCode, message: string) {
        super(message);
        this.name = "ParseError";
        this.code = code;
    }
}
