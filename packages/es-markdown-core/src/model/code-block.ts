import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

export interface CodeBlock extends MarkupBase<"codeBlock"> {
    readonly mode: PlacementMode;
    readonly info: string | null;
    readonly language: string | null;
    readonly literal: string;
    readonly fenced: boolean;
    readonly closed: boolean;
}
