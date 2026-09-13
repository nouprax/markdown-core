export interface Record {
    readonly name: string;
    readonly value: string;
}
export interface Attributes {
    readonly classes: readonly string[];
    readonly records: readonly Record[];
}
export const Attributes: { readonly empty: Attributes } = Object.freeze({
    empty: Object.freeze({ classes: Object.freeze([]), records: Object.freeze([]) })
});
