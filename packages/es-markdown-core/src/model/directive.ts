import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { DirectiveLabel } from "./directive-label.js";

/** An inline directive: `:name`, optionally followed by `[label]` and then
 * `{attributes}`. */
export interface Directive extends MarkupBase<"directive"> {
    /** Always `embedded`.
     *
     * A directive that stands on its own line is a {@link DirectiveBlock}, a
     * different kind. */
    readonly mode: PlacementMode;
    /** The name written after the colon, never empty. */
    readonly name: string;
    /** The directive's attribute map, in source order, or null when no
     * `{...}` container was written.
     *
     * An empty container is an empty map. */
    readonly attributes: Readonly<Record<string, string>> | null;
    /** The directive's label, or null when none was written.
     *
     * An explicit empty `[]` is a present label whose content is empty, and
     * stays distinct from null. */
    readonly label: DirectiveLabel | null;
}
