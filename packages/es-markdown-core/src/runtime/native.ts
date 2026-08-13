export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    /** Parses `bytes` and returns the document, or 0 with `*errorOutput`
     * set. */
    es_document_open(bytes: number, length: number, flags: number, errorOutput: number): number;
    /** Hands `document` new text and takes nothing from it; returns the
     * caller-owned successor document, or 0 with `*errorOutput` set. */
    es_document_edit(document: number, bytes: number, length: number, errorOutput: number): number;
    es_document_free(document: number): void;
    es_document_series(document: number): bigint;
    /** Writes a pointer to the document's own diagnostic array — 20-byte
     * (code i32, scope 4×i32) rows that borrow from the document — and
     * returns the row count. */
    es_document_diagnostics(document: number, dataOutput: number): number;
    es_document_root(document: number): number;
    es_node_id(node: number): bigint;
    es_node_revision(node: number): bigint;
    /** Writes the node's own absolute extent to `output` as four i32
     * coordinates: start line, start column, end line, end column. */
    es_node_scope(node: number, output: number): void;
    es_node_html_comment(node: number): number;
    es_error_code(error: number): number;
    es_error_free(error: number): void;
    es_node_kind(node: number): number;
    es_node_first_child(node: number): number;
    es_node_next_sibling(node: number): number;
    es_node_heading_level(node: number): number;
    es_node_reference_form(node: number): number;
    /** Writes i32 flavor, i32 tight, i32 has-start, i32 padding, i64 start
     * to `output` in one crossing. */
    es_node_list_properties(node: number, output: number): void;
    es_node_checked(node: number): number;
    /** Writes u32 info/language/literal (data, length) view pairs plus i32
     * fenced and i32 closed to `output` in one crossing. */
    es_node_code_properties(node: number, output: number): void;
    es_node_formula_mode(node: number): number;
    es_node_table_column_count(node: number): number;
    es_node_table_alignment(node: number, index: number): number;
    es_node_table_row_header(node: number): number;
    /** Writes i32 mode, u32 has-attributes, the name's (data, length) view
     * pair, and the u32 attribute count to `output` in one crossing. Label
     * topology is carried exclusively by canonical child records. */
    es_node_directive_properties(node: number, output: number): void;
    /** Writes one attribute pair to `output` as u32 key data, u32 key length,
     * u32 value data, u32 value length; 0 when the index is out of range. */
    es_node_directive_attribute_at(node: number, index: number, output: number): number;
    es_string(object: number, field: number, dataOutput: number, lengthOutput: number): void;
}

const wasmURL = new URL("../markdown-core.wasm", import.meta.url);

async function loadWasm(): Promise<WebAssembly.Instance> {
    let bytes: BufferSource;
    if (wasmURL.protocol === "file:") {
        const nodeFileSystem = "node:fs/promises";
        const fileSystem = (await import(nodeFileSystem)) as {
            readFile(url: URL): Promise<Uint8Array<ArrayBuffer>>;
        };
        // readFile returns an exact-size Uint8Array, itself a valid
        // BufferSource; wrapping it again would copy the whole binary.
        bytes = await fileSystem.readFile(wasmURL);
    } else {
        const response = await fetch(wasmURL);
        if (!response.ok) throw new Error(`failed to load Markdown Core WASM: ${response.status}`);
        bytes = await response.arrayBuffer();
    }
    const memoryHolder: { memory?: WebAssembly.Memory } = {};
    // Supplies time (u64 nanoseconds at timePtr) for the engine's
    // per-series identity entropy; any clock id gets the same source, and
    // nothing else in the engine consumes time. Each call advances the
    // reported value a full second past the previous call: the entropy mix
    // survives libc only at seconds granularity, and a freed-and-reallocated
    // document at the same address within the same wall-clock second would
    // otherwise mint the same series. The clock is only the fallback layer
    // of the seed; random_get below carries the cross-runtime uniqueness
    // contract.
    let lastNanoseconds = 0n;
    const wasi = {
        clock_time_get: (_clockId: number, _precision: bigint, timePtr: number): number => {
            const memory = memoryHolder.memory;
            if (!memory) return 28; // WASI EINVAL; unreachable in practice
            const now = BigInt(Math.round((performance.timeOrigin + performance.now()) * 1e6));
            lastNanoseconds = now > lastNanoseconds + 1_000_000_000n ? now : lastNanoseconds + 1_000_000_000n;
            new DataView(memory.buffer).setBigUint64(timePtr, lastNanoseconds, true);
            return 0;
        },
        // Backs the engine's getentropy: a series salt must stay
        // collision-resistant across isolated runtimes (workers, processes),
        // where every deterministic input — allocator state, coarse clocks —
        // repeats exactly.
        random_get: (bufferPointer: number, bufferLength: number): number => {
            const memory = memoryHolder.memory;
            if (!memory) return 28; // WASI EINVAL; unreachable in practice
            crypto.getRandomValues(new Uint8Array(memory.buffer, bufferPointer, bufferLength));
            return 0;
        },
        fd_close: (): number => 0,
        fd_seek: (): number => 0,
        fd_write: (): number => 0,
        proc_exit: (code: number): never => {
            throw new Error(`Markdown Core WASM exited with status ${code}`);
        }
    };
    const env = {
        // Growth support: nothing to refresh — every Uint8Array over wasm
        // memory is created at its use site, and the decoder's cached
        // DataView revalidates against `memory.buffer` identity on every
        // access, so a replaced buffer is picked up automatically.
        emscripten_notify_memory_growth: (): void => {}
    };
    const instance = (await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi, env })).instance;
    memoryHolder.memory = instance.exports["memory"] as WebAssembly.Memory;
    return instance;
}

// Top-level initialization keeps Document() synchronous in Node and browsers.
const instance = await loadWasm();
export const native = instance.exports as NativeExports;
