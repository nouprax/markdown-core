/**
 * A parse failure, and nothing else.
 *
 * It carries no scope: an input the parser could not turn into a document has
 * no extent to point at, and a failure the author could act on would have been
 * a diagnostic instead.
 */
export class ParseError extends Error {
    code;
    constructor(code, message) {
        super(message);
        this.name = "ParseError";
        this.code = code;
    }
}
