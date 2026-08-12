import type { Markup } from "./markup.js";
import type { MarkupBase } from "./base.js";

/**
 * How a reference was written.
 *
 * The three resolve identically and differ only in source form, which the
 * tree keeps because it is what was written.
 */
export type ReferenceForm = "full" | "collapsed" | "shortcut";

/**
 * A link reference definition, at the position it was written.
 *
 * The destination is stated here, once, rather than copied into every
 * reference that resolves to it.
 */
export interface ReferenceDefinition extends MarkupBase<"referenceDefinition"> {
    /** The label between `[` and `]`, exactly as written. */
    readonly label: string;
    /** The destination this definition assigns to its label.
     *
     * Empty when written empty, as `[foo]: <>` does. Never null: a definition
     * that writes no destination is not a definition at all. */
    readonly destination: string;
    /** The optional title. */
    readonly title: string | null;
}

/**
 * `[text][label]`, `[label][]`, or `[label]`.
 *
 * It carries no destination: which definition the label resolves to is an
 * answer, asked of the document rather than read off the node. {@link Link}
 * stays the inline form `[a](/u)`, whose destination is written in the
 * source.
 */
export interface LinkReference extends MarkupBase<"linkReference"> {
    /** The label this reference resolves by, exactly as written. */
    readonly label: string;
    /** The source form the reference was written in. */
    readonly form: ReferenceForm;
    /** The reference's inline caption content in source order. */
    readonly content: readonly Markup[];
}

/** `![alt][label]` and its collapsed and shortcut forms. */
export interface ImageReference extends MarkupBase<"imageReference"> {
    /** The label this reference resolves by, exactly as written. */
    readonly label: string;
    /** The source form the reference was written in. */
    readonly form: ReferenceForm;
    /** The reference's alternative-text content in source order. */
    readonly content: readonly Markup[];
}
