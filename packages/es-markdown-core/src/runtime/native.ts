export interface NativeExports extends WebAssembly.Exports {
    readonly memory: WebAssembly.Memory;
    malloc(size: number): number;
    free(pointer: number): void;
    /** Parses the one dialect and returns one owned MCB1 result, or zero only
     * when the result itself cannot be allocated. Parse failures are typed
     * payloads. */
    es_parse(source: number, length: number): number;
    es_result_free(result: number): void;
}

const wasmURL = new URL("../markdown-core.wasm", import.meta.url);

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
const imports = { wasi_snapshot_preview1: wasi, env };

async function loadWasm(): Promise<WebAssembly.Instance> {
    if (wasmURL.protocol === "file:") {
        const nodeFileSystem = "node:fs/promises";
        const fileSystem = (await import(nodeFileSystem)) as {
            readFile(url: URL): Promise<BufferSource>;
        };
        // The bytes as read are the module: a Uint8Array is a BufferSource,
        // so nothing is copied element by element into another.
        return (await WebAssembly.instantiate(await fileSystem.readFile(wasmURL), imports)).instance;
    }
    const response = await fetch(wasmURL);
    if (!response.ok) throw new Error(`failed to load Markdown Core WASM: ${response.status}`);
    // Compile while the bytes arrive, and let the engine cache the compiled
    // module, where the host serves it as `application/wasm`; a host that
    // labels it otherwise refuses the stream, and the same bytes then
    // instantiate from the buffered clone.
    if (typeof WebAssembly.instantiateStreaming === "function") {
        const buffered = response.clone();
        try {
            return (await WebAssembly.instantiateStreaming(response, imports)).instance;
        } catch {
            return (await WebAssembly.instantiate(await buffered.arrayBuffer(), imports)).instance;
        }
    }
    return (await WebAssembly.instantiate(await response.arrayBuffer(), imports)).instance;
}

// Top-level initialization keeps Document.parse synchronous in Node and browsers.
const instance = await loadWasm();
export const native = instance.exports as NativeExports;
