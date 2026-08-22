import type { MarkupBase } from "./base.js";

export interface CodeBlock extends MarkupBase<"codeBlock"> {
    readonly info: string | null;
    readonly language: string | null;
    readonly literal: string;
    readonly fenced: boolean;
    readonly closed: boolean;
}
