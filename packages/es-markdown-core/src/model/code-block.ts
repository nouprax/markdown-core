import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

/** An indented or fenced code block. */
export interface CodeBlock extends MarkupBase<"codeBlock"> {
    /** Always `standalone`.
     *
     * The embedded form of code is {@link Code}, a different kind. */
    readonly mode: PlacementMode;
    /** The whole info string written after the opening fence: null when the
     * fence carried none, and always null for an indented block. */
    readonly info: string | null;
    /** The info string's first whitespace-delimited word — the conventional
     * language tag — so a consumer choosing a highlighter never splits
     * `info` itself.
     *
     * Null when there is no info string to take one from. */
    readonly language: string | null;
    /** The code itself: a fenced block's fences and info string are outside
     * it, and an indented block's four columns of indentation are stripped
     * from every line. */
    readonly literal: string;
    readonly fenced: boolean;
    /** Whether a fenced block's closing fence was written.
     *
     * An indented block has no fence to leave open and always reports true;
     * text ending inside an open fence reports false. */
    readonly closed: boolean;
}
