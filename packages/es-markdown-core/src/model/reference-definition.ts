import type { MarkupBase } from "./base.js";

/**
 * A link reference definition, at the byte where its opening bracket was
 * written. `label` is the bytes between the brackets exactly as the source
 * spells them; `norm` is the match key — full Unicode case fold, trimmed,
 * internal whitespace collapsed — and neither derives the other.
 * `destination` is never absent, because a definition that could not build one
 * is not produced at all; `title` is `null` when the source wrote none and
 * `""` when it wrote an empty one.
 */
export interface ReferenceDefinition extends MarkupBase<"referenceDefinition"> {
    readonly label: string;
    readonly norm: string;
    readonly destination: string;
    readonly title: string | null;
}
