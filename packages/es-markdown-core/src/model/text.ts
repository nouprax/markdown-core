import type { MarkupBase } from "./base.js";

export interface Text extends MarkupBase<"text"> {
    readonly literal: string;
}
