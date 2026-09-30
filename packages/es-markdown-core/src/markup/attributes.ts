export interface Record {
    readonly name: string;
    readonly value: string;
}
export interface Attributes {
    readonly classes: readonly string[];
    readonly records: readonly Record[];
}
export const Attributes: { readonly empty: Attributes } = {
    empty: { classes: [], records: [] }
};
