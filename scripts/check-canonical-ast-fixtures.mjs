import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { TextDecoder } from "node:util";

const root = process.cwd();
const contractPath = path.join(root, "docs/specs/canonical-ast.md");
const specPath = path.join(root, "specs/canonical-ast");
const manifestPath = path.join(specPath, "manifest.json");
const decoder = new TextDecoder("utf-8", { fatal: true });

const [contract, manifestText, entries] = await Promise.all([
    readFile(contractPath, "utf8"),
    readFile(manifestPath, "utf8"),
    readdir(specPath)
]);
const manifest = JSON.parse(manifestText);
const failures = [];
const difference = (left, right) => [...left].filter((value) => !right.has(value)).sort();
const sameArray = (left, right) => left.length === right.length && left.every((value, index) => value === right[index]);
const set = (values) => new Set(values);

const nodeTable = contract.match(/## Node inventory[\s\S]*?## ParseOptions/)?.[0];
if (nodeTable === undefined) throw new Error("Unable to locate the canonical node inventory");

const rows = [...nodeTable.matchAll(/^\| `([A-Za-z]+)` \| ([^|]+) \|/gm)];
const canonicalKinds = rows.map((match) => match[1]);
const fieldsByKind = Object.fromEntries(
    rows.map((match) => [match[1], [...match[2].matchAll(/`([A-Za-z]+):/g)].map((field) => field[1])])
);
const canonicalFields = rows.flatMap((match) => fieldsByKind[match[1]].map((field) => `${match[1]}.${field}`));

const optionNames = [
    "smartPunctuation",
    "footnotes",
    "stripHTMLComments",
    "tables",
    "strikethrough",
    "autolinks",
    "taskLists",
    "formulas",
    "dollarFormulaDelimiters",
    "latexFormulaDelimiters",
    "directives"
];
const stateValidators = {
    "placement.embedded": (tree) => / mode=embedded /.test(tree),
    "placement.standalone": (tree) => / mode=standalone /.test(tree),
    "list.flavor.bullet": (tree) => /^.*List scope=.* flavor=bullet /m.test(tree),
    "list.flavor.ordered": (tree) => /^.*List scope=.* flavor=ordered /m.test(tree),
    "list.start.null": (tree) => /^.*List scope=.* start=null /m.test(tree),
    "list.start.value": (tree) => /^.*List scope=.* start=-?\d+ /m.test(tree),
    "list.tight.false": (tree) => /^.*List scope=.* tight=false /m.test(tree),
    "list.tight.true": (tree) => /^.*List scope=.* tight=true /m.test(tree),
    "listItem.checked.null": (tree) => /^.*ListItem scope=.* checked=null /m.test(tree),
    "listItem.checked.false": (tree) => /^.*ListItem scope=.* checked=false /m.test(tree),
    "listItem.checked.true": (tree) => /^.*ListItem scope=.* checked=true /m.test(tree),
    "codeBlock.info.null": (tree) => /^.*CodeBlock scope=.* info=null /m.test(tree),
    "codeBlock.info.value": (tree) => /^.*CodeBlock scope=.* info="/m.test(tree),
    "codeBlock.language.null": (tree) => /^.*CodeBlock scope=.* language=null /m.test(tree),
    "codeBlock.language.value": (tree) => /^.*CodeBlock scope=.* language="/m.test(tree),
    "codeBlock.fenced.false": (tree) => /^.*CodeBlock scope=.* fenced=false /m.test(tree),
    "codeBlock.fenced.true": (tree) => /^.*CodeBlock scope=.* fenced=true /m.test(tree),
    "codeBlock.closed.false": (tree) => /^.*CodeBlock scope=.* closed=false /m.test(tree),
    "codeBlock.closed.true": (tree) => /^.*CodeBlock scope=.* closed=true /m.test(tree),
    "table.alignment.none": (tree) => /^.*Table scope=.*alignments=\[[^\]]*none[^\]]*\]/m.test(tree),
    "table.alignment.left": (tree) => /^.*Table scope=.*alignments=\[[^\]]*left[^\]]*\]/m.test(tree),
    "table.alignment.center": (tree) => /^.*Table scope=.*alignments=\[[^\]]*center[^\]]*\]/m.test(tree),
    "table.alignment.right": (tree) => /^.*Table scope=.*alignments=\[[^\]]*right[^\]]*\]/m.test(tree),
    "tableRow.isHeader.false": (tree) => /^.*TableRow scope=.* isHeader=false /m.test(tree),
    "tableRow.isHeader.true": (tree) => /^.*TableRow scope=.* isHeader=true /m.test(tree),
    "directive.attributes.null": (tree) => /^.*Directive(?:Block)? scope=.* attributes=null /m.test(tree),
    "directive.attributes.empty": (tree) => /^.*Directive(?:Block)? scope=.* attributes="\{\}" /m.test(tree),
    "directive.attributes.value": (tree) => /^.*Directive(?:Block)? scope=.* attributes="\{.+\}" /m.test(tree),
    "directive.label.null": (tree) => /^.*Directive(?:Block)? scope=.* label=null /m.test(tree),
    "directive.label.empty": (tree) => /^.*Directive(?:Block)? scope=.* label=0 /m.test(tree),
    "directive.label.populated": (tree) => /^.*Directive(?:Block)? scope=.* label=[1-9]\d* /m.test(tree),
    "link.title.null": (tree) => /^.*Link scope=.* title=null /m.test(tree),
    "link.title.empty": (tree) => /^.*Link scope=.* title="" /m.test(tree),
    "link.title.value": (tree) => /^.*Link scope=.* title=".+" /m.test(tree),
    "image.title.null": (tree) => /^.*Image scope=.* title=null /m.test(tree),
    "image.title.value": (tree) => /^.*Image scope=.* title=".+" /m.test(tree),
    "scope.positive": (tree) => / scope=[1-9]\d*:[1-9]\d*\.\./.test(tree),
    "scope.zero": (tree) => / scope=0:0\.\.0:0 /.test(tree),
    "children.empty": (tree) => / children=0(?:\n|$)/.test(tree),
    "children.populated": (tree) => / children=[1-9]\d*(?:\n|$)/.test(tree),
    "escaping.empty-string": (tree) => /=""/.test(tree),
    "escaping.newline": (tree) => /\\n/.test(tree),
    "escaping.json": (tree) => /attributes="\{\\"/.test(tree)
};
const orderValidators = {
    "document.source-order": (tree) => tree.startsWith("Document scope="),
    "table.header-rows-cells": (tree) =>
        /Table scope=[\s\S]*TableRow scope=.*isHeader=true[\s\S]*TableCell scope=[\s\S]*TableRow scope=.*isHeader=false/.test(
            tree
        ),
    "directive.label-before-content": (tree) =>
        /DirectiveBlock scope=.*label=[1-9]\d* children=[2-9]\d*\n[\s\S]*Text scope=[\s\S]*Paragraph scope=/.test(tree),
    "inline.source-order": (tree) => /Paragraph scope=.* children=[2-9]\d*/.test(tree)
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

    if (!sameArray(Object.keys(testCase.parseOptions ?? {}), optionNames)) {
        failures.push(`${label} parseOptions must explicitly list every frozen option in contract order`);
    } else if (Object.values(testCase.parseOptions).some((value) => typeof value !== "boolean")) {
        failures.push(`${label} parseOptions values must all be booleans`);
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
        const match = line.match(treeLine);
        if (match === null) {
            failures.push(`${testCase.expected}:${index + 1} does not match the canonical line grammar`);
            continue;
        }
        const kind = match[1];
        actualKinds.add(kind);
        for (const field of fieldsByKind[kind] ?? []) allObservedFields.add(`${kind}.${field}`);
        const lineWithoutStrings = line.replace(/"(?:\\.|[^"\\])*"/g, '""');
        const fieldNames = [...lineWithoutStrings.matchAll(/ ([A-Za-z]+)=/g)].map((field) => field[1]);
        const dumpFields = {
            Document: [],
            BlockQuote: [],
            Paragraph: [],
            Heading: ["level"],
            ThematicBreak: [],
            List: ["flavor", "start", "tight"],
            ListItem: ["checked"],
            CodeBlock: ["mode", "info", "language", "literal", "fenced", "closed"],
            HTMLBlock: ["literal"],
            FormulaBlock: ["mode", "literal"],
            Table: ["alignments"],
            TableRow: ["isHeader"],
            TableCell: [],
            DirectiveBlock: ["mode", "name", "attributes", "label"],
            FootnoteDefinition: ["id"],
            Text: ["literal"],
            SoftBreak: [],
            LineBreak: [],
            Code: ["mode", "literal"],
            HTML: ["literal"],
            Formula: ["mode", "literal"],
            Emphasis: [],
            Strong: [],
            Strikethrough: [],
            Link: ["destination", "title"],
            Image: ["source", "title"],
            Directive: ["mode", "name", "attributes", "label"],
            FootnoteReference: ["id"]
        };
        const expectedFieldNames = ["scope", ...(dumpFields[kind] ?? []), "children"];
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
