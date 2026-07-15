import type { Scope } from "./values.js";

export type ParseErrorCode = "invalidArgument" | "allocationFailed" | "internal";

export class ParseError extends Error {
    readonly code: ParseErrorCode;
    readonly scope: Scope | null;

    constructor(code: ParseErrorCode, message: string, parseScope: Scope | null = null) {
        super(message);
        this.name = "ParseError";
        this.code = code;
        this.scope = parseScope;
    }
}
