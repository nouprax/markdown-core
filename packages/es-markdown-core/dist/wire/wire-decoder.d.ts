import { Concrete } from "../concrete.js";
import type { Semantic } from "../model/semantic.js";
/** The decoded pair; the `Read` wrapper (with its non-enumerable `dump`) is
 * the runtime's to seal. */
export interface DecodedRead {
    readonly semantic: Semantic;
    readonly concrete: Concrete;
}
/** Decodes a full payload: the envelope, the tree, the concrete view. */
export declare function decodeRead(bytes: Uint8Array): DecodedRead;
/**
 * Decodes only the envelope of a payload whose read is discarded -- the
 * `Document` constructor's initial feed -- so an error still surfaces and a
 * healthy tree is not built just to be thrown away.
 */
export declare function decodeDiscarded(bytes: Uint8Array): void;
