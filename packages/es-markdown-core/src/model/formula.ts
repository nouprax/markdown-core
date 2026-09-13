import type { Placement } from "../values.js";
import type { MarkupBase } from "./base.js";

export interface Formula extends MarkupBase<"formula"> {
    readonly mode: Placement;
    readonly literal: string;
}
