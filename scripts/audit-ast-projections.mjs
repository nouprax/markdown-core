#!/usr/bin/env node
/**
 * AST-projection audit.
 *
 * `docs/specs/canonical-ast.md` carries one table — kind, fields in
 * canonical order, invariants — and that table is the AST definition. Four
 * platforms then declare the same definition again in their own language,
 * because a Swift `Heading` cannot be produced by a C function and neither
 * can a Kotlin one or a TypeScript one. Those four declarations are not
 * duplication to be removed; they are the projection layer, and the layer's
 * whole job is to DEFINE the shape and INITIALIZE it. It derives nothing.
 *
 * Which is exactly why it has to be checked. Four hand-written copies of one
 * table drift the way five hand-written source lists drifted, and a
 * projection that quietly lacks a field is indistinguishable, from inside
 * that language, from a field the parser never had.
 *
 * This audit reads the table and requires every platform to declare every
 * kind with every field. It does not check types: a `level` is `Int` in the
 * table, `Int32` in Swift, `Int` in Kotlin and `number` in TypeScript, and
 * pretending one spelling is canonical would be a lie the audit then has to
 * maintain.
 *
 *   node scripts/audit-ast-projections.mjs
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

/** The kind -> fields table of the canonical AST contract. */
function definition() {
    const lines = fs
        .readFileSync(path.join(root, "docs/specs/canonical-ast.md"), "utf8")
        .split("\n");
    const header = lines.findIndex((line) => line.startsWith("| Kind | Fields in canonical order"));
    if (header < 0) {
        throw new Error("canonical-ast.md: the kind/fields table is gone or renamed");
    }
    const kinds = new Map();
    for (const line of lines.slice(header + 2)) {
        if (!line.startsWith("|")) break;
        const cells = line.trim().replace(/^\||\|$/g, "").split("|").map((c) => c.trim());
        if (cells.length < 2) break;
        const kind = cells[0].replace(/`/g, "");
        // `mode` is spelled without a type in the table because the allowed
        // values are fixed per kind by the table above it.
        const fields =
            cells[1] === "none"
                ? []
                : [...cells[1].matchAll(/`([A-Za-z]+)(?::[^`]*)?`/g)].map((m) => m[1]);
        kinds.set(kind, fields);
    }
    if (kinds.size === 0) {
        throw new Error("canonical-ast.md: the kind/fields table read as empty");
    }
    return kinds;
}

/** Locates each kind's declaration in one platform and reads its field names. */
function projection({ label, directories, declaration, field }) {
    const files = [];
    for (const directory of directories) {
        const absolute = path.join(root, directory);
        if (!fs.existsSync(absolute)) continue;
        const walk = (d) => {
            for (const entry of fs.readdirSync(d, { withFileTypes: true })) {
                const full = path.join(d, entry.name);
                if (entry.isDirectory()) walk(full);
                else files.push(full);
            }
        };
        walk(absolute);
    }
    return {
        label,
        fieldsOf(kind) {
            for (const file of files) {
                const text = fs.readFileSync(file, "utf8");
                const match = text.match(declaration(kind));
                if (!match) continue;
                // Read to the end of the declaration's own block, so a later
                // type in the same file cannot lend this one its fields.
                const from = match.index;
                const next = [...text.slice(from + 1).matchAll(/^(?:public |export )/gm)]
                    .map((m) => m.index + from + 1)
                    .find((index) => index > from + match[0].length);
                const body = text.slice(from, next ?? text.length);
                return new Set([...body.matchAll(field)].map((m) => m[1]));
            }
            return null;
        }
    };
}

const projections = [
    projection({
        label: "Swift",
        directories: ["packages/swift-markdown-core/Sources/MarkdownCore"],
        // Document is a final class — it owns the native parse, which a value
        // type cannot release — while every other kind is a struct. Both are
        // declarations; only the keyword differs.
        declaration: (kind) => new RegExp(`public (?:final class|struct) ${kind}\\b[^\\n]*\\{`),
        field: /public (?:let|var) ([A-Za-z]+)\s*:/g
    }),
    projection({
        label: "Kotlin",
        directories: ["packages/kotlin-markdown-core/src/commonMain/kotlin"],
        // Both spellings: most kinds take an `internal constructor`, the two
        // extension kinds take a plain one. A reader that knew only the first
        // reported them as missing.
        declaration: (kind) => new RegExp(`public class ${kind}\\b[^\\n]*\\(`),
        field: /(?:public |override )?val ([A-Za-z]+)\s*:/g
    }),
    projection({
        label: "ES",
        directories: ["packages/es-markdown-core/src/model"],
        // A kind with no fields is a type alias, not an interface — which is
        // the correct TypeScript for it, and reads as "declared with zero
        // fields", not as "missing".
        declaration: (kind) =>
            new RegExp(`export (?:interface ${kind}\\b[^\\n]*\\{|type ${kind}\\s*=)`),
        field: /readonly ([A-Za-z]+)\s*[?]?\s*:/g
    })
];

const kinds = definition();
let failed = false;

for (const { label, fieldsOf } of projections) {
    for (const [kind, expected] of kinds) {
        const declared = fieldsOf(kind);
        if (declared === null) {
            console.error(`${label}: no declaration for ${kind}`);
            failed = true;
            continue;
        }
        const missing = expected.filter((f) => !declared.has(f));
        if (missing.length) {
            console.error(`${label}: ${kind} does not declare ${missing.join(", ")}`);
            failed = true;
        }
    }
}

if (failed) {
    console.error("\nAST-projection audit failed: a platform's definition layer has drifted from the contract.");
    process.exit(1);
}
console.log(
    `AST-projection audit passed: ${kinds.size} kinds declared by ${projections.length} platforms.`
);
