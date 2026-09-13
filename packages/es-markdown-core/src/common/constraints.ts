/** A node-independent size. Every present component is a positive 32-bit integer. */
export interface Dimensions {
    /** Required width in 1..2147483647. */
    readonly width: number;
    /** Height in 1..2147483647, or null when unspecified. */
    readonly height: number | null;
}

/** Authored horizontal content alignment; "none" means no explicit alignment. */
export type Flow = "none" | "left" | "center" | "right";
