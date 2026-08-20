import type { PlacementMode } from "../values.js";
import type { MarkupBase } from "./base.js";

export interface Code extends MarkupBase<"code"> {
    readonly mode: PlacementMode;
    readonly literal: string;
}
