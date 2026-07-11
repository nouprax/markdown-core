import type { MarkupBase } from "./base.js";

export interface HTML extends MarkupBase<"html"> {
    readonly literal: string;
}
