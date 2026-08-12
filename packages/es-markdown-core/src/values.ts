/** A one-based line/column source coordinate. */
export interface Position {
    readonly line: number;
    readonly column: number;
}

/**
 * A node's absolute source extent: start and end coordinates, both inclusive
 * of the construct's own markers.
 *
 * `end` names the last coordinate the construct occupies, not the one after
 * it.
 */
export interface Scope {
    readonly start: Position;
    readonly end: Position;
}

export type ListFlavor = "bullet" | "ordered";
/**
 * Whether a construct is embedded in inline content or stands alone as its
 * own block.
 *
 * Placement is not containment: a {@link Formula} may be `standalone` while sitting
 * inside a paragraph, so a consumer reads the mode rather than inferring it
 * from where the node sits in the tree.
 */
export type PlacementMode = "embedded" | "standalone";
/** A table column's alignment as its delimiter row declared it with `:`
 * markers; `none` is a column whose delimiter cell carried neither. */
export type TableAlignment = "none" | "left" | "center" | "right";
