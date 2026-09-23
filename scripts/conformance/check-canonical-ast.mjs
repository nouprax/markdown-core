import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { TextDecoder } from "node:util";
import { stateValidators } from "./canonical-states.mjs";

const root = process.cwd();
const proseContractPath = path.join(root, "docs/specs/canonical-ast.md");
const contractPath = path.join(root, "docs/specs/canonical-ast.json");
const specPath = path.join(root, "specs/canonical-ast");
const manifestPath = path.join(specPath, "manifest.json");
const decoder = new TextDecoder("utf-8", { fatal: true });

const [proseContract, contractText, manifestText, entries] = await Promise.all([
    readFile(proseContractPath, "utf8"),
    readFile(contractPath, "utf8"),
    readFile(manifestPath, "utf8"),
    readdir(specPath)
]);
const manifest = JSON.parse(manifestText);
const contract = JSON.parse(contractText);
const failures = [];
const difference = (left, right) => [...left].filter((value) => !right.has(value)).sort();
const sameArray = (left, right) => left.length === right.length && left.every((value, index) => value === right[index]);
const set = (values) => new Set(values);

const nodeTable = proseContract.match(/## Node inventory[\s\S]*?## Parsing/)?.[0];
if (nodeTable === undefined) throw new Error("Unable to locate the canonical node inventory");

const rows = [...nodeTable.matchAll(/^\| `([A-Za-z]+)` \| ([^|]+) \|/gm)];
const canonicalKinds = rows.map((match) => match[1]);
const fieldsByKind = Object.fromEntries(
    rows.map((match) => [match[1], [...match[2].matchAll(/`([A-Za-z]+):/g)].map((field) => field[1])])
);
const canonicalFields = rows.flatMap((match) => fieldsByKind[match[1]].map((field) => `${match[1]}.${field}`));

const orderValidators = {
    "definition.term-before-bodies": (tree) =>
        /Definition scope=.*\n[^\n]*DefinitionTerm children=[1-9]\d*[\s\S]*DefinitionBody children=/.test(tree),
    "callout.title-before-content": (tree) =>
        /Callout scope=.* children=[1-9]\d*\n[^\n]*Title children=[1-9]\d*[\s\S]*Paragraph scope=/.test(tree),
    "document.source-order": (tree) => tree.startsWith("Document scope="),
    "table.caption-head-content-foot": (tree) =>
        /TableCaption scope=[\s\S]*TableHead children=[\s\S]*TableBody children=[\s\S]*TableFoot children=/.test(tree),
    "table.head-content-foot": (tree) =>
        /TableHead children=\d+[\s\S]*TableBody children=\d+[\s\S]*TableFoot children=\d+/.test(tree),
    "directive.label-before-content": (tree) =>
        /DirectiveBlock scope=.* children=[1-9]\d*\n[\s\S]*DirectiveLabel scope=[\s\S]*Paragraph scope=/.test(tree),
    "markup.attributes.source-order": (tree) =>
        /DirectiveBlock scope=.*attributes=\{[^}]*properties=".*" metadata=".*"\}/.test(tree),
    "inline.source-order": (tree) => /Paragraph scope=.* children=(?:[2-9]|[1-9]\d+)(?:\n|$)/.test(tree),
    // Every `Footnote` node nests under `Document` after the last content line.
    "document.content-before-footnotes": (tree) => {
        const top = tree
            .split("\n")
            .filter((line) => /^(?:├──|└──) /.test(line))
            .map((line) => line.slice(4).split(" ", 1)[0]);
        const first = top.indexOf("Footnote");
        return first >= 0 && top.slice(first).every((kind) => kind === "Footnote" || kind === "Specimen");
    },
    "document.specimens-source-order": (tree) => {
        const definitions = [...tree.matchAll(/^(?:├──|└──) Specimen scope=(\d+):(\d+)\.\./gm)];
        return (
            definitions.length > 1 &&
            definitions.every(
                (entry, i) =>
                    i === 0 ||
                    Number(entry[1]) > Number(definitions[i - 1][1]) ||
                    (entry[1] === definitions[i - 1][1] && Number(entry[2]) > Number(definitions[i - 1][2]))
            )
        );
    },
    // A `Cite` nests its items in source order: their scopes ascend (M4).
    "cite.items-in-order": (tree) => {
        let cites = 0;
        let ordered = true;
        const lines = tree.split("\n");
        for (const [index, line] of lines.entries()) {
            const marker = line.search(/[├└]/);
            if (marker < 0 || !line.slice(marker + 4).startsWith("Cite scope=")) continue;
            cites++;
            let previous = null;
            for (const item of lines.slice(index + 1)) {
                const itemMarker = item.search(/[├└]/);
                if (itemMarker <= marker) break;
                const match = /^Citation scope=(\d+):(\d+)\.\./.exec(item.slice(itemMarker + 4));
                if (match === null || itemMarker !== marker + 4) continue;
                const start = [Number(match[1]), Number(match[2])];
                if (previous && (start[0] < previous[0] || (start[0] === previous[0] && start[1] <= previous[1]))) {
                    ordered = false;
                }
                previous = start;
            }
        }
        return cites > 0 && ordered;
    }
};

if (manifest.schemaVersion !== 1) failures.push("manifest schemaVersion must be 1");
if (
    manifest.contract !== "docs/specs/canonical-ast.md" ||
    manifest.dumpGrammar !== "docs/specs/canonical-ast-dump.md"
) {
    failures.push("manifest contract paths drifted from the repository specifications");
}
if (
    manifest.format?.encoding !== "UTF-8" ||
    manifest.format?.lineEndings !== "LF" ||
    manifest.format?.finalNewline !== true ||
    manifest.format?.caseOrder !== "manifest"
) {
    failures.push("manifest must freeze UTF-8, LF, one final newline, and manifest case order");
}
if (!sameArray(manifest.coverageRequirements?.kinds ?? [], canonicalKinds)) {
    failures.push("manifest kind inventory must exactly match the canonical AST contract order");
}
if (!sameArray(manifest.coverageRequirements?.states ?? [], Object.keys(stateValidators))) {
    failures.push("manifest state vocabulary must exactly match the fail-closed audit vocabulary");
}
if (!sameArray(manifest.coverageRequirements?.orders ?? [], Object.keys(orderValidators))) {
    failures.push("manifest order vocabulary must exactly match the fail-closed audit vocabulary");
}

const allowedEntries = new Set(["README.md", "manifest.json"]);
const names = new Set();
const inputs = new Set();
const expectedFiles = new Set();
const allCoveredKinds = new Set();
const allCoveredStates = new Set();
const allCoveredOrders = new Set();
const allObservedFields = new Set();
const treeLine =
    /^(?:(?:│ {3}| {4})*(?:├──|└──) )?([A-Z][A-Za-z]+) scope=-?\d+:-?\d+\.\.-?\d+:-?\d+(?: .+)? children=\d+$/;
// A group line nests a node-valued list under its owner with no scope and no
// fields; the names are the dump grammar's.
const groupLine = /^(?:(?:│ {3}| {4})*(?:├──|└──) )([A-Z][A-Za-z]+) children=\d+$/;
const GROUPS = new Set([
    "DefinitionTerm",
    "DefinitionBody",
    "Title",
    "CitationPrefix",
    "CitationSuffix",
    "TableHead",
    "TableBody",
    "TableFoot"
]);
if (!Array.isArray(manifest.cases) || manifest.cases.length === 0) {
    failures.push("manifest cases must be a non-empty array");
}

for (const testCase of manifest.cases ?? []) {
    const label = typeof testCase.name === "string" ? testCase.name : "<unnamed>";
    if (!/^[a-z][a-z0-9-]*$/.test(label) || names.has(label)) {
        failures.push(`invalid or duplicate case name: ${label}`);
    }
    names.add(label);
    const expectedInput = `${label}.md`;
    const expectedOutput = `${label}.ast`;
    if (testCase.input !== expectedInput || testCase.expected !== expectedOutput) {
        failures.push(`${label} paths must be ${expectedInput} and ${expectedOutput}`);
    }
    inputs.add(testCase.input);
    expectedFiles.add(testCase.expected);
    allowedEntries.add(testCase.input);
    allowedEntries.add(testCase.expected);

    // The dialect has no switches: every case is the one language, and the
    // manifest records coverage vocabulary alone. A case that still names an
    // option is asking for a language the parser does not have.
    if ("parseOptions" in testCase) {
        failures.push(`${label} names parseOptions; the dialect has no parse options`);
    }

    let markdown;
    let tree;
    for (const file of [testCase.input, testCase.expected]) {
        try {
            const bytes = await readFile(path.join(specPath, file));
            const text = decoder.decode(bytes);
            if (!text.endsWith("\n") || text.includes("\r")) {
                failures.push(`${file} must use LF and include a final newline`);
            }
            if (file === testCase.expected && text.endsWith("\n\n")) {
                failures.push(`${file} must contain exactly one final newline`);
            }
            if (file === testCase.input) markdown = text;
            else tree = text;
        } catch (error) {
            failures.push(`${file} is missing or is not valid UTF-8: ${error.message}`);
        }
    }
    if (markdown === undefined || tree === undefined) continue;

    const lines = tree.slice(0, -1).split("\n");
    const actualKinds = new Set();
    for (const [index, line] of lines.entries()) {
        const group = line.match(groupLine);
        if (group !== null) {
            if (!GROUPS.has(group[1])) failures.push(`${testCase.expected}:${index + 1} names an unknown group`);
            continue;
        }
        const match = line.match(treeLine);
        if (match === null) {
            failures.push(`${testCase.expected}:${index + 1} does not match the canonical line grammar`);
            continue;
        }
        const kind = match[1];
        actualKinds.add(kind);
        for (const field of fieldsByKind[kind] ?? []) allObservedFields.add(`${kind}.${field}`);
        // Strings first, then bracketed groups: `attributes={a="1" b="2"}` is
        // ONE field, and without the second pass ` b=` reads as a second one.
        const lineWithoutStrings = line
            .replace(/"(?:\\.|[^"\\])*"/g, '""')
            .replace(/=\[[^\]]*\]/g, "=[]")
            .replace(/=\{[^}]*\}/g, "={}");
        const fieldNames = [...lineWithoutStrings.matchAll(/ ([A-Za-z]+)=/g)].map((field) => field[1]);
        // The dump's scalar field names ARE the contract's; node-valued fields
        // are represented by nested dump descendants. Until Step 15A this was a
        // hand-written copy of the table -- a SEVENTH one -- and Q29 found it
        // by deleting `mode` from the contract and watching this file disagree.
        //
        // A field is node-valued when its type names a KIND. That used to
        // be a regex listing four of them plus an explicit `label` exception,
        // because a directive's label was a COUNT in the dump rather than a
        // node; Step 7 made it a node and the exception became a lie.
        const kindNames = new Set(contract.kinds.map((kind) => kind.name));
        const isNodeValuedField = (type, seen = new Set()) =>
            [...type.matchAll(/[A-Za-z]+/g)].some(([name]) => {
                if (name === "Markup" || kindNames.has(name)) return true;
                if (seen.has(name)) return false;
                const value = contract.values[name];
                if (!Array.isArray(value?.branches)) return false;
                const next = new Set([...seen, name]);
                return value.branches.some((branch) =>
                    branch.fields.some((field) => isNodeValuedField(field.type, next))
                );
            });
        const dumpFields = Object.fromEntries(
            contract.kinds.map((kind) => [
                kind.name,
                kind.fields.filter((field) => !isNodeValuedField(field.type)).map((field) => field.name)
            ])
        );
        const inherited = contract.inheritedFields.map((field) => field.name);
        const expectedFieldNames = [...inherited, ...(dumpFields[kind] ?? []), "children"];
        if (!sameArray(fieldNames, expectedFieldNames)) {
            failures.push(
                `${testCase.expected}:${index + 1} fields are ${fieldNames.join(",")}; expected ${expectedFieldNames.join(",")}`
            );
        }
    }
    const declaredKinds = set(testCase.coverage?.kinds ?? []);
    for (const [description, values] of [
        ["missing declared kinds", difference(declaredKinds, actualKinds)],
        ["undeclared kinds", difference(actualKinds, declaredKinds)]
    ]) {
        if (values.length > 0) failures.push(`${label} ${description}: ${values.join(", ")}`);
    }
    for (const kind of declaredKinds) allCoveredKinds.add(kind);

    for (const state of testCase.coverage?.states ?? []) {
        allCoveredStates.add(state);
        if (!(state in stateValidators)) failures.push(`${label} declares unknown state: ${state}`);
        else if (!stateValidators[state](tree)) failures.push(`${label} does not demonstrate declared state: ${state}`);
    }
    for (const order of testCase.coverage?.orders ?? []) {
        allCoveredOrders.add(order);
        if (!(order in orderValidators)) failures.push(`${label} declares unknown order: ${order}`);
        else if (!orderValidators[order](tree)) failures.push(`${label} does not demonstrate declared order: ${order}`);
    }
}

for (const [description, actual, required] of [
    ["Markup kind coverage", allCoveredKinds, set(canonicalKinds)],
    ["behavior-bearing field coverage", allObservedFields, set(canonicalFields)],
    ["state coverage", allCoveredStates, set(Object.keys(stateValidators))],
    ["child-order coverage", allCoveredOrders, set(Object.keys(orderValidators))]
]) {
    const missing = difference(required, actual);
    const unknown = difference(actual, required);
    if (missing.length > 0) failures.push(`${description} is missing: ${missing.join(", ")}`);
    if (unknown.length > 0) failures.push(`${description} is undeclared: ${unknown.join(", ")}`);
}

const unexpectedEntries = entries.filter((entry) => !allowedEntries.has(entry));
const missingEntries = difference(allowedEntries, set(entries));
if (unexpectedEntries.length > 0) failures.push(`unmanifested spec entries: ${unexpectedEntries.sort().join(", ")}`);
if (missingEntries.length > 0) failures.push(`manifested spec entries missing on disk: ${missingEntries.join(", ")}`);
if (inputs.size !== names.size || expectedFiles.size !== names.size) failures.push("case paths must be unique");

if (failures.length > 0) throw new Error(failures.join("\n"));
process.stdout.write(
    `Canonical AST manifest v${manifest.schemaVersion} covers ${canonicalKinds.length} Markup kinds, ${canonicalFields.length} fields, and ${manifest.cases.length} cases.\n`
);
