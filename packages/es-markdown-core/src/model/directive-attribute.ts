/**
 * One directive attribute. The sequence is sorted by name, so a pair is all
 * there is to say about one entry.
 */
export interface DirectiveAttribute {
    readonly name: string;
    readonly value: string;
}
