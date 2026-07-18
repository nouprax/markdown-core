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
    "linked_list.c",
    "text.c"
].map((file) => path.join(root, "packages/markdown-core/core", file));
const extensions = [
    "core-extensions.c",
    "ast.c",
    "session.c",
    "adopt.c",
    "incremental.c",
    "lookups.c",
    "footnote.c",
    "delta.c",
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
    "es_document_parse",
    "es_document_free",
    "es_document_root",
    "es_error_code",
    "es_error_free",
    "es_node_kind",
    "es_node_first_child",
    "es_node_next_sibling",
    "es_scope_coordinate",
    "es_node_heading_level",
    "es_node_list_flavor",
    "es_node_list_tight",
    "es_node_list_start_state",
    "es_node_checked",
    "es_node_code_flag",
    "es_node_formula_mode",
    "es_node_table_column_count",
    "es_node_table_alignment",
    "es_node_table_row_header",
    "es_node_directive_mode",
    "es_node_directive_label_count",
    "es_string"
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
    process.stderr.write(result.stdout);
    process.stderr.write(result.stderr);
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
