import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";
import type { DirectiveLabel } from "./directive-label.js";
import type { Markup } from "./markup.js";

/** A directive that stands on its own line: `::name` for a leaf, or a run of
 * three or more colons for a container, whose blocks run to a closing run at
 * least as long — or to the end of its parent when none is written.
 *
 * Either form may carry `[label]` and then `{attributes}`. */
export interface DirectiveBlock extends MarkupBase<"directiveBlock"> {
    /** Always `standalone`.
     *
     * A directive written inside inline content is a {@link Directive}, a
     * different kind. */
    readonly mode: PlacementMode;
    /** The name written after the colons, never empty. */
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
    /** The directive's block content in source order.
     *
     * The label is not part of it: a written label is the directive's first
     * canonical child, and this list begins after it. A leaf `::name` closes
     * on its own line, so its content is empty. */
    readonly content: readonly Markup[];
}
