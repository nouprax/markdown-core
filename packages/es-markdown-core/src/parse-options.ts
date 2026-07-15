export interface ParseOptions {
    readonly smartPunctuation?: boolean;
    readonly footnotes?: boolean;
    readonly stripHTMLComments?: boolean;
    readonly tables?: boolean;
    readonly strikethrough?: boolean;
    readonly autolinks?: boolean;
    readonly taskLists?: boolean;
    readonly formulas?: boolean;
    readonly dollarFormulaDelimiters?: boolean;
    readonly latexFormulaDelimiters?: boolean;
    readonly directives?: boolean;
}
