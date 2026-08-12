import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { DirectiveLabel } from "./directive-label.js";

export interface Directive extends MarkupBase<"directive"> {
    readonly mode: PlacementMode;
    readonly name: string;
    /** The directive's attribute map, in source order, or null when no
     * `{...}` container was written. An empty container is an empty map. */
    readonly attributes: Readonly<Record<string, string>> | null;
    readonly label: DirectiveLabel | null;
}
