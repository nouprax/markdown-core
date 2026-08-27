import { mkdir, rm } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const root = path.resolve(packageDirectory, "../..");
const dist = path.join(packageDirectory, "dist");
const core = [
    "markdown_core.c",
    "node.c",
    "iterator.c",
    "blocks.c",
    "inlines.c",
    "scanners.c",
    "utf8.c",
    "buffer.c",
    "references.c",
    "map.c",
    "houdini_html_u.c",
    "markdown_core_ctype.c",
    "linked_list.c"
].map((file) => path.join(root, "packages/markdown-core/core", file));
const extensions = [
    "core-extensions.c",
    "ast.c",
    "table.c",
    "strikethrough.c",
    "autolink.c",
    "formula.c",
    "directive.c",
    "ext_scanners.c",
    "tasklist.c"
].map((file) => path.join(root, "packages/markdown-core/extensions", file));

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });
const output = path.join(dist, "markdown-core.wasm");
const exported = [
    "malloc",
    "free",
    "es_session_new",
    "es_session_feed",
    "es_session_finish",
    "es_session_advance",
    "es_session_free",
    "es_wire_free"
].map((name) => `_${name}`);
const result = spawnSync(
    "emcc",
    [
        ...core,
        ...extensions,
        path.join(packageDirectory, "src/bridge.c"),
        "-O3",
        "-std=c99",
        "-sSTANDALONE_WASM=1",
        // A FIXED HEAP ONLY MOVES THE CLIFF. Without this the heap is
        // pinned at the 16 MiB default and a document over about 1.6 MiB
        // does not fail -- it stops returning. Reserving more just picks a
        // larger input to fail on, and a long stream reaches any bound.
        // The loader supplies `emscripten_notify_memory_growth`, which a
        // standalone module with a growing heap will not instantiate
        // without.
        "-sALLOW_MEMORY_GROWTH=1",
        "--no-entry",
        `-sEXPORTED_FUNCTIONS=${JSON.stringify(exported)}`,
        "-DMARKDOWN_CORE_STATIC_DEFINE",
        "-DMARKDOWN_CORE_EXTENSIONS_STATIC_DEFINE",
        `-I${path.join(root, "packages/markdown-core/core")}`,
        `-I${path.join(root, "packages/markdown-core/extensions")}`,
        `-I${path.join(root, "packages/markdown-core/include")}`,
        `-I${path.join(root, "packages/markdown-core/core/include")}`,
        "-o",
        output
    ],
    {
        cwd: root,
        encoding: "utf8",
        env: { ...process.env, EM_CACHE: path.join(root, "build/emscripten-cache") }
    }
);
if (result.status !== 0) {
    // A spawn that never started has no status and no output; without this the
    // failure prints as an ERR_INVALID_ARG_TYPE from the stream, which names
    // the wrong thing entirely. The usual cause is emcc not being on PATH.
    if (result.error) process.stderr.write(`emcc could not be run: ${String(result.error)}\n`);
    process.stderr.write(result.stdout ?? "");
    process.stderr.write(result.stderr ?? "");
    process.exit(result.status ?? 1);
}
const typescript = spawnSync(
    path.join(root, "node_modules/.bin/tsc"),
    ["-p", path.join(packageDirectory, "tsconfig.json")],
    { cwd: root, encoding: "utf8" }
);
if (typescript.status !== 0) {
    process.stderr.write(typescript.stdout);
    process.stderr.write(typescript.stderr);
    process.exit(typescript.status ?? 1);
}
