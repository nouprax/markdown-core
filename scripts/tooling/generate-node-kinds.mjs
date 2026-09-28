#!/usr/bin/env node
/**
 * Node-kind generator.
 *
 * Two schemas define every node kind, and every table that enumerates kinds is
 * generated from them:
 *
 * - `docs/specs/canonical-ast.json` is the public contract. Each kind's
 *   `ordinal` is its wire number: the value of `markdown_core_node_kind`, the
 *   JNI kind and the ES wire kind.
 * - `packages/markdown-core/node-types.json` is the native representation: the
 *   internal `markdown_core_node_type` of each class, the public kind it
 *   reports, the payload record it allocates and the element that defines its
 *   structure. Each class is numbered in list order from 1.
 *
 * Whole files are generated where a file is nothing but the table (Kotlin and
 * ES). In C, where a table sits beside the code that reads it, one marked
 * region per file is generated and the rest stays hand-written.
 *
 *   node scripts/tooling/generate-node-kinds.mjs          rewrite the outputs
 *   node scripts/tooling/generate-node-kinds.mjs --check  fail if any is stale
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const CONTRACT = "docs/specs/canonical-ast.json";
const NATIVE = "packages/markdown-core/node-types.json";
const GENERATOR = "scripts/tooling/generate-node-kinds.mjs";

const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");

/** `HTMLBlock` -> `HTML_BLOCK`, `ThematicBreak` -> `THEMATIC_BREAK`. */
const screaming = (name) =>
    name
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
        .toUpperCase();

/** `HTMLBlock` -> `htmlBlock`, `ThematicBreak` -> `thematicBreak`. */
const camel = (name) => name.replace(/^[A-Z]+(?=[A-Z][a-z]|$)|^[A-Z]/, (head) => head.toLowerCase());

const hex = (value) => `0x${value.toString(16).padStart(4, "0")}`;

/** Validates the two schemas and joins them into one model; throws listing every error. */
export function buildModel(contract, native) {
    const errors = [];
    // A content kind is one no field names as its type, so `[Markup]` accepts
    // it; every other kind lives only in the typed fields that name it, and the
    // Document only at the root.
    const fieldKinds = new Set(
        contract.kinds.flatMap(({ fields }) => fields.map(({ type }) => type.replace(/[[\]?]/g, "")))
    );
    const kinds = [...contract.kinds]
        .map(({ name, ordinal }) => ({ name, ordinal, content: name !== "Document" && !fieldKinds.has(name) }))
        .sort((a, b) => a.ordinal - b.ordinal);
    kinds.forEach((kind, index) => {
        if (kind.ordinal !== index + 1) {
            errors.push(
                `${CONTRACT}: ordinals must run 1..${String(kinds.length)} once each; ${kind.name} is ${String(kind.ordinal)}`
            );
        }
    });
    const kindNames = new Set(kinds.map((kind) => kind.name));

    const identifier = /^[A-Za-z_][A-Za-z0-9_]*$/;
    const seen = new Set();
    const classes = ["block", "inline"].map((name) => {
        if (!Array.isArray(native[name]) || native[name].length === 0) {
            errors.push(`${NATIVE}: no ${name} types`);
            return { name, types: [] };
        }
        const types = native[name].map((type, index) => {
            const where = `${NATIVE}: ${name} type ${type.name ?? String(index)}`;
            const allowed = new Set(["name", "kind", "record", "structure", "note"]);
            for (const key of Object.keys(type)) if (!allowed.has(key)) errors.push(`${where}: unknown key ${key}`);
            for (const key of ["kind", "record", "structure"]) {
                if (!(key in type)) errors.push(`${where}: ${key} must be given, as null when there is none`);
            }
            if (!/^[A-Z][A-Z0-9_]*$/.test(type.name ?? "")) errors.push(`${where}: name must be SCREAMING_SNAKE`);
            if (seen.has(type.name)) errors.push(`${where}: named twice`);
            seen.add(type.name);
            if (type.kind !== null && !kindNames.has(type.kind)) {
                errors.push(`${where}: kind ${String(type.kind)} is not a kind of ${CONTRACT}`);
            }
            for (const key of ["record", "structure"]) {
                if (type[key] !== null && !identifier.test(type[key] ?? "")) {
                    errors.push(`${where}: ${key} must be a C identifier or null`);
                }
            }
            return { ...type, value: index + 1 };
        });
        return { name, types };
    });
    const reported = new Set(classes.flatMap(({ types }) => types.map((type) => type.kind)));
    for (const kind of kindNames) {
        if (!reported.has(kind)) errors.push(`${NATIVE}: no native type reports the public kind ${kind}`);
    }
    if (errors.length) throw new Error(errors.join("\n"));

    const [block, inline] = classes;
    return { kinds, block, inline, count: Math.max(block.types.length, inline.types.length) + 1 };
}

const BEGIN = `/* BEGIN GENERATED by ${GENERATOR}; edit the node-kind schemas instead. */`;
const END = "/* END GENERATED */";

/** Replaces the one generated region of a hand-written C file. */
function region(relative, source, body) {
    const start = source.indexOf(BEGIN);
    const end = source.indexOf(END);
    if (start < 0 || end < start || source.indexOf(BEGIN, start + 1) >= 0) {
        throw new Error(`${relative}: needs exactly one generated region`);
    }
    const indent = source.slice(source.lastIndexOf("\n", end) + 1, end);
    return source.slice(0, start + BEGIN.length) + "\n" + body + indent + source.slice(end);
}

/** A table indexed by the dense value of each type of one class. */
function classTable(declaration, klass, entry) {
    const rows = klass.types
        .map((type) => [type, entry(type)])
        .filter(([, value]) => value !== null)
        .map(([type, value]) => `    [MARKDOWN_CORE_NODE_${type.name} & MARKDOWN_CORE_NODE_VALUE_MASK] = ${value},`);
    return `${declaration}[MARKDOWN_CORE_NODE_KIND_COUNT] = {\n${rows.join("\n")}\n};\n`;
}

function comment(note, indent) {
    if (!note) return "";
    const words = note.split(/\s+/);
    const lines = [];
    let line = `${indent}/*`;
    for (const word of words) {
        if (line.length + 1 + word.length > 80) {
            lines.push(line);
            line = `${indent} *`;
        }
        line += ` ${word}`;
    }
    lines.push(`${line} */`);
    return lines.join("\n") + "\n";
}

function internalEnum({ block, inline, count }) {
    const entries = (klass, flag) =>
        klass.types
            .map(
                (type) =>
                    comment(type.note, "    ") +
                    `    MARKDOWN_CORE_NODE_${type.name} = MARKDOWN_CORE_NODE_TYPE_${flag} | ${hex(type.value)},`
            )
            .join("\n");
    return (
        "/* One past the largest value either class uses, so `kind & VALUE_MASK` is a\n" +
        " * dense index into a per-class table. */\n" +
        `#define MARKDOWN_CORE_NODE_KIND_COUNT (${hex(count)})\n\n` +
        "typedef enum {\n" +
        "    /* Error status */\n" +
        "    MARKDOWN_CORE_NODE_NONE = 0x0000,\n\n" +
        "    /* Block */\n" +
        entries(block, "BLOCK") +
        "\n\n    /* Inline */\n" +
        entries(inline, "INLINE") +
        "\n} markdown_core_node_type;\n"
    );
}

function publicEnum({ kinds }) {
    return (
        "    MARKDOWN_CORE_KIND_NONE = 0,\n" +
        kinds.map((kind) => `    MARKDOWN_CORE_KIND_${screaming(kind.name)} = ${String(kind.ordinal)},`).join("\n") +
        "\n"
    );
}

function testKindCount({ kinds }) {
    return `#define TS_KIND_COUNT (MARKDOWN_CORE_KIND_${screaming(kinds.at(-1).name)} + 1)\n`;
}

function nodeTables({ block, inline }) {
    const record = (type) => (type.record === null ? null : `sizeof(${type.record})`);
    const typeString = (type) => `"${type.name.toLowerCase()}"`;
    return (
        "/* A type without a record is absent and reads as 0: no inline record, and\n" +
        " * `as.data` left NULL. An element-owned payload comes from `opaque_alloc_func`. */\n" +
        classTable("static const size_t S_block_payload_size", block, record) +
        "\n" +
        classTable("static const size_t S_inline_payload_size", inline, record) +
        "\n" +
        classTable("static const char *const S_block_type_string", block, typeString) +
        "\n" +
        classTable("static const char *const S_inline_type_string", inline, typeString)
    );
}

function facadeTables({ kinds, block, inline }) {
    const kind = (type) => (type.kind === null ? null : `MARKDOWN_CORE_KIND_${screaming(type.kind)}`);
    const names = ["None", ...kinds.map((entry) => entry.name)].map((name) => `    "${name}",`).join("\n");
    return (
        "/* The public kind each native type reports; a private type reads as NONE. */\n" +
        classTable("static const markdown_core_node_kind S_block_kind", block, kind) +
        "\n" +
        classTable("static const markdown_core_node_kind S_inline_kind", inline, kind) +
        "\n" +
        "/* clang-format off */\n" +
        `static const char *const S_kind_name[] = {\n${names}\n};\n` +
        "/* clang-format on */\n"
    );
}

function structureTables({ block, inline }) {
    const element = (type) => (type.structure === null ? null : `&MARKDOWN_CORE_ELEMENT_${type.structure}`);
    return (
        "/* Explicitly sized, so a kind whose value index outgrows the projection is a\n" +
        " * compile error here rather than a silent NULL at every lookup. */\n" +
        classTable("const markdown_core_element *const markdown_core_block_structure", block, element) +
        "\n" +
        classTable("const markdown_core_element *const markdown_core_inline_structure", inline, element)
    );
}

function kotlin({ kinds }) {
    return (
        `// Generated by ${GENERATOR} from ${CONTRACT}; do not edit.\n` +
        "package com.nouprax.markdown.core\n\n" +
        "internal enum class JniNodeKind(\n" +
        "    val rawValue: Int,\n" +
        ") {\n" +
        kinds.map((kind) => `    ${screaming(kind.name)}(${String(kind.ordinal)}),\n`).join("") +
        "    ;\n\n" +
        "    companion object {\n" +
        "        private val byRawValue = entries.associateBy(JniNodeKind::rawValue)\n\n" +
        "        fun from(rawValue: Int): JniNodeKind =\n" +
        '            requireNotNull(byRawValue[rawValue]) { "unsupported native node kind $rawValue" }\n' +
        "    }\n" +
        "}\n"
    );
}

function es({ kinds }) {
    const names = kinds.map((kind) => camel(kind.name));
    return (
        `// Generated by ${GENERATOR} from ${CONTRACT}; do not edit.\n` +
        'import type { Markup } from "../markup/markup.js";\n\n' +
        "export type NativeKind =\n" +
        names.map((name, index) => `    | "${name}"${index === names.length - 1 ? ";" : ""}`).join("\n") +
        "\n\n" +
        "/** Indexed by wire ordinal. */\n" +
        'export const kinds: readonly (NativeKind | "none")[] = Object.freeze([\n' +
        ['    "none"', ...names.map((name) => `    "${name}"`)].join(",\n") +
        "\n]);\n\n" +
        "type Exactly<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;\n" +
        "type Holds<Claim extends true> = Claim;\n\n" +
        "/** Fails to compile unless the wire kinds are exactly the Markup union's kinds, so every\n" +
        " * switch or mapped type exhaustive over one is exhaustive over the other. */\n" +
        'export type NativeKindsAreMarkupKinds = Holds<Exactly<NativeKind, Markup["kind"]>>;\n\n' +
        "/** The kinds a `[Markup]` field accepts: every kind no field of the contract names as its type. */\n" +
        "export const contentKinds: ReadonlySet<NativeKind> = new Set([\n" +
        kinds
            .filter((kind) => kind.content)
            .map((kind) => `    "${camel(kind.name)}"`)
            .join(",\n") +
        "\n]);\n"
    );
}

const PACKAGE = "packages/markdown-core";
/** Every generated file's full text, reading the hand-written parts with `readSource`. */
export function outputs(model, readSource) {
    const edit = (relative, body) => [relative, region(relative, readSource(relative), body)];
    return new Map([
        edit(`${PACKAGE}/core/node_type.h`, internalEnum(model)),
        edit(`${PACKAGE}/include/markdown_core.h`, publicEnum(model)),
        edit(`${PACKAGE}/core/node.c`, nodeTables(model)),
        edit(`${PACKAGE}/elements/ast.c`, facadeTables(model)),
        edit(`${PACKAGE}/elements/core-elements.c`, structureTables(model)),
        edit(`${PACKAGE}/tests/support/test_support.h`, testKindCount(model)),
        [
            "packages/kotlin-markdown-core/src/jniMain/kotlin/com/nouprax/markdown/core/wire/JniNodeKind.kt",
            kotlin(model)
        ],
        ["packages/es-markdown-core/src/wire/kinds.ts", es(model)]
    ]);
}

function main() {
    const check = process.argv.includes("--check");
    let model;
    try {
        model = buildModel(JSON.parse(read(CONTRACT)), JSON.parse(read(NATIVE)));
    } catch (error) {
        console.error(error.message);
        process.exit(1);
    }
    const stale = [];
    for (const [relative, text] of outputs(model, read)) {
        const absolute = path.join(root, relative);
        const current = fs.existsSync(absolute) ? fs.readFileSync(absolute, "utf8") : null;
        if (current === text) continue;
        if (check) stale.push(relative);
        else fs.writeFileSync(absolute, text);
    }
    if (stale.length) {
        console.error(`Stale node-kind output; run node ${GENERATOR}:\n${stale.map((file) => `  ${file}`).join("\n")}`);
        process.exit(1);
    }
    console.log(
        check
            ? `Node-kind outputs are current: ${String(model.kinds.length)} public kinds, ` +
                  `${String(model.block.types.length + model.inline.types.length)} native types.`
            : "Node-kind outputs written."
    );
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) main();
