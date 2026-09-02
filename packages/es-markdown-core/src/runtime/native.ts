export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    /** Parses and returns one owned MCB1 result, or zero only when the
     * result itself cannot be allocated. Parse failures are typed payloads. */
    es_parse(source: number, length: number, flags: number): number;
    es_result_free(result: number): void;
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
