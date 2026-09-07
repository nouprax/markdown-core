import type { Markup } from "./markup.js";
import type { Scope } from "../values.js";

/**
 * One element of `Document.footnotes`: a scoped value, not a `Markup` kind.
 * It is written, so it has a scope, and it owns its content, but it is never
 * a child of a node. `id` is the definition's label under the reference-label
 * normalization without the caret, the id every `footnote` referent naming it
 * carries, compared byte for byte.
 */
export interface Footnote {
    readonly scope: Scope;
    readonly id: string;
    /** The parsed block content of the definition. */
    readonly content: readonly Markup[];
}
