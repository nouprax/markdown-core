import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

export interface Formula extends MarkupBase<"formula"> {
    readonly mode: PlacementMode;
    readonly literal: string;
}
