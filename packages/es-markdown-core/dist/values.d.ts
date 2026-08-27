export interface Position {
    readonly line: number;
    readonly column: number;
}
export interface Scope {
    readonly start: Position;
    readonly end: Position;
}
/**
 * A node's identity: the name a consumer tracks an element by across a
 * stream's feeds -- the render key. `block` is the owning block's
 * document-unique mint -- the block is the minimal update unit, so it alone
 * names the region an incremental consumer re-renders -- and `ordinal` is the
 * node's pre-order ordinal among that block's inline descendants, `0` for the
 * block itself. The pair is unique within one document and never reused
 * within a parse; it is not stable across documents. The halves are opaque
 * values: compare them, key maps by them, and derive nothing else from them.
 */
export interface Identity {
    readonly block: number;
    readonly ordinal: number;
}
export type ListFlavor = "bullet" | "ordered";
/** Which of the three reference spellings the source wrote. */
export type ReferenceForm = "full" | "collapsed" | "shortcut";
export type PlacementMode = "embedded" | "standalone";
export type TableAlignment = "none" | "left" | "center" | "right";
