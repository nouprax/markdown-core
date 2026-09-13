import type { MarkupBase } from "./base.js";

export interface Code extends MarkupBase<"code"> {
    readonly literal: string;
}
