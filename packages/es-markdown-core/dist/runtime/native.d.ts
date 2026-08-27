export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    /** Returns the session, or 0 for an allocation failure -- the one failure
     * opening a session can report, so 0 is the whole answer and no error
     * crosses the wire. */
    es_session_new(flags: number): number;
    /** Writes the MKC6 payload after those bytes -- the envelope around the
     * facade's own wire, carrying the document or the error -- behind the two
     * output slots, and returns nonzero. Zero means the payload buffer itself
     * could not be built, which is the one failure with nothing to decode.
     * The buffer is the caller's to release with `es_wire_free`. */
    es_session_feed(session: number, chunk: number, length: number, dataOutput: number, lengthOutput: number): number;
    es_session_finish(session: number, dataOutput: number, lengthOutput: number): number;
    /** Feed whose read is DISCARDED BY CONTRACT (the constructor's initial
     * feed): no projection, no serialization -- the answer is the bare MKC6
     * envelope, or the error behind it. */
    es_session_advance(session: number, chunk: number, length: number, dataOutput: number, lengthOutput: number): number;
    es_session_free(session: number): void;
    es_wire_free(pointer: number): void;
}
export declare const native: NativeExports;
