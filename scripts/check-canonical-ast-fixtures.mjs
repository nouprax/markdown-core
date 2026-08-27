import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { TextDecoder } from "node:util";

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

const nodeTable = proseContract.match(/## Node inventory[\s\S]*?## ParseOptions/)?.[0];
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
    "directives"
];
const stateValidators = {
    "placement.embedded": (tree) => / mode=embedded /.test(tree),
    "placement.standalone": (tree) => / mode=standalone /.test(tree),
    "list.flavor.bullet": (tree) => /^.*List id=\d+:\d+ scope=.* flavor=bullet /m.test(tree),
    "list.flavor.ordered": (tree) => /^.*List id=\d+:\d+ scope=.* flavor=ordered /m.test(tree),
    "list.start.null": (tree) => /^.*List id=\d+:\d+ scope=.* start=null /m.test(tree),
    "list.start.value": (tree) => /^.*List id=\d+:\d+ scope=.* start=-?\d+ /m.test(tree),
    "list.tight.false": (tree) => /^.*List id=\d+:\d+ scope=.* tight=false /m.test(tree),
    "list.tight.true": (tree) => /^.*List id=\d+:\d+ scope=.* tight=true /m.test(tree),
    "listItem.checked.null": (tree) => /^.*ListItem id=\d+:\d+ scope=.* checked=null /m.test(tree),
    "listItem.checked.false": (tree) => /^.*ListItem id=\d+:\d+ scope=.* checked=false /m.test(tree),
    "listItem.checked.true": (tree) => /^.*ListItem id=\d+:\d+ scope=.* checked=true /m.test(tree),
    "codeBlock.info.null": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* info=null /m.test(tree),
    "codeBlock.info.value": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* info="/m.test(tree),
    "codeBlock.language.null": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* language=null /m.test(tree),
    "codeBlock.language.value": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* language="/m.test(tree),
    "codeBlock.fenced.false": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* fenced=false /m.test(tree),
    "codeBlock.fenced.true": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* fenced=true /m.test(tree),
    "codeBlock.closed.false": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* closed=false /m.test(tree),
    "codeBlock.closed.true": (tree) => /^.*CodeBlock id=\d+:\d+ scope=.* closed=true /m.test(tree),
    "table.alignment.none": (tree) => /^.*Table id=\d+:\d+ scope=.*alignments=\[[^\]]*none[^\]]*\]/m.test(tree),
    "table.alignment.left": (tree) => /^.*Table id=\d+:\d+ scope=.*alignments=\[[^\]]*left[^\]]*\]/m.test(tree),
    "table.alignment.center": (tree) => /^.*Table id=\d+:\d+ scope=.*alignments=\[[^\]]*center[^\]]*\]/m.test(tree),
    "table.alignment.right": (tree) => /^.*Table id=\d+:\d+ scope=.*alignments=\[[^\]]*right[^\]]*\]/m.test(tree),
    "tableRow.isHeader.false": (tree) => /^.*TableRow id=\d+:\d+ scope=.* isHeader=false /m.test(tree),
    "tableRow.isHeader.true": (tree) => /^.*TableRow id=\d+:\d+ scope=.* isHeader=true /m.test(tree),
    "directive.attributes.null": (tree) => /^.*Directive(?:Block)? id=\d+:\d+ scope=.* attributes=null /m.test(tree),
    "directive.attributes.empty": (tree) => /^.*Directive(?:Block)? id=\d+:\d+ scope=.* attributes=\[\] /m.test(tree),
    "directive.attributes.value": (tree) => /^.*Directive(?:Block)? id=\d+:\d+ scope=.* attributes=\[.+\] /m.test(tree),
    // A label is a NODE now, so its three states are read off the tree rather
    // than off a count on the parent: absent is a directive with no
    // DirectiveLabel child, empty is one with `children=0`, populated is one
    // with children.
    "directive.label.null": (tree) =>
        /^(.*)Directive(?:Block)? id=[^ ]* scope=[^\n]*\n(?!\1(?:\u2502|\|)?\s*(?:\u251c|\u2514)\u2500\u2500 DirectiveLabel )/m.test(
            tree
        ),
    "directive.label.empty": (tree) => /DirectiveLabel id=\S+ scope=\S+ children=0$/m.test(tree),
    "directive.label.populated": (tree) => /DirectiveLabel id=\S+ scope=\S+ children=[1-9]\d*$/m.test(tree),
    "reference.form.full": (tree) => /^.*(?:Link|Image)Reference id=\d+:\d+ scope=.* form=full /m.test(tree),
    "reference.form.collapsed": (tree) => /^.*(?:Link|Image)Reference id=\d+:\d+ scope=.* form=collapsed /m.test(tree),
    "reference.form.shortcut": (tree) => /^.*(?:Link|Image)Reference id=\d+:\d+ scope=.* form=shortcut /m.test(tree),
    "link.title.null": (tree) => /^.*Link id=\d+:\d+ scope=.* title=null /m.test(tree),
    "link.title.empty": (tree) => /^.*Link id=\d+:\d+ scope=.* title="" /m.test(tree),
    "link.title.value": (tree) => /^.*Link id=\d+:\d+ scope=.* title=".+" /m.test(tree),
    "image.title.null": (tree) => /^.*Image id=\d+:\d+ scope=.* title=null /m.test(tree),
    "image.title.value": (tree) => /^.*Image id=\d+:\d+ scope=.* title=".+" /m.test(tree),
    "scope.positive": (tree) => / scope=[1-9]\d*:[1-9]\d*\.\./.test(tree),
    /* `scope.zero` was here, and it required the canonical corpus to demonstrate
       a node with NO position -- 0:0..0:0. Its only two witnesses in that corpus
       were the LineBreak and the SoftBreak in inlines.ast, and 0a.12b gave both
       of them a real position (D26). The remaining producers of that shape are
       D13's empty Text and the split-off table lead, and pinning either as
       canonical coverage would bless a defect the stage is closing -- which is
       exactly the golden-regeneration trap: a golden regenerated over a live
       defect blesses it. The state is
       therefore deleted rather than re-witnessed. This is a coverage obligation,
       not a grammar or schema change: the dump still permits 0:0..0:0, so no
       binding and no golden format moves. */
    "children.empty": (tree) => / children=0(?:\n|$)/.test(tree),
    "children.populated": (tree) => / children=[1-9]\d*(?:\n|$)/.test(tree),
    "escaping.empty-string": (tree) => /=""/.test(tree),
    "escaping.newline": (tree) => /\\n/.test(tree),
    // An attribute VALUE that contains a quote. It was called `escaping.json`
    // when the whole attribute map was one JSON string; the escaping it checks
    // is the dump's, and that is what it was always about.
    "escaping.attribute-value": (tree) => /attributes=\[[^\]]*="[^\]]*\\"/.test(tree)
};
const orderValidators = {
    "document.source-order": (tree) => tree.startsWith("Document id="),
    "table.header-rows-cells": (tree) =>
        /Table id=[\s\S]*TableRow id=\d+:\d+ scope=.*isHeader=true[\s\S]*TableCell id=[\s\S]*TableRow id=\d+:\d+ scope=.*isHeader=false/.test(
            tree
        ),
    "directive.label-before-content": (tree) =>
        /DirectiveBlock id=\d+:\d+ scope=.* children=[2-9]\d*\n[\s\S]*DirectiveLabel id=[\s\S]*Paragraph id=/.test(tree),
    "inline.source-order": (tree) => /Paragraph id=\d+:\d+ scope=.* children=[2-9]\d*/.test(tree)
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
    /^(?:(?:│ {3}| {4})*(?:├──|└──) )?([A-Z][A-Za-z]+) id=\d+:\d+ scope=-?\d+:-?\d+\.\.-?\d+:-?\d+(?: .+)? children=\d+$/;

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
        // Strings first, then bracketed groups: `attributes=[a="1" b="2"]` is
        // ONE field, and without the second pass ` b=` reads as a second one.
        const lineWithoutStrings = line.replace(/"(?:\\.|[^"\\])*"/g, '""').replace(/=\[[^\]]*\]/g, "=[]");
        const fieldNames = [...lineWithoutStrings.matchAll(/ ([A-Za-z]+)=/g)].map((field) => field[1]);
        // The dump's field names for a kind ARE the contract's, minus the
        // fields that are the child structure itself. Until Step 15A this was a
        // hand-written copy of the table -- a SEVENTH one -- and Q29 found it
        // by deleting `mode` from the contract and watching this file disagree.
        //
        // A field is child structure when its type names a KIND. That used to
        // be a regex listing four of them plus an explicit `label` exception,
        // because a directive's label was a COUNT in the dump rather than a
        // node; Step 7 made it a node and the exception became a lie.
        const kindNames = new Set(contract.kinds.map((kind) => kind.name));
        const isChildEdge = (type) =>
            [...type.matchAll(/[A-Za-z]+/g)].some((word) => word[0] === "Markup" || kindNames.has(word[0]));
        const dumpFields = Object.fromEntries(
            contract.kinds.map((kind) => [
                kind.name,
                kind.fields.filter((field) => !isChildEdge(field.type)).map((field) => field.name)
            ])
        );
        const expectedFieldNames = ["id", "scope", ...(dumpFields[kind] ?? []), "children"];
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
