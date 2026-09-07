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
    "decimal" | "lowerAlpha" | "upperAlpha" | "lowerRoman" | "upperRoman" | "example" | "default";
export type OrderedListDelimiter = "period" | "oneParen" | "twoParens" | "default";
/**
 * The target of a `Link` or `Image`: a tagged value, not a node, so it has no
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
    | { readonly kind: "footnote"; readonly id: string };
export type TableAlignment = "none" | "left" | "center" | "right";
