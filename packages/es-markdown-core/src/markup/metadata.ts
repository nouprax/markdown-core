import type { MarkupBase } from "./base.js";

export interface Metadata extends MarkupBase<"metadata"> {
    readonly name: MetadataValue | null;
    readonly title: MetadataValue | null;
    readonly subtitle: MetadataValue | null;
    readonly time: MetadataValue | null;
    readonly date: MetadataValue | null;
    readonly authors: MetadataValue | null;
    readonly keywords: MetadataValue | null;
    readonly abstract: MetadataValue | null;
    readonly state: MetadataValue | null;
    readonly comment: MetadataValue | null;
}
export type MetadataValue =
    | { readonly kind: "scalar"; readonly value: MetadataScalar }
    | { readonly kind: "list"; readonly items: readonly MetadataListItem[] };
export type MetadataScalar =
    | { readonly kind: "null" }
    | { readonly kind: "bool"; readonly value: boolean }
    | { readonly kind: "number"; readonly value: string }
    | { readonly kind: "text"; readonly value: string };
export type MetadataListItem =
    { readonly kind: "number"; readonly value: string } | { readonly kind: "text"; readonly value: string };
