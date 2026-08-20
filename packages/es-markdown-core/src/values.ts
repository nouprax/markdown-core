export interface Position {
    readonly line: number;
    readonly column: number;
}

export interface Scope {
    readonly start: Position;
    readonly end: Position;
}

export type ListFlavor = "bullet" | "ordered";
export type PlacementMode = "embedded" | "standalone";
export type TableAlignment = "none" | "left" | "center" | "right";
