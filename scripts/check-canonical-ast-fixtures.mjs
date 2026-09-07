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

const nodeTable = proseContract.match(/## Node inventory[\s\S]*?## Parsing/)?.[0];
if (nodeTable === undefined) throw new Error("Unable to locate the canonical node inventory");

const rows = [...nodeTable.matchAll(/^\| `([A-Za-z]+)` \| ([^|]+) \|/gm)];
const canonicalKinds = rows.map((match) => match[1]);
const fieldsByKind = Object.fromEntries(
    rows.map((match) => [match[1], [...match[2].matchAll(/`([A-Za-z]+):/g)].map((field) => field[1])])
);
const canonicalFields = rows.flatMap((match) => fieldsByKind[match[1]].map((field) => `${match[1]}.${field}`));

/**
 * The kind of every node and of its parent, read from the connectors: a line's
 * depth is the column of its connector, and its parent is the nearest line
 * above at the depth before it. The dump nests a `DirectiveLabel` under its
 * directive the same way, so the label reads as that directive's child here,
 * which is what a placement question needs.
 */
function parentEdges(tree) {
    const byDepth = ["Document"];
    const edges = [];
    for (const line of tree.split("\n")) {
        if (!line.length) continue;
        const marker = line.search(/[\u251c\u2514]/);
        const depth = marker < 0 ? 0 : marker / 4 + 1;
        const kind = (marker < 0 ? line : line.slice(marker + 4)).split(" ", 1)[0];
        if (depth > 0) edges.push({ kind, parent: byDepth[depth - 1] });
        byDepth[depth] = kind;
        byDepth.length = depth + 1;
    }
    return edges;
}
// A `Footnote` is a scoped value rather than a kind, and its content is block
// content, so a comment nested under it is block-placed (M4).
const BLOCK_CONTENT = new Set(["Document", "Callout", "ListItem", "Footnote", "Specimen", "DirectiveBlock"]);
const INLINE_CONTENT = new Set([
    "Paragraph",
    "Heading",
    "TableCell",
    "DirectiveLabel",
    "Emphasis",
    "Strong",
    "Strikethrough",
    "Link",
    "Image"
]);

const stateValidators = {
    "placement.embedded": (tree) => / mode=embedded /.test(tree),
    "placement.standalone": (tree) => / mode=standalone /.test(tree),
    "list.flavor.bullet": (tree) => /^.*List scope=.* flavor=bullet /m.test(tree),
    "list.flavor.ordered": (tree) => /^.*List scope=.* flavor=ordered /m.test(tree),
    "list.start.null": (tree) => /^.*List scope=.* start=null /m.test(tree),
    "list.start.value": (tree) => /^.*List scope=.* start=-?\d+ /m.test(tree),
    "list.tight.false": (tree) => /^.*List scope=.* tight=false /m.test(tree),
    "list.tight.true": (tree) => /^.*List scope=.* tight=true /m.test(tree),
    "listItem.marker.null": (tree) => /^.*ListItem scope=.* marker=null /m.test(tree),
    "listItem.marker.space": (tree) => /^.*ListItem scope=.* marker=" " /m.test(tree),
    "listItem.marker.value": (tree) => /^.*ListItem scope=.* marker="[xX]" /m.test(tree),
    "list.variant.decimal": (tree) => /^.*List scope=.* variant=decimal /m.test(tree),
    "list.variant.null": (tree) => /^.*List scope=.* variant=null /m.test(tree),
    "list.delimiter.period": (tree) => /^.*List scope=.* delimiter=period /m.test(tree),
    "list.delimiter.parenthesis.unclosed": (tree) =>
        /^.*List scope=.* delimiter=parenthesis\(closed=false\) /m.test(tree),
    "codeBlock.info.null": (tree) => /^.*CodeBlock scope=.* info=null /m.test(tree),
    "codeBlock.info.value": (tree) => /^.*CodeBlock scope=.* info="/m.test(tree),
    "codeBlock.language.null": (tree) => /^.*CodeBlock scope=.* language=null /m.test(tree),
    "codeBlock.language.value": (tree) => /^.*CodeBlock scope=.* language="/m.test(tree),
    "codeBlock.fenced.false": (tree) => /^.*CodeBlock scope=.* fenced=false /m.test(tree),
    "codeBlock.fenced.true": (tree) => /^.*CodeBlock scope=.* fenced=true /m.test(tree),
    "codeBlock.closed.false": (tree) => /^.*CodeBlock scope=.* closed=false /m.test(tree),
    "codeBlock.closed.true": (tree) => /^.*CodeBlock scope=.* closed=true /m.test(tree),
    "table.column.relative.null": (tree) => /Table scope=.* columns=\[(?:[a-z]+:null)(?:,[a-z]+:null)*\]/.test(tree),
    "tableCell.span.one": (tree) => /TableCell scope=.* rowspan=1 colspan=1 /.test(tree),
    "tableCell.content.inline": (tree) =>
        parentEdges(tree).some((edge) => edge.parent === "TableCell" && edge.kind === "Text"),
    "table.alignment.none": (tree) => /^.*Table scope=.*columns=\[[^\]]*none[^\]]*\]/m.test(tree),
    "table.alignment.left": (tree) => /^.*Table scope=.*columns=\[[^\]]*left[^\]]*\]/m.test(tree),
    "table.alignment.center": (tree) => /^.*Table scope=.*columns=\[[^\]]*center[^\]]*\]/m.test(tree),
    "table.alignment.right": (tree) => /^.*Table scope=.*columns=\[[^\]]*right[^\]]*\]/m.test(tree),
    // Every `>` container is a `Callout` (M3). Its metadata line arrives with
    // O8; until then every callout is metadata-free, which these three states
    // pin: no variant, no fold marker, and no `Title` group, which is
    // the only way a title prints.
    "callout.variant.null": (tree) => /^.*Callout scope=\S+ variant=null /m.test(tree),
    "callout.collapsed.null": (tree) => /^.*Callout scope=.* collapsed=null /m.test(tree),
    "callout.title.null": (tree) => /^.*Callout scope=/m.test(tree) && !/Title children=/.test(tree),
    "directive.attributes.null": (tree) => /^.*Directive(?:Block)? scope=.* attributes=null /m.test(tree),
    "directive.attributes.empty": (tree) => /^.*Directive(?:Block)? scope=.* attributes=\[\] /m.test(tree),
    "directive.attributes.value": (tree) => /^.*Directive(?:Block)? scope=.* attributes=\[.+\] /m.test(tree),
    // The dump visualizes the DirectiveLabel field as a nested Markup node:
    // absent emits no label node, empty has `children=0`, and populated owns
    // inline descendants.
    "directive.label.null": (tree) =>
        /^(.*)Directive(?:Block)? scope=[^\n]*\n(?!\1(?:\u2502|\|)?\s*(?:\u251c|\u2514)\u2500\u2500 DirectiveLabel )/m.test(
            tree
        ),
    "directive.label.empty": (tree) => /DirectiveLabel scope=\S+ children=0$/m.test(tree),
    "directive.label.populated": (tree) => /DirectiveLabel scope=\S+ children=[1-9]\d*$/m.test(tree),
    // M2: a reference occurrence is the `Link` or `Image` it names, and dumps
    // identically to a direct one apart from scope. The case holds one direct
    // and several reference occurrences of each kind, so every `Link` line and
    // every `Image` line, scope removed, must be one line.
    "reference.resolution.identical": (tree) =>
        ["Link", "Image"].every((kind) => {
            const lines = tree
                .split("\n")
                .filter((line) => new RegExp(`(?:^|\u2500 )${kind} scope=`).test(line))
                .map((line) => line.replace(/^.*?(?= scope=)/, "").replace(/ scope=\S+/, ""));
            return lines.length >= 2 && new Set(lines).size === 1;
        }),
    "link.title.null": (tree) => /^.*Link scope=.* title=null /m.test(tree),
    "link.title.empty": (tree) => /^.*Link scope=.* title="" /m.test(tree),
    "link.title.value": (tree) => /^.*Link scope=.* title=".+" /m.test(tree),
    "image.title.null": (tree) => /^.*Image scope=.* title=null /m.test(tree),
    "image.title.value": (tree) => /^.*Image scope=.* title=".+" /m.test(tree),
    "scope.positive": (tree) => / scope=[1-9]\d*:[1-9]\d*\.\./.test(tree),
    /* `scope.zero` was here, and it required the canonical corpus to demonstrate
       a node with NO position -- 0:0..0:0. Its only two witnesses in that corpus
       were the LineBreak and the SoftBreak in inlines.ast, and 0a.12b gave both
       of them a real position (D26). The remaining producers of that shape are
       D13's empty Text and the split-off table lead, and pinning either as
       canonical coverage would bless a defect the stage is closing -- which is
       a defect rather than canonical behavior. The state is therefore deleted
       rather than re-witnessed. This is a coverage obligation,
       not a grammar or schema change: the dump still permits 0:0..0:0, so no
       binding and no golden format moves. */
    "children.empty": (tree) => / children=0(?:\n|$)/.test(tree),
    "children.populated": (tree) => / children=[1-9]\d*(?:\n|$)/.test(tree),
    "escaping.empty-string": (tree) => /=""/.test(tree),
    "escaping.newline": (tree) => /\\n/.test(tree),
    // An attribute VALUE that contains a quote. It was called `escaping.json`
    // when the whole attribute map was one JSON string; the escaping it checks
    // is the dump's, and that is what it was always about.
    "escaping.attribute-value": (tree) => /attributes=\[[^\]]*="[^\]]*\\"/.test(tree),
    // `Comment` is the one kind valid in both block and inline content, and
    // the parent edge is what records which (M0).
    "comment.placement.block": (tree) =>
        parentEdges(tree).some((edge) => edge.kind === "Comment" && BLOCK_CONTENT.has(edge.parent)),
    "comment.placement.inline": (tree) =>
        parentEdges(tree).some((edge) => edge.kind === "Comment" && INLINE_CONTENT.has(edge.parent)),
    // `dest` is the tagged `Destination` value (M1). `[a]()` wrote a
    // destination and wrote nothing in it, so the empty branch is a state of
    // its own and not an absence.
    "destination.url.empty": (tree) => / dest=url\(""\) /.test(tree),
    "destination.url.value": (tree) => / dest=url\("(?:\\.|[^"\\])+"\) /.test(tree),
    // The citation model (M4): a `Citation` is a value line under its `Cite`
    // with a tagged referent, its affixes are groups printed even when
    // empty, and a `Footnote` is a value line under `Document` after the
    // content, or absent.
    "citation.referent.footnote": (tree) =>
        /^.*Citation scope=\S+ referent=footnote\(id="[^"]*"\) children=0$/m.test(tree),
    "citation.affix.empty": (tree) => /CitationPrefix children=0\n.*CitationSuffix children=0(?:\n|$)/.test(tree),
    "document.specimens.empty": (tree) =>
        tree.startsWith("Document scope=") && !/^(?:├──|└──) Specimen scope=/m.test(tree),
    "document.footnotes.empty": (tree) =>
        tree.startsWith("Document scope=") && !/^(?:├──|└──) Footnote scope=/m.test(tree),
    "document.footnotes.populated": (tree) => /^(?:├──|└──) Footnote scope=\S+ id="[^"]*" children=\d+$/m.test(tree)
};
const orderValidators = {
    "document.source-order": (tree) => tree.startsWith("Document scope="),
    "table.head-content-foot": (tree) =>
        /TableHead children=\d+[\s\S]*TableBody children=\d+[\s\S]*TableFoot children=\d+/.test(tree),
    "directive.label-before-content": (tree) =>
        /DirectiveBlock scope=.* children=[1-9]\d*\n[\s\S]*DirectiveLabel scope=[\s\S]*Paragraph scope=/.test(tree),
    "directive.attributes.source-order": (tree) =>
        /DirectiveBlock scope=.*attributes=\[properties=".*" metadata=".*"\]/.test(tree),
    "inline.source-order": (tree) => /Paragraph scope=.* children=[2-9]\d*/.test(tree),
    // Every `Footnote` value nests under `Document` after the last content
    // line (M4).
    "document.content-before-footnotes": (tree) => {
        const top = tree
            .split("\n")
            .filter((line) => /^(?:├──|└──) /.test(line))
            .map((line) => line.slice(4).split(" ", 1)[0]);
        const first = top.indexOf("Footnote");
        return first >= 0 && top.slice(first).every((kind) => kind === "Footnote");
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
                if (match === null) continue;
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
const GROUPS = new Set(["Title", "CitationPrefix", "CitationSuffix", "TableHead", "TableBody", "TableFoot"]);
// A scoped value prints as a value line -- scope, its scalar fields, children
// -- without being a kind (M4).
const scopedValues = Object.fromEntries(
    Object.entries(contract.values ?? {})
        .filter(([, value]) => value.scoped)
        .map(([name, value]) => [name, value.fields])
);

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
        const value = kind in scopedValues;
        if (!value) {
            actualKinds.add(kind);
            for (const field of fieldsByKind[kind] ?? []) allObservedFields.add(`${kind}.${field}`);
        }
        // Strings first, then bracketed groups: `attributes=[a="1" b="2"]` is
        // ONE field, and without the second pass ` b=` reads as a second one.
        const lineWithoutStrings = line.replace(/"(?:\\.|[^"\\])*"/g, '""').replace(/=\[[^\]]*\]/g, "=[]");
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
        const isNodeValuedField = (type) =>
            [...type.matchAll(/[A-Za-z]+/g)].some(
                (word) => word[0] === "Markup" || kindNames.has(word[0]) || word[0] in scopedValues
            );
        const dumpFields = Object.fromEntries(
            [...contract.kinds, ...Object.entries(scopedValues).map(([name, fields]) => ({ name, fields }))].map(
                (kind) => [
                    kind.name,
                    kind.fields.filter((field) => !isNodeValuedField(field.type)).map((field) => field.name)
                ]
            )
        );
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
