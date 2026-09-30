/**
 * Why a public call failed. Each is the C `markdown_core_status` of the same
 * name:
 * - `"allocationFailed"`: the parse could not allocate, or the source exceeds
 *   the engine's capacity;
 * - `"outOfBounds"`: an argument is outside the range the call reads, such as
 *   a source too short for the node, a position whose line or column is not
 *   an integer of at least 1, or an edit range the session's text does not
 *   hold;
 * - `"kindMismatch"`: a value is not of the kind the call reads; no call of
 *   this binding reads a kind the tree has not already typed, so none reports
 *   it.
 */
export type ErrorCode = "allocationFailed" | "outOfBounds" | "kindMismatch";

/** The one error every public call of the library throws, carrying its code. */
export class MarkdownCoreError extends Error {
    readonly code: ErrorCode;

    constructor(code: ErrorCode) {
        super(code);
        this.name = "MarkdownCoreError";
        this.code = code;
    }
}
