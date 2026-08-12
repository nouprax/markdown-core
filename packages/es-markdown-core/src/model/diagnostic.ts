import type { Scope } from "../values.js";

/**
 * Why a diagnostic was raised.
 *
 * One case, and it is not a placeholder. Markdown has no parse errors: every
 * byte sequence is a valid document, and the constructs an author most often
 * gets "wrong" — an unclosed fence, a link reference with no definition, a
 * short table row — are all defined outcomes of the standard semantics
 * rather than failures. Reporting those would be reporting Markdown itself.
 *
 * A value and a type under one name, like {@link WalkEvent}: a consumer compares
 * `diagnostic.code` against `DiagnosticCode.directiveAttributes` at runtime,
 * which a bare union of string literals cannot offer. It is not a TypeScript
 * `enum` keyword, which emits a namespace object this ESM package would then
 * have to ship and which erasable-syntax builds reject.
 *
 * `directiveAttributes` is the exception, because a directive's attribute
 * grammar is this library's own rather than Markdown's: braces that do not
 * parse are a mistake with no defined fallback meaning, and the parse
 * degrades them to literal text. That degradation is invisible in the tree —
 * the node simply has no attributes — so without this a editor cannot tell
 * "no attributes were written" from "the attributes were rejected".
 */
export const DiagnosticCode = {
    /** A directive's `{...}` attribute block did not parse. */
    directiveAttributes: "directiveAttributes"
} as const;

export type DiagnosticCode = (typeof DiagnosticCode)[keyof typeof DiagnosticCode];

/**
 * One thing to underline: what, and where.
 *
 * No message. A message is a string shown to a human, which means it is
 * localized, and localizing it in the library would put one English sentence
 * in it and four copies of the decision to ignore that sentence in the
 * bindings. The code says what happened; the wording belongs to whoever is
 * speaking to the user.
 */
export interface Diagnostic {
    readonly code: DiagnosticCode;
    readonly scope: Scope;
}
