import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Destination, Dimensions } from "../values.js";

/**
 * Inline media from direct or resolved Markdown image syntax. The target type is not inferred.
 * Complete `W`, `WxH`, `alt|W` and `alt|WxH` labels
 * supply positive 32-bit dimensions without leading zeros.
 */
export interface Media extends MarkupBase<"media"> {
    /** Required, for the reason `Link.dest` is. */
    readonly dest: Destination;
    /** Optional. */
    readonly title: string | null;
    /** Authored size from a complete label suffix, or null. Independent of attribute records. */
    readonly dimensions: Dimensions | null;
    /** Parsed alt excluding a valid suffix; numeric-only labels are empty, malformed suffixes remain. */
    readonly content: readonly Markup[];
}
