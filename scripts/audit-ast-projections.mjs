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
 * so the two cannot drift apart.
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
const DUMP_PATH = "docs/specs/canonical-ast-dump.md";

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

/**
 * The dump grammar's field-order table: kind -> the fields the dump prints
 * between `scope` and `children`, in order.
 *
 * A third copy of the contract. A normative table nobody reads is only prose,
 * so this audit checks it as data.
 */
function dumpGrammarDefinition() {
    const lines = fs.readFileSync(path.join(root, DUMP_PATH), "utf8").split("\n");
    const header = lines.findIndex((line) => line.startsWith("| Kind | Ordered fields between"));
    if (header < 0) {
        throw new Error(`${DUMP_PATH}: the field-order table is gone or renamed`);
    }
    const kinds = new Map();
    for (const line of lines.slice(header + 2)) {
        if (!line.startsWith("|")) break;
        const cells = line
            .trim()
            .replace(/^\||\|$/g, "")
            .split("|")
            .map((cell) => cell.trim());
        if (cells.length < 2) break;
        const fields = cells[1] === "none" ? [] : [...cells[1].matchAll(/`([A-Za-z]+)`/g)].map((m) => m[1]);
        for (const kind of cells[0].matchAll(/`([A-Za-z]+)`/g)) {
            if (kinds.has(kind[1])) throw new Error(`${DUMP_PATH}: ${kind[1]} is named twice`);
            kinds.set(kind[1], fields);
        }
    }
    if (kinds.size === 0) {
        throw new Error(`${DUMP_PATH}: the field-order table read as empty`);
    }
    return kinds;
}

/** Locates each kind's declaration in one platform and reads its field names. */
function projection({ label, directories, declaration, field, optional }) {
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
                return new Map([...body.matchAll(field)].map((m) => [m[1], optional(m)]));
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
        field: /public (?:let|var) ([A-Za-z]+)\s*:\s*([^\n]+)/g,
        optional: (m) => m[2].trim().endsWith("?")
    }),
    projection({
        label: "Kotlin model",
        directories: ["packages/kotlin-markdown-core/src/commonMain/kotlin"],
        // Both spellings: most kinds take an `internal constructor`, the two
        // extension kinds take a plain one. A reader that knew only the first
        // reported them as missing.
        declaration: (kind) => new RegExp(`public class ${kind}\\b[^\\n]*\\(`),
        field: /(?:public |override )?val ([A-Za-z]+)\s*:\s*([^\n]+?),?\s*$/gm,
        optional: (m) => m[2].trim().endsWith("?")
    }),
    projection({
        label: "ES model",
        directories: ["packages/es-markdown-core/src/model"],
        // A kind with no fields is a type alias, not an interface — which is
        // the correct TypeScript for it, and reads as "declared with zero
        // fields", not as "missing".
        declaration: (kind) => new RegExp(`export (?:interface ${kind}\\b[^\\n]*\\{|type ${kind}\\s*=)`),
        field: /readonly ([A-Za-z]+)\s*([?]?)\s*:\s*([^\n;]+);/g,
        optional: (m) => m[2] === "?" || /\|\s*null\b/.test(m[3])
    })
];

const kinds = definition();
const contract = JSON.parse(fs.readFileSync(path.join(root, CONTRACT_PATH), "utf8"));

/** A node-valued field the dump represents as a nested descendant rather than
 * a scalar token. This includes typed ownership fields such as `label`; nested
 * dump layout does not make every such field part of a generic child list. */
const kindNames = new Set(contract.kinds.map((kind) => kind.name));
const structural = (field) =>
    [...field.type.matchAll(/[A-Za-z]+/g)].some((word) => word[0] === "Markup" || kindNames.has(word[0]));

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

/* Every projection surface names every kind.
 *
 * §4.1's Step 15A requires ONE audit over the C header, the C dump, the Kotlin
 * platform adapters + decoders + model, the ES writer + export list + decoder
 * + model, the Swift model + dumper, and the canonical-AST manifest. Until
 * 15A.3 this file
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
        label: "Kotlin JNI node kinds",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/jniMain/kotlin/com/nouprax/markdown/core/wire/JniNodeKind.kt",
            /^\s{4}([A-Z][A-Z_]*)\(\d+\),$/gm
        )
    },
    {
        label: "Kotlin decoder",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/jniMain/kotlin/com/nouprax/markdown/core/wire/JniMarkupDecoder.kt",
            /JniNodeKind\.([A-Z_]+)\s*->/g
        )
    },
    {
        label: "Kotlin/Native C facade adapter",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/nativePlatformMain/kotlin/com/nouprax/markdown/core/PlatformParser.native.kt",
            /^\s+MARKDOWN_CORE_KIND_([A-Z_]+)\s*->/gm
        )
    },
    {
        label: "Kotlin dumper",
        expect: [...kinds.keys()],
        actual: namedKinds(
            "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/visitor/TreeDumper.kt",
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
        label: "ES Wasm batch writer",
        expect: [...kinds.keys()].map(snake),
        actual: namedKinds(
            "packages/es-markdown-core/src/bridge.c",
            /^\s{4}case MARKDOWN_CORE_KIND_([A-Z_]+):/gm
        ).filter((name) => name !== "NONE")
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
            "packages/swift-markdown-core/Sources/MarkdownCore/Visitor/TreeDumper.swift",
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

/* The C dump prints every scalar field the contract gives a kind. Its
 * per-kind dump dispatch nests structural children and any explicitly chosen
 * node-valued fields as graphical descendants. `children=` still counts only
 * structural children; in particular, a directive may visually nest its
 * `label` field without counting or exposing that field as a child. */
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
            .fields.filter((field) => !structural(field))
            .map((field) => field.name);
        const printed = arms.get(snake(kind)) ?? [];
        const missing = expected.filter((field) => !printed.includes(field));
        if (missing.length) {
            console.error(`C dump: ${kind} never prints ${missing.join(", ")}`);
            failed = true;
        }
        // BOTH WAYS. The models may carry members the contract does not name --
        // `accept`, `dump`, an initializer -- so their check is one-directional.
        // The dump's line has no such slack: every `name=` on it is a contract
        // field or it is a field the contract deleted and nobody removed.
        const extra = printed.filter((field) => !expected.includes(field));
        if (extra.length) {
            console.error(`C dump: ${kind} prints ${extra.join(", ")}, which the contract does not give it`);
            failed = true;
        }
        void fields;
    }
}

// The dump grammar's field-order table is the contract's non-structural fields,
// kind for kind and in order.
{
    const grammar = dumpGrammarDefinition();
    for (const [kind] of kinds) {
        const expected = contract.kinds
            .find((entry) => entry.name === kind)
            .fields.filter((field) => !structural(field))
            .map((field) => field.name);
        const declared = grammar.get(kind);
        if (declared === undefined) {
            console.error(`${DUMP_PATH}: no field-order row for ${kind}`);
            failed = true;
            continue;
        }
        if (declared.join(",") !== expected.join(",")) {
            console.error(
                `${DUMP_PATH}: ${kind} reads [${declared.join(", ")}] and the contract says [${expected.join(", ")}]`
            );
            failed = true;
        }
    }
    for (const kind of grammar.keys()) {
        if (!kinds.has(kind)) {
            console.error(`${DUMP_PATH}: names ${kind}, which the contract does not`);
            failed = true;
        }
    }
}

/* NULLABILITY IS PART OF THE CONTRACT and until Step 14 no gate read it.
 *
 * §4.1's standing rule 4 names "a field's name, type, NULLABILITY or category"
 * and says no commit may leave this audit red -- but the audit compared field
 * NAMES only, so flipping `Link.destination` to optional in the contract and
 * leaving all three models required passed, measured. Requirement 14's whole
 * deliverable is that `null` and `""` are different facts, and a contract
 * nothing checks cannot carry that. */
const optionality = new Map(
    contract.kinds.map((kind) => [kind.name, new Map(kind.fields.map((f) => [f.name, f.optional === true]))])
);

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
        for (const name of expected) {
            if (!declared.has(name)) continue;
            const wanted = optionality.get(kind).get(name);
            if (declared.get(name) !== wanted) {
                console.error(
                    `${label}: ${kind}.${name} is declared ${declared.get(name) ? "optional" : "required"} and the ` +
                        `contract says ${wanted ? "optional" : "required"}`
                );
                failed = true;
            }
        }
    }
}

if (failed) {
    console.error("\nAST-projection audit failed: a platform's definition layer has drifted from the contract.");
    process.exit(1);
}
console.log(
    `AST-projection audit passed: ${String(kinds.size)} kinds over ` +
        `${String(kindSurfaces.length)} surfaces, the C dump's fields, the prose table, the dump grammar, and ` +
        `${String(modelProjections.length)} models.`
);
