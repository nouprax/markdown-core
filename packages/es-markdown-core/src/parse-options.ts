export interface ParseOptions {
    readonly smartPunctuation?: boolean;
    readonly footnotes?: boolean;
    readonly tables?: boolean;
    readonly strikethrough?: boolean;
    readonly autolinks?: boolean;
    readonly taskLists?: boolean;
    readonly formulas?: boolean;
    readonly directives?: boolean;
    readonly crossLinks?: boolean;
    readonly embeds?: boolean;
}
