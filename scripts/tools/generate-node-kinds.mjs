#!/usr/bin/env node
/**
 * Node-kind generator.
 *
 * Two schemas define every node kind, and every table that enumerates kinds is
 * generated from them:
 *
 * - `docs/specs/canonical-ast.json` is the public contract. Each kind's
 *   `ordinal` is its wire number: the value of `markdown_core_node_kind`, the
 *   MCB3 kind ordinal every binding decoder reads (Kotlin and ES).
 * - `packages/markdown-core/node-types.json` is the native representation: the
 *   internal `markdown_core_node_type` of each class, the public kind it
 *   reports, the ops of the record it holds, the element that defines its
 *   structure and what its nodes may hold. Each class is numbered in list
 *   order from 1.
 *
 * Whole files are generated where a file is nothing but the table (Kotlin and
 * ES). In C, where a table sits beside the code that reads it, marked regions
 * are generated, one per table and each named for it, and the rest stays
 * hand-written.
 *
 *   node scripts/tools/generate-node-kinds.mjs          rewrite the outputs
 *   node scripts/tools/generate-node-kinds.mjs --check  fail if any is stale
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const CONTRACT = "docs/specs/canonical-ast.json";
const NATIVE = "packages/markdown-core/node-types.json";
const GENERATOR = "scripts/tools/generate-node-kinds.mjs";

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
    const kinds = [...contract.kinds]
        .map(({ name, ordinal }) => ({ name, ordinal }))
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
            const allowed = new Set(["name", "kind", "ops", "structure", "contain", "note"]);
            for (const key of Object.keys(type)) if (!allowed.has(key)) errors.push(`${where}: unknown key ${key}`);
            for (const key of ["kind", "ops", "structure", "contain"]) {
                if (!(key in type)) errors.push(`${where}: ${key} must be given, as null when there is none`);
            }
            if (!/^[A-Z][A-Z0-9_]*$/.test(type.name ?? "")) errors.push(`${where}: name must be SCREAMING_SNAKE`);
            if (seen.has(type.name)) errors.push(`${where}: named twice`);
            seen.add(type.name);
            if (type.kind !== null && !kindNames.has(type.kind)) {
                errors.push(`${where}: kind ${String(type.kind)} is not a kind of ${CONTRACT}`);
            }
            for (const key of ["ops", "structure"]) {
                if (type[key] !== null && !identifier.test(type[key] ?? "")) {
                    errors.push(`${where}: ${key} must be a C identifier or null`);
                }
            }
            if (type.contain !== null && type.contain !== undefined) {
                const keys = new Set(["blocks", "inlines", "members", "member", "root", "owned", "flat"]);
                for (const key of Object.keys(type.contain)) {
                    if (!keys.has(key)) errors.push(`${where}: unknown contain key ${key}`);
                    else if (key !== "members" && typeof type.contain[key] !== "boolean") {
                        errors.push(`${where}: contain.${key} must be a boolean`);
                    }
                }
                if (type.contain.members !== undefined && !Array.isArray(type.contain.members)) {
                    errors.push(`${where}: contain.members must be a list of type names`);
                }
            }
            return { ...type, value: index + 1 };
        });
        return { name, types };
    });
    /* A member names a type of either class; the masks hold one bit per value. */
    const typeClass = new Map(classes.flatMap(({ name, types }) => types.map((type) => [type.name, name])));
    for (const { name, types } of classes) {
        for (const type of types) {
            for (const member of type.contain?.members ?? []) {
                if (!typeClass.has(member)) errors.push(`${NATIVE}: ${name} type ${type.name}: no type ${member}`);
            }
        }
    }
    if (Math.max(...classes.map(({ types }) => types.length)) >= 32) {
        errors.push(`${NATIVE}: a class outgrows the 32-bit containment masks`);
    }
    const reported = new Set(classes.flatMap(({ types }) => types.map((type) => type.kind)));
    for (const kind of kindNames) {
        if (!reported.has(kind)) errors.push(`${NATIVE}: no native type reports the public kind ${kind}`);
    }
    if (errors.length) throw new Error(errors.join("\n"));

    const [block, inline] = classes;
    return { kinds, block, inline, count: Math.max(block.types.length, inline.types.length) + 1 };
}

const begin = (name) => `/* BEGIN GENERATED ${name} by ${GENERATOR}; edit the node-kind schemas instead. */`;
const END = "/* END GENERATED */";

/** Replaces the generated region `name` of a hand-written C file, which holds
 * it once. */
function region(relative, source, name, body) {
    const BEGIN = begin(name);
    const start = source.indexOf(BEGIN);
    const end = source.indexOf(END, start);
    if (start < 0 || end < start || source.indexOf(BEGIN, start + 1) >= 0) {
        throw new Error(`${relative}: needs exactly one generated region of ${name}`);
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

/** One table over both classes, indexed as element.h's markdown_core_kind_table_index:
 * a block type at its value, an inline type 32 past it, and every entry of no
 * kind -- each class's values that number no type, the last among them --
 * reading as `none`. */
function kindTable(declaration, { block, inline }, entry, none) {
    const rows = [];
    for (const [offset, klass] of [
        ["", block],
        ["MARKDOWN_CORE_KIND_TABLE_CLASS + ", inline]
    ]) {
        const byValue = new Map(klass.types.map((type) => [type.value, type]));
        for (let value = 0; value < 32; value++) {
            const type = byValue.get(value);
            const row = type ? entry(type) : none;
            if (row === null) continue;
            const index = type
                ? `${offset}(MARKDOWN_CORE_NODE_${type.name} & MARKDOWN_CORE_NODE_VALUE_MASK)`
                : `${offset}${String(value)}`;
            rows.push(`    [${index}] = ${row},`);
        }
    }
    return `${declaration}[MARKDOWN_CORE_KIND_TABLE_COUNT] = {\n${rows.join("\n")}\n};\n`;
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
    /* The schema's rule, resolved to a mask per class: a child is held when it
     * is no root and the parent names it among its members, or it is no
     * member type, the parent holds its whole class and, for a flat parent,
     * it is not the parent's own type. */
    const all = [
        ...block.types.map((type) => ({ type, klass: "block" })),
        ...inline.types.map((type) => ({ type, klass: "inline" }))
    ];
    const holds = (parent, { type: child, klass }) => {
        const contain = parent.contain ?? {};
        if (child.contain?.root) return false;
        if ((contain.members ?? []).includes(child.name)) return true;
        if (child.contain?.member || (contain.flat && child.name === parent.name)) return false;
        return Boolean(contain[`${klass}s`]);
    };
    const mask = (parent, klass) =>
        all
            .filter((entry) => entry.klass === klass && holds(parent, entry))
            .reduce((bits, { type }) => bits | (1 << type.value), 0) >>> 0;
    const hold = (type) => {
        const contain = type.contain;
        if (contain === null) return null;
        const parts = [];
        const named = [
            ["root", "ROOT"],
            ["owned", "OWNED"]
        ]
            .filter(([key]) => contain[key])
            .map(([, flag]) => `MARKDOWN_CORE_HOLD_${flag}`);
        if (named.length) parts.push(`.flags = ${named.join(" | ")}`);
        for (const klass of ["block", "inline"]) {
            const bits = mask(type, klass);
            if (bits) parts.push(`.${klass}s = 0x${bits.toString(16).padStart(8, "0")}u`);
        }
        return `{${parts.join(", ")}}`;
    };
    const typeString = (type) => `"${type.name.toLowerCase()}"`;
    const ops = (type) => `&MARKDOWN_CORE_RECORD_${type.ops ?? "NONE"}`;
    const named = [...new Set([...block.types, ...inline.types].map((type) => type.ops).filter((name) => name))];
    return (
        "/* Explicitly sized, so a kind whose value index outgrows the projection is a\n" +
        " * compile error here rather than a silent NULL at every lookup. */\n" +
        classTable("const markdown_core_element *const markdown_core_block_structure", block, element) +
        "\n" +
        classTable("const markdown_core_element *const markdown_core_inline_structure", inline, element) +
        "\n" +
        "/* What a node of each type may hold (element.h, markdown_core_hold), one bit\n" +
        " * per held type's value, resolved from node-types.json `contain`; a type that\n" +
        " * holds nothing and has no rule as a child reads as all zero. */\n" +
        "/* clang-format off */\n" +
        kindTable(
            "const markdown_core_hold markdown_core_holds",
            { block, inline },
            hold,
            "{.flags = MARKDOWN_CORE_HOLD_ROOT}"
        ) +
        "/* clang-format on */\n" +
        "\n" +
        "/* The ops of each type's record (element.h, markdown_core_record_ops), each\n" +
        " * defined by the element whose record it is; a type that holds none reads as\n" +
        " * the record of nothing, whose hooks are all NULL. */\n" +
        named.map((name) => `extern const markdown_core_record_ops MARKDOWN_CORE_RECORD_${name};\n`).join("") +
        "static const markdown_core_record_ops MARKDOWN_CORE_RECORD_NONE = {0};\n" +
        "\n" +
        "/* clang-format off */\n" +
        kindTable(
            "const markdown_core_record_ops *const markdown_core_records",
            { block, inline },
            ops,
            "&MARKDOWN_CORE_RECORD_NONE"
        ) +
        "/* clang-format on */\n" +
        "\n" +
        classTable("static const char *const S_block_type_string", block, typeString) +
        "\n" +
        classTable("static const char *const S_inline_type_string", inline, typeString)
    );
}

function kotlin({ kinds }) {
    return (
        `// Generated by ${GENERATOR} from ${CONTRACT}; do not edit.\n` +
        "package com.nouprax.markdown.core\n\n" +
        "/**\n" +
        " * A node's kind, as the canonical AST contract names it. Its ordinal there is\n" +
        " * the MCB3 kind ordinal and the value of C's `markdown_core_node_kind`.\n" +
        " */\n" +
        "public enum class NodeKind(\n" +
        "    internal val rawValue: Int,\n" +
        "    private val label: String,\n" +
        ") {\n" +
        kinds.map((kind) => `    ${screaming(kind.name)}(${String(kind.ordinal)}, "${kind.name}"),\n`).join("") +
        "    ;\n\n" +
        "    /** The kind's name in the canonical AST, as C's `markdown_core_node_kind_name` answers it. */\n" +
        "    override fun toString(): String = label\n\n" +
        "    internal companion object {\n" +
        "        /** The kinds are numbered in declaration order from 1. */\n" +
        "        fun from(rawValue: Int): NodeKind = entries[rawValue - 1]\n" +
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
        'export const kinds: readonly (NativeKind | "none")[] = [\n' +
        ['    "none"', ...names.map((name) => `    "${name}"`)].join(",\n") +
        "\n];\n\n" +
        "type Exactly<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;\n" +
        "type Holds<Claim extends true> = Claim;\n\n" +
        "/** Fails to compile unless the wire kinds are exactly the Markup union's kinds, so every\n" +
        " * switch or mapped type exhaustive over one is exhaustive over the other. */\n" +
        'export type NativeKindsAreMarkupKinds = Holds<Exactly<NativeKind, Markup["kind"]>>;\n'
    );
}

/** Swift's reserved words, which a case name must write in backticks. */
const SWIFT_KEYWORDS = new Set(
    (
        "associatedtype borrowing class consuming deinit enum extension fileprivate func import init inout " +
        "internal let nonisolated open operator private precedencegroup protocol public rethrows static struct " +
        "subscript typealias var break case catch continue default defer do else fallthrough for guard if in " +
        "repeat return throw switch where while Any as await false is nil self Self super throws true try"
    ).split(" ")
);

/** A Swift identifier: a keyword such as `class` is written in backticks. */
const swiftCase = (name) => (SWIFT_KEYWORDS.has(name) ? `\`${name}\`` : name);

function swift({ kinds }) {
    return (
        `// Generated by ${GENERATOR} from ${CONTRACT}; do not edit.\n\n` +
        "/// A node's kind, as the canonical AST contract names it. The raw value is\n" +
        "/// the kind's ordinal there, which C's `markdown_core_node_kind` holds.\n" +
        "public enum NodeKind: Int32, Sendable, Hashable, CaseIterable, CustomStringConvertible {\n" +
        kinds
            .map(
                (kind) =>
                    `    /// \`${kind.name}\`.\n    case ${swiftCase(camel(kind.name))} = ${String(kind.ordinal)}\n`
            )
            .join("") +
        "\n" +
        "    /// Indexed by ordinal from 1.\n" +
        "    private static let names = [\n" +
        kinds.map((kind) => `        "${kind.name}",\n`).join("") +
        "    ]\n\n" +
        "    /// The kind's name in the canonical AST, as C's\n" +
        "    /// `markdown_core_node_kind_name` answers it.\n" +
        "    public var description: String { Self.names[Int(rawValue) - 1] }\n" +
        "}\n"
    );
}

const PACKAGE = "packages/markdown-core";
/** Every generated file's full text, reading the hand-written parts with `readSource`. */
export function outputs(model, readSource) {
    const edit = (relative, regions) => [
        relative,
        Object.entries(regions).reduce(
            (source, [name, body]) => region(relative, source, name, body),
            readSource(relative)
        )
    ];
    return new Map([
        edit(`${PACKAGE}/core/node_type.h`, { "node kinds": internalEnum(model) }),
        edit(`${PACKAGE}/include/markdown_core.h`, { "node kinds": publicEnum(model) }),
        edit(`${PACKAGE}/elements/ast.c`, { "node kinds": facadeTables(model) }),
        edit(`${PACKAGE}/elements/core-elements.c`, { "node kinds": structureTables(model) }),
        edit(`${PACKAGE}/tests/support/test_support.h`, { "node kinds": testKindCount(model) }),
        [
            "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/markup/Kinds.kt",
            kotlin(model)
        ],
        ["packages/es-markdown-core/src/wire/kinds.ts", es(model)],
        ["packages/swift-markdown-core/Sources/MarkdownCore/Kinds.swift", swift(model)]
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
