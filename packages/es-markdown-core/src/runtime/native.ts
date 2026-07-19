export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    es_session_open(flags: number, errorOutput: number): number;
    es_session_free(session: number): void;
    es_session_edit(
        session: number,
        byteStart: number,
        byteEnd: number,
        bytes: number,
        length: number,
        errorOutput: number
    ): number;
    es_session_commit(session: number, changesOutput: number, errorOutput: number): number;
    es_session_document(session: number): number;
    es_session_revision(session: number): bigint;
    es_session_lineage(session: number): bigint;
    es_session_length(session: number): number;
    es_session_footnote_info(session: number, id: bigint, fieldsOutput: number): number;
    es_session_footnotes(session: number, dataOutput: number): number;
    es_session_footnote_references(session: number, definition: bigint, dataOutput: number): number;
    es_delta_revision(delta: number, boundary: number): bigint;
    es_delta_ids(delta: number, verdict: number, dataOutput: number): number;
    es_delta_free(delta: number): void;
    es_document_root(document: number): number;
    es_node_id(node: number): bigint;
    es_node_revision(node: number): bigint;
    es_error_code(error: number): number;
    es_error_free(error: number): void;
    es_node_kind(node: number): number;
    es_node_first_child(node: number): number;
    es_node_next_sibling(node: number): number;
    es_scope_coordinate(node: number, field: number): number;
    es_node_heading_level(node: number): number;
    es_node_list_flavor(node: number): number;
    es_node_list_tight(node: number): number;
    es_node_list_start_state(node: number, output: number): number;
    es_node_checked(node: number): number;
    es_node_code_flag(node: number, field: number): number;
    es_node_formula_mode(node: number): number;
    es_node_table_column_count(node: number): number;
    es_node_table_alignment(node: number, index: number): number;
    es_node_table_row_header(node: number): number;
    es_node_directive_mode(node: number): number;
    es_node_directive_label_count(node: number): number;
    es_string(object: number, field: number, dataOutput: number, lengthOutput: number): void;
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
    const memoryHolder: { memory?: WebAssembly.Memory } = {};
    // Supplies time (u64 nanoseconds at timePtr) for the engine's
    // per-session identity entropy; any clock id gets the same source, and
    // nothing else in the engine consumes time. Each call advances the
    // reported value a full second past the previous call: the entropy mix
    // survives libc only at seconds granularity, and a freed-and-reallocated
    // session at the same address within the same wall-clock second would
    // otherwise mint the same lineage.
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
        fd_close: (): number => 0,
        fd_seek: (): number => 0,
        fd_write: (): number => 0,
        proc_exit: (code: number): never => {
            throw new Error(`Markdown Core WASM exited with status ${code}`);
        }
    };
    const env = {
        // Growth support: nothing to refresh — every DataView / Uint8Array
        // over wasm memory is created at its use site, never cached across
        // calls, so a replaced buffer is picked up automatically.
        emscripten_notify_memory_growth: (): void => {}
    };
    const instance = (await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi, env })).instance;
    memoryHolder.memory = instance.exports["memory"] as WebAssembly.Memory;
    return instance;
}

// Top-level initialization keeps Document.parse synchronous in Node and browsers.
const instance = await loadWasm();
export const native = instance.exports as NativeExports;
