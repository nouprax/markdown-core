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
    "concrete_records.c",
    "iterator.c",
    "blocks.c",
    "inlines.c",
    "delimiter.c",
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
    "ast.c",
    "document.c",
    "arena.c",
    "source.c",
    "concrete.c",
    "diff.c",
    "core-extensions.c",
    "table.c",
    "strikethrough.c",
    "autolink.c",
    "formula.c",
    "directive.c",
    "cross_reference.c",
    "ext_scanners.c",
    "tasklist.c"
].map((file) => path.join(root, "packages/markdown-core/extensions", file));

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });
const output = path.join(dist, "markdown-core.wasm");
const exported = [
    "malloc",
    "free",
    "es_document_open",
    "es_document_edit",
    "es_document_free",
    "es_document_series",
    "es_document_diagnostics",
    "es_document_root",
    "es_node_id",
    "es_node_revision",
    "es_node_html_comment",
    "es_error_code",
    "es_error_free",
    "es_node_kind",
    "es_node_first_child",
    "es_node_next_sibling",
    "es_node_scope",
    "es_node_heading_level",
    "es_node_list_properties",
    "es_node_checked",
    "es_node_code_properties",
    "es_node_formula_mode",
    "es_node_table_column_count",
    "es_node_table_alignment",
    "es_node_table_row_header",
    "es_node_directive_properties",
    "es_node_directive_attribute_at",
    "es_string",
    "es_node_reference_form"
].map((name) => `_${name}`);
/** A spawnSync result with `error` set means the tool never launched
 * (typically ENOENT) and `stdout`/`stderr` are undefined — writing them
 * would throw and mask the real failure. */
function requireLaunched(result, tool, remedy) {
    if (!result.error) return;
    process.stderr.write(`failed to launch ${tool}: ${result.error.message}\n${remedy}\n`);
    process.exit(1);
}

const result = spawnSync(
    "emcc",
    [
        ...core,
        ...extensions,
        path.join(packageDirectory, "src/bridge.c"),
        "-O3",
        "-std=c99",
        "-sSTANDALONE_WASM=1",
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
requireLaunched(
    result,
    "emcc",
    "Install it with scripts/init-environment.sh --install emscripten, then activate it in this shell " +
        "with: source .tools/emsdk/4.0.23/emsdk_env.sh"
);
if (result.status !== 0) {
    process.stderr.write(result.stdout);
    process.stderr.write(result.stderr);
    process.exit(result.status ?? 1);
}
const typescript = spawnSync(
    path.join(root, "node_modules/.bin/tsc"),
    ["-p", path.join(packageDirectory, "tsconfig.json")],
    { cwd: root, encoding: "utf8" }
);
requireLaunched(
    typescript,
    "tsc",
    "Install JavaScript dependencies with scripts/init-environment.sh --install (or pnpm install)."
);
if (typescript.status !== 0) {
    process.stderr.write(typescript.stdout);
    process.stderr.write(typescript.stderr);
    process.exit(typescript.status ?? 1);
}
