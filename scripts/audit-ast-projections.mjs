#!/usr/bin/env node
/**
 * AST-projection audit.
 *
 * `docs/specs/canonical-ast.json` is THE contract: kind, fields in canonical
 * order, types, nullability. Platforms then declare the same definition again
 * in their own language, because a Swift `Heading` cannot be produced by a C
 * function and neither can a Kotlin one or a TypeScript one. Those
 * declarations are not duplication to be removed; they are the projection
 * layer, and the layer's whole job is to DEFINE the shape and INITIALIZE it.
 * It derives nothing.
 *
 * Which is exactly why it has to be checked. Hand-written copies of one table
 * drift the way five hand-written source lists drifted, and a projection that
 * quietly lacks a field is indistinguishable, from inside that language, from
 * a field the parser never had.
 *
 * THE PROSE IS CHECKED TOO. `docs/specs/canonical-ast.md` carries everything a
 * table cannot say -- the core rules, coordinates, ownership, the attribute
 * grammar -- and its own kind/field table is a SECOND copy of the contract.
 * This audit compares it against the JSON kind for kind and field for field,
 * so the two cannot drift apart. Until Step 15A the Markdown table WAS the
 * contract, it lived under `docs/deprecated/`, which
 * `docs/RECONSTRUCTION.md` says is archive and not normative, and four
 * executable policy files read it from there.
 *
 * It does not check types across platforms: a `level` is `Int` in the
 * contract, `Int32` in Swift, `Int` in Kotlin and `number` in TypeScript, and
 * pretending one spelling is canonical would be a lie the audit then has to
 * maintain.
 *
 *   node scripts/audit-ast-projections.mjs
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");

const CONTRACT_PATH = "docs/specs/canonical-ast.json";
const PROSE_PATH = "docs/specs/canonical-ast.md";

/** The contract: an ordered kind -> field-name list. */
function definition() {
    const contract = JSON.parse(fs.readFileSync(path.join(root, CONTRACT_PATH), "utf8"));
    if (!Array.isArray(contract.kinds) || contract.kinds.length === 0) {
        throw new Error(`${CONTRACT_PATH}: no kinds`);
    }
    return new Map(contract.kinds.map((kind) => [kind.name, kind.fields.map((f) => f.name)]));
}

/** The same table as the prose spells it, so the two can be compared. */
function proseDefinition() {
    const lines = fs.readFileSync(path.join(root, PROSE_PATH), "utf8").split("\n");
    const header = lines.findIndex((line) => line.startsWith("| Kind | Fields in canonical order"));
    if (header < 0) {
        throw new Error(`${PROSE_PATH}: the kind/fields table is gone or renamed`);
    }
    const kinds = new Map();
    for (const line of lines.slice(header + 2)) {
        if (!line.startsWith("|")) break;
        const cells = line
            .trim()
            .replace(/^\||\|$/g, "")
            .split("|")
            .map((c) => c.trim());
        if (cells.length < 2) break;
        const kind = cells[0].replace(/`/g, "");
        // `mode` is spelled without a type in the prose because the allowed
        // values are fixed per kind by the table above it.
        const fields = cells[1] === "none" ? [] : [...cells[1].matchAll(/`([A-Za-z]+)(?::[^`]*)?`/g)].map((m) => m[1]);
        kinds.set(kind, fields);
    }
    if (kinds.size === 0) {
        throw new Error(`${PROSE_PATH}: the kind/fields table read as empty`);
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

/** SCREAMING_SNAKE for a PascalCase kind: `HTMLBlock` -> `HTML_BLOCK`. */
const camel = (kind) =>
    kind.replace(/^([A-Z]+)(?=[A-Z][a-z]|$)/, (m) => m.toLowerCase()).replace(/^([A-Z])/, (m) => m.toLowerCase());

const snake = (kind) =>
    kind
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
        .toUpperCase();

/** A field the dump cannot print, because it IS the child structure. */
const structural = (field) => /Markup|ListItem|TableRow|TableCell/.test(field.type);

/** Every kind named by one file, in the order it names them. */
function namedKinds(relative, pattern, transform = (m) => m[1]) {
    const text = read(relative);
    return [...text.matchAll(pattern)].map(transform);
}

const modelProjections = [
    projection({
        label: "Swift model",
        directories: ["packages/swift-markdown-core/Sources/MarkdownCore"],
        // Document is a final class — it owns the native parse, which a value
        // type cannot release — while every other kind is a struct. Both are
        // declarations; only the keyword differs.
        declaration: (kind) => new RegExp(`public (?:final class|struct) ${kind}\\b[^\\n]*\\{`),
        field: /public (?:let|var) ([A-Za-z]+)\s*:/g
    }),
    projection({
        label: "Kotlin model",
        directories: ["packages/kotlin-markdown-core/src/commonMain/kotlin"],
        // Both spellings: most kinds take an `internal constructor`, the two
        // extension kinds take a plain one. A reader that knew only the first
        // reported them as missing.
        declaration: (kind) => new RegExp(`public class ${kind}\\b[^\\n]*\\(`),
        field: /(?:public |override )?val ([A-Za-z]+)\s*:/g
    }),
    projection({
        label: "ES model",
        directories: ["packages/es-markdown-core/src/model"],
        // A kind with no fields is a type alias, not an interface — which is
        // the correct TypeScript for it, and reads as "declared with zero
        // fields", not as "missing".
        declaration: (kind) => new RegExp(`export (?:interface ${kind}\\b[^\\n]*\\{|type ${kind}\\s*=)`),
        field: /readonly ([A-Za-z]+)\s*[?]?\s*:/g
    })
];

const kinds = definition();
const contract = JSON.parse(fs.readFileSync(path.join(root, CONTRACT_PATH), "utf8"));
let failed = false;

// The prose's table is a second copy of the contract, in order.
{
    const prose = proseDefinition();
    const contractOrder = [...kinds.keys()].join(",");
    const proseOrder = [...prose.keys()].join(",");
    if (contractOrder !== proseOrder) {
        console.error(`${PROSE_PATH}: its table names a different set or order of kinds than ${CONTRACT_PATH}`);
        failed = true;
    }
    for (const [kind, fields] of kinds) {
        const declared = prose.get(kind);
        if (declared === undefined) continue;
        if (declared.join(",") !== fields.join(",")) {
            console.error(
                `${PROSE_PATH}: ${kind} reads [${declared.join(", ")}] and the contract says [${fields.join(", ")}]`
            );
            failed = true;
        }
    }
}

/* SIX SURFACES, and every one of them names every kind.
 *
 * §4.1's Step 15A requires ONE audit over the C header, the C dump, the Kotlin
 * bridge + decoder + model, the ES bridge + export list + decoder + model, the
 * Swift model + dumper, and the canonical-AST manifest. Until 15A.3 this file
 * read three of those -- the three MODELS -- so a decoder that forgot a kind, a
 * dumper that could not name one, or a wire enum that was one short was
 * invisible here and visible only if some test happened to parse that kind. */
const kindSurfaces = [
    {
        label: "C header kind enum",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds("packages/markdown-core/include/markdown_core.h", /MARKDOWN_CORE_KIND_([A-Z_]+)/g).filter(
            (name) => name !== "NONE"
        )
    },
    {
        label: "C dump kind names",
        expect: [...kinds.keys()],
        actual: namedKinds("packages/markdown-core/extensions/ast.c", /^\s+"([A-Za-z]+)"[,}]/gm).filter(
            (name) => name !== "None"
        )
    },
    {
        label: "Kotlin wire kinds",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/wire/WireKind.kt",
            /^\s{4}([A-Z][A-Z_]*)\(\d+\),$/gm
        )
    },
    {
        label: "Kotlin decoder",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/wire/WireMarkupDecoder.kt",
            /WireKind\.([A-Z_]+)\s*->/g
        )
    },
    {
        label: "Kotlin dumper",
        expect: [...kinds.keys()],
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/TreeDumper.kt",
            /override fun visit([A-Za-z]+)\(/g
        )
    },
    {
        label: "ES export list",
        expect: [...kinds.keys()],
        actual: namedKinds("packages/es-markdown-core/src/index.ts", /export (?:type )?\{([^}]*)\}/g, (m) => m[1])
            .flatMap((names) => names.split(",").map((n) => n.trim()))
            .filter((name) => kinds.has(name))
    },
    {
        label: "ES wire kinds",
        expect: [...kinds.keys()].map(camel),
        actual: namedKinds("packages/es-markdown-core/src/wire/kinds.ts", /"([a-zA-Z]+)"/g).filter(
            (name) => name !== "none"
        )
    },
    {
        label: "ES decoder",
        expect: [...kinds.keys()].map(camel),
        actual: namedKinds("packages/es-markdown-core/src/wire/node-decoder.ts", /case "([a-zA-Z]+)":/g)
    },
    {
        label: "ES dumper",
        expect: [...kinds.keys()],
        actual: namedKinds("packages/es-markdown-core/src/tree-dumper.ts", /^\s+visit([A-Za-z]+): \(/gm)
    },
    {
        label: "Swift dumper",
        expect: [...kinds.keys()],
        actual: namedKinds(
            "packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift",
            /mutating func visit\(_:? ?n?o?d?e?:? (?:MarkdownCore\.)?([A-Za-z]+)\)/g
        )
    },
    {
        label: "Swift walker",
        expect: [...kinds.keys()],
        actual: namedKinds(
            "packages/swift-markdown-core/Sources/MarkdownCore/Walker/Walker.swift",
            /mutating func visit\(_:? ?n?o?d?e?:? (?:MarkdownCore\.)?([A-Za-z]+)\)/g
        )
    },
    {
        label: "canonical-AST manifest",
        expect: [...kinds.keys()],
        actual: JSON.parse(read("specs/canonical-ast/manifest.json")).coverageRequirements.kinds
    }
];

for (const { label, expect, actual } of kindSurfaces) {
    const missing = expect.filter((kind) => !actual.includes(kind));
    const extra = actual.filter((kind) => !expect.includes(kind));
    if (actual.length === 0) {
        console.error(`${label}: read no kinds at all — this reader is looking at the wrong thing`);
        failed = true;
        continue;
    }
    if (missing.length) {
        console.error(`${label}: does not name ${missing.join(", ")}`);
        failed = true;
    }
    if (extra.length) {
        console.error(`${label}: names ${extra.join(", ")}, which the contract does not`);
        failed = true;
    }
}

/* The C dump prints every field the contract gives a kind, EXCEPT the ones that
 * are the child structure itself -- `content`, `items`, `header`, `rows`,
 * `cells` are the tree and the `children=` count. `label` is the exception to
 * the exception: it is structural and the dump prints its LENGTH, because a
 * directive's label and its content are two runs of one child list and nothing
 * else in the line says where the first one ends. */
{
    const source = read("packages/markdown-core/extensions/ast.c");
    const body = source.slice(source.indexOf("static void dump_fields"));
    const arms = new Map();
    let pending = [];
    for (const line of body.slice(0, body.indexOf("\n}\n")).split("\n")) {
        const label = /case MARKDOWN_CORE_KIND_([A-Z_]+):/.exec(line);
        if (label) {
            pending.push(label[1]);
            continue;
        }
        for (const literal of line.matchAll(/"([^"]*)"/g)) {
            for (const field of literal[1].matchAll(/([a-zA-Z_]+)=/g)) {
                for (const kind of pending) arms.set(kind, [...(arms.get(kind) ?? []), field[1]]);
            }
        }
        if (/^\s+break;/.test(line)) pending = [];
    }
    for (const [kind, fields] of kinds) {
        const expected = contract.kinds
            .find((entry) => entry.name === kind)
            .fields.filter((field) => field.name === "label" || !structural(field))
            .map((field) => field.name);
        const printed = arms.get(snake(kind)) ?? [];
        const missing = expected.filter((field) => !printed.includes(field));
        if (missing.length) {
            console.error(`C dump: ${kind} never prints ${missing.join(", ")}`);
            failed = true;
        }
        void fields;
    }
}

for (const { label, fieldsOf } of modelProjections) {
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
    `AST-projection audit passed: ${String(kinds.size)} kinds over ` +
        `${String(kindSurfaces.length)} surfaces, the C dump's fields, the prose table, and ` +
        `${String(modelProjections.length)} models.`
);
