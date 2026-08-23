/** What a region's bytes are to their owner. */
export const RegionRole = {
    /** Syntax the owner is made of: a fence, a bullet, a heading's `#`s. */
    marker: "marker",
    /** Bytes the owner's meaning is made of. */
    content: "content",
    /** Bytes the owner consumed and kept nothing of. */
    discarded: "discarded"
} as const;

export type RegionRole = (typeof RegionRole)[keyof typeof RegionRole];

/**
 * One region of the concrete view: a byte range of the normalized source with
 * exactly one owner and exactly one role.
 *
 * `start` and `length` index `Concrete.source`, which is BYTES and not a
 * string: the parser counts positions in bytes, and a string index disagrees
 * with it on the first character outside ASCII.
 */
export interface Region {
    readonly start: number;
    readonly length: number;
    readonly role: RegionRole;
    /**
     * The owner, as the path of child indices from the semantic root: `[]` is
     * the root and `[0, 2]` is the third child of the first.
     *
     * A pointer names a node only while the WASM handle is alive, and these
     * values outlive it, so the path is the locator rather than the node.
     */
    readonly owner: readonly number[];
}

/**
 * The concrete view of a parse: the normalized source, its line index, and
 * every region of it.
 *
 * Total, and that is the point of the pair: every byte of `source` lies in
 * exactly one region and every region has exactly one owner in the semantic
 * tree, so nothing the parser read is reachable through neither view.
 *
 * The regions are held as parallel arrays and a `Region` is built when it is
 * asked for. Measured on this repository's own design document -- one region
 * per 17 bytes of prose -- an object per region costs several times the source
 * it describes, and the arrays cost about 25 bytes each.
 */
export class Concrete {
    /**
     * The NORMALIZED source: UTF-8 validated, NUL replaced, every line ending
     * a single `\n`. Not the bytes the caller passed in.
     */
    readonly source: Uint8Array;

    readonly #lineStarts: Uint32Array;
    readonly #regions: Uint32Array;
    readonly #ownerPaths: Int32Array;
    readonly #ownerOffsets: Uint32Array;

    constructor(
        source: Uint8Array,
        lineStarts: Uint32Array,
        regions: Uint32Array,
        ownerPaths: Int32Array,
        ownerOffsets: Uint32Array
    ) {
        this.source = source;
        this.#lineStarts = lineStarts;
        this.#regions = regions;
        this.#ownerPaths = ownerPaths;
        this.#ownerOffsets = ownerOffsets;
    }

    /** How many lines the normalized source has. */
    get lineCount(): number {
        return this.#lineStarts.length;
    }

    /** Where `line` begins in `source`, counting lines from 1. */
    lineStart(line: number): number {
        if (!Number.isInteger(line) || line < 1 || line > this.#lineStarts.length) {
            throw new RangeError(`no line ${line}`);
        }
        return this.#lineStarts[line - 1]!;
    }

    /** How many regions the view has. They are in source order. */
    get regionCount(): number {
        return this.#ownerOffsets.length - 1;
    }

    /** The region at `index`, counting from 0. */
    region(index: number): Region {
        if (!Number.isInteger(index) || index < 0 || index >= this.regionCount) {
            throw new RangeError(`no region ${index}`);
        }
        return {
            start: this.#regions[index * 3]!,
            length: this.#regions[index * 3 + 1]!,
            role: ROLES[this.#regions[index * 3 + 2]!]!,
            owner: Array.from(this.#ownerPaths.subarray(this.#ownerOffsets[index], this.#ownerOffsets[index + 1]))
        };
    }
}

/** The native role, in the order `markdown_core_region_role` declares it. */
const ROLES: readonly RegionRole[] = [RegionRole.marker, RegionRole.content, RegionRole.discarded];
