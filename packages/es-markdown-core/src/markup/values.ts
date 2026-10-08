import type { Footnote } from "./footnote.js";

/** How a document counts columns: UTF-8 bytes or UTF-16 code units. Storage
 * stays UTF-8 either way; the unit only counts what a scope query takes and
 * returns. */
export type TextUnit = "utf8" | "utf16";

/** Editor coordinates: a line counted from 1, and a column counted from 1 in
 * the document's text unit. Not JavaScript string indices. */
export interface Position {
    readonly line: number;
    readonly column: number;
}

/**
 * Editor coordinates of one source range of a node, computed on request from
 * the runs and the source (`Document.scope`). `start` is the
 * position of the range's first byte, where a line terminator is the column
 * after its line's last character. `end` is the line holding the byte just
 * past the range's last byte and the column count from that line's start to
 * it, so a range that ends right after a line terminator ends at `L:0` of the
 * next line, and a zero-byte document is `1:1..1:0`. Not a substring or
 * half-open range.
 */
export interface Scope {
    readonly start: Position;
    readonly end: Position;
}

/**
 * Where a node is, in bytes of the input of the parser that produced it: the
 * UTF-8 source for a block, and its inline root's content, which starts at 0,
 * for an inline node. `lead` is the signed distance from the end of the
 * previous node in the same relation -- or from the owner's start, for the
 * first node of a relation -- to this node's start, and `span` the length of
 * its range. Neither changes when text before the node shifts; scopes are
 * computed from runs and the source on request.
 */
export interface Extent {
    readonly lead: number;
    readonly span: number;
}

/**
 * A run of a node's own source: a source range whose `lead` is from the end
 * of the run before, or, for the first run, from the end of the source of the
 * previous node in the same relation (or from the start of the owner's
 * source, for a relation's first node), and whose `span` is its length in
 * bytes. A node's source starts where its first run starts and ends where its
 * last ends; between them the runs cover exactly the node's own source, so the
 * source between two runs is not the node's. Every node has at least one run.
 */
export interface Run {
    readonly lead: number;
    readonly span: number;
}

export type ListFlavor = "bullet" | "ordered";
export type OrderedListVariant =
    | "decimal"
    | { readonly kind: "alpha"; readonly lowercased: boolean }
    | { readonly kind: "roman"; readonly lowercased: boolean }
    | "default";
export type OrderedListDelimiter = "period" | { readonly kind: "parenthesis"; readonly closed: boolean } | "default";
/**
 * The target of a `Link`, `Embedded` or `Reference`: a tagged value, not a
 * node, so it has no id, no extent and no children, and a branch's fields
 * exist only in that branch. A direct link or image and every `Reference` own
 * the `url` branch, the complete semantic destination the inherited grammar
 * produced -- decoded, not percent-encoded, normalized, or resolved, and
 * possibly empty. A reference link or image owns the `reference` branch, the
 * normalized label it names: the first `Reference` in document source order
 * whose label is equal, or, when none is, the first `Heading` in document
 * source order whose text declares it (`Document.reference`). The `cross`
 * branch is the workspace address a cross link produces.
 */
export type Destination =
    | { readonly kind: "url"; readonly value: string }
    | { readonly kind: "cross"; readonly path: string; readonly anchor: string | null }
    | { readonly kind: "reference"; readonly label: string };
export type Placement = "embedded" | "standalone";
/**
 * How a bibliographic citation is rendered: `[@key]` is `normal`, `@key` in
 * running text names the author in text, and `-@key` suppresses the author.
 */
export type BibMode = "normal" | "authorInText" | "suppressAuthor";
/**
 * What a `Citation` names: a tagged value, not a node, so it has no id or
 * extent, and a branch's fields exist only in that branch. The `bib` branch is
 * produced by bibliography citations; every referenced or inline footnote call
 * produces the `footnote` branch. The `specimen` branch names the first
 * `Specimen` in document source order whose label is equal
 * (`Document.specimen`).
 */
export type CitationReferent =
    | { readonly kind: "bib"; readonly key: string; readonly mode: BibMode }
    | { readonly kind: "footnote"; readonly target: FootnoteTarget }
    | { readonly kind: "specimen"; readonly label: string };

/**
 * The footnote a footnote referent names: a label, which names the first
 * `Footnote` in document source order whose label is equal
 * (`Document.footnote`), or an inline note's `Footnote`, which the referent
 * owns.
 */
export type FootnoteTarget =
    { readonly kind: "label"; readonly value: string } | { readonly kind: "note"; readonly footnote: Footnote };
