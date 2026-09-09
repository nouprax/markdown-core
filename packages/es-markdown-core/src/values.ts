export interface Position {
    readonly line: number;
    readonly column: number;
}

export interface Scope {
    readonly start: Position;
    readonly end: Position;
}

export type ListFlavor = "bullet" | "ordered";
export type OrderedListVariant =
    | "decimal"
    | { readonly kind: "alpha"; readonly lowercased: boolean }
    | { readonly kind: "roman"; readonly lowercased: boolean }
    | "default";
export type OrderedListDelimiter = "period" | { readonly kind: "parenthesis"; readonly closed: boolean } | "default";
/**
 * The target of a `Link` or `Media`: a tagged value, not a node, so it has no
 * scope and no children, and a branch's fields exist only in that branch.
 * Every link and image owns the `url` branch, the complete semantic
 * destination the inherited grammar produced -- decoded, not percent-encoded,
 * normalized, or resolved, and possibly empty. The `cross` branch is the
 * workspace address a cross link produces.
 */
export type Destination =
    | { readonly kind: "url"; readonly value: string }
    | { readonly kind: "cross"; readonly path: string; readonly anchor: string | null };
export type PlacementMode = "embedded" | "standalone";
/**
 * How a bibliographic citation is rendered: `[@key]` is `normal`, `@key` in
 * running text names the author in text, and `-@key` suppresses the author.
 */
export type BibMode = "normal" | "authorInText" | "suppressAuthor";
/**
 * What a `Citation` names: a tagged value, not a node, so it has no scope,
 * and a branch's fields exist only in that branch. The `footnote` branch
 * names the `Footnote` in `Document.footnotes` with the equal id; the `bib`
 * branch is produced by the citations module.
 */
export type CitationReferent =
    | { readonly kind: "bib"; readonly key: string; readonly mode: BibMode }
    | { readonly kind: "footnote"; readonly id: string }
    | { readonly kind: "specimen"; readonly id: string };
export type TableAlignment = "none" | "left" | "center" | "right";

export interface Record {
    readonly name: string;
    readonly value: string;
}
export interface Attributes {
    readonly classes: readonly string[];
    readonly records: readonly Record[];
}
export const Attributes: { readonly empty: Attributes } = { empty: { classes: [], records: [] } };
export interface Metadata {
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
    readonly scope: Scope;
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

/** A node-independent size. Every present component is a positive 32-bit integer. */
export interface Dimensions {
    /** Required width in 1..2147483647. */
    readonly width: number;
    /** Height in 1..2147483647, or null when unspecified. */
    readonly height: number | null;
}
