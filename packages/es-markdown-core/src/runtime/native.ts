export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    /** Returns the session, or 0 for an allocation failure -- the one failure
     * opening a session can report, so 0 is the whole answer and no error
     * crosses the wire. */
    es_session_new(flags: number): number;
    /** Writes the MKC8 payload after those bytes -- the envelope around the
     * facade's own wire, carrying the document or the error -- behind the two
     * output slots, and returns nonzero. Zero means the payload buffer itself
     * could not be built, which is the one failure with nothing to decode.
     * `request` asks for the frame (#162): 1 for a DELTA against the last
     * payload this session wrote, anything else for FULL; the facade answers
     * FULL whenever it cannot honor a delta. The buffer is the caller's to
     * release with `es_wire_free`. */
    es_session_feed(
        session: number,
        chunk: number,
        length: number,
        request: number,
        dataOutput: number,
        lengthOutput: number
    ): number;
    /** Seals the stream and writes the sealed document's payload the same
     * way, in the frame `request` asks for; the session is ended either
     * way. */
    es_session_finish(session: number, request: number, dataOutput: number, lengthOutput: number): number;
    /** Feed whose read is DISCARDED BY CONTRACT (the constructor's initial
     * feed): no projection, no serialization -- the answer is the bare MKC8
     * envelope, or the error behind it. */
    es_session_advance(
        session: number,
        chunk: number,
        length: number,
        dataOutput: number,
        lengthOutput: number
    ): number;
    es_session_free(session: number): void;
    es_wire_free(pointer: number): void;
}

const wasmURL = new URL("../markdown-core.wasm", import.meta.url);

async function loadWasm(): Promise<WebAssembly.Instance> {
    let bytes: BufferSource;
    if (wasmURL.protocol === "file:") {
        const nodeFileSystem = "node:fs/promises";
        const fileSystem = (await import(nodeFileSystem)) as {
            readFile(url: URL): Promise<Uint8Array>;
        };
        bytes = Uint8Array.from(await fileSystem.readFile(wasmURL)).buffer;
    } else {
        const response = await fetch(wasmURL);
        if (!response.ok) throw new Error(`failed to load Markdown Core WASM: ${response.status}`);
        bytes = await response.arrayBuffer();
    }
    const wasi = {
        fd_close: (): number => 0,
        fd_seek: (): number => 0,
        fd_write: (): number => 0,
        proc_exit: (code: number): never => {
            throw new Error(`Markdown Core WASM exited with status ${code}`);
        }
    };
    // A standalone module with a growing heap does not instantiate without this
    // import. It is where a host would refresh cached views of `memory.buffer`,
    // which growing DETACHES -- this runtime caches none, so it has nothing to
    // do. Anything that comes to hold a view across a call into WASM breaks
    // under growth and passes without it.
    const env = { emscripten_notify_memory_growth: (): void => {} };
    return (await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi, env })).instance;
}

// Top-level initialization keeps the Document constructor, feed, and seal
// synchronous in Node and browsers.
const instance = await loadWasm();
export const native = instance.exports as NativeExports;
