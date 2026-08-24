export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    es_document_parse(source: number, length: number, flags: number, errorOutput: number): number;
    es_document_free(document: number): void;
    es_document_root(document: number): number;
    es_document_source(document: number, dataOutput: number, lengthOutput: number): void;
    es_document_line_count(document: number): number;
    es_document_line_starts(document: number, output: number): void;
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
    es_node_reference_form(node: number): number;
    es_node_table_column_count(node: number): number;
    es_node_table_alignment(node: number, index: number): number;
    es_node_table_row_header(node: number): number;
    es_node_directive_mode(node: number): number;
    es_node_directive_attribute_count(node: number): number;
    es_set_attribute_index(index: number): void;
    /** Returns whether the field was PRESENT. A `false` answer means the source
     * did not write it; a `true` answer with length 0 means it wrote it and it
     * was empty, and the two are different facts (requirement 14). */
    es_string(object: number, field: number, dataOutput: number, lengthOutput: number): number;
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

// Top-level initialization keeps Document.parse synchronous in Node and browsers.
const instance = await loadWasm();
export const native = instance.exports as NativeExports;
