import type { Markup } from "./markup.js";
import type { Scope } from "../values.js";

/** A document-owned specimen definition; its displayed number is derived. */
export interface Specimen {
    readonly scope: Scope;
    /** The authored label, or null for an anonymous definition. */
    readonly id: string | null;
    /** An explicit effective counter reset, or null for continuation. */
    readonly start: number | null;
    readonly content: readonly Markup[];
}
