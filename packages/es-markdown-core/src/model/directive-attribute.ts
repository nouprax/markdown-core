/**
 * One directive attribute. Its sequence preserves first-occurrence source
 * order; later occurrences update or accumulate in that original position.
 */
export interface DirectiveAttribute {
    readonly name: string;
    readonly value: string;
}
