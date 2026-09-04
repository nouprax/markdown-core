#!/usr/bin/env node
/**
 * Obsidian-flavored Markdown supplementary parity gate.
 *
 * The official Obsidian help snapshot owns the language. This gate executes
 * the most-used current npm OFM parser over the intersection it implements.
 * A separately pinned frontmatter/YAML pair supplies executable evidence for
 * Obsidian Properties. The gate compares a scope-free semantic tree and keeps
 * every current product gap explicit and fail-closed.
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import remarkObsidian from "@quartz-community/remark-obsidian";
import matter from "gray-matter";
import { JSON_SCHEMA, load as loadYaml } from "js-yaml";
import remarkGfm from "remark-gfm";
import remarkParse from "remark-parse";
import { unified } from "unified";

import { readExamples } from "./lib/fixture-corpus.mjs";
import { parseCanonicalDump, parseCanonicalFields } from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policyPath = "specs/oracles/obsidian/deltas.json";
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));
const verbose = process.argv.includes("--verbose");
const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");

if (
    policy.metadataOracle.options.language !== "yaml" ||
    policy.metadataOracle.options.delimiters !== "---" ||
    policy.metadataOracle.options.schema !== "JSON_SCHEMA"
) {
    process.stderr.write(`obsidian parity: invalid Properties oracle options in ${policyPath}\n`);
    process.exit(1);
}

if (!fs.existsSync(ours)) {
    process.stderr.write(`obsidian parity: missing ${path.relative(root, ours)}\nBuild it with: pnpm build:c\n`);
    process.exit(1);
}

const require = createRequire(import.meta.url);
for (const expected of [policy.oracle, policy.metadataOracle.frontmatter, policy.metadataOracle.yaml]) {
    const installedPackage = JSON.parse(fs.readFileSync(require.resolve(`${expected.package}/package.json`), "utf8"));
    if (installedPackage.version !== expected.version) {
        process.stderr.write(
            `obsidian parity: policy pins ${expected.package}@${expected.version}, ` +
                `but ${installedPackage.version} is installed\nRun: pnpm install --frozen-lockfile\n`
        );
        process.exit(1);
    }
}

const processor = unified().use(remarkParse).use(remarkGfm).use(remarkObsidian, policy.oracle.options);

const mdastKinds = {
    root: "Document",
    heading: "Heading",
    paragraph: "Paragraph",
    blockquote: "Callout",
    list: "List",
    listItem: "ListItem",
    text: "Text",
    emphasis: "Emphasis",
    strong: "Strong",
    delete: "Strikethrough",
    inlineCode: "Code",
    code: "CodeBlock",
    link: "Link",
    image: "Image",
    break: "LineBreak",
    thematicBreak: "ThematicBreak",
    wikilink: "CrossLink",
    highlight: "Mark"
};

const comparedFields = {
    Heading: ["level"],
    Text: ["literal"],
    List: ["flavor", "start", "tight"],
    ListItem: ["marker"],
    Code: ["literal"],
    CodeBlock: ["literal", "info"],
    Link: ["dest", "title"],
    Image: ["dest", "title"],
    CrossLink: ["embedded", "dest", "label"]
};

function urlDestination(value) {
    return { kind: "url", value };
}

function crossDestination(path, anchor) {
    return { kind: "cross", path, anchor };
}

function metadataScalar(value) {
    if (value === null) return { kind: "null" };
    if (typeof value === "boolean") return { kind: "bool", value };
    if (typeof value === "number" && Number.isFinite(value)) {
        return { kind: "number", value: String(value) };
    }
    if (typeof value === "string" && !/[\r\n]/.test(value)) return { kind: "text", value };
    throw new Error(`unsupported Properties scalar: ${Object.prototype.toString.call(value)}`);
}

function metadataValue(value) {
    if (!Array.isArray(value)) return { kind: "scalar", value: metadataScalar(value) };
    const values = value.map(metadataScalar);
    if (values.some((item) => item.kind !== "number" && item.kind !== "text")) {
        throw new Error("Properties lists contain only text and number items");
    }
    return { kind: "list", values };
}

function projectMetadata(value) {
    if (value === undefined) return [];
    if (typeof value !== "object" || Array.isArray(value)) {
        throw new Error("Properties payload is not a top-level mapping");
    }
    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
        throw new Error("Properties payload has a non-data object prototype");
    }
    return Object.entries(value).map(([name, recordValue]) => {
        if (name.length === 0 || /[\r\n]/.test(name)) throw new Error("Properties name is empty or multiline");
        return { name, value: metadataValue(recordValue) };
    });
}

function sourceLine(source, start) {
    const newline = source.indexOf("\n", start);
    const physicalEnd = newline < 0 ? source.length : newline;
    const contentEnd = physicalEnd > start && source[physicalEnd - 1] === "\r" ? physicalEnd - 1 : physicalEnd;
    return {
        text: source.slice(start, contentEnd),
        next: newline < 0 ? source.length : newline + 1,
        terminated: newline >= 0
    };
}

function officialPropertiesEnvelope(source) {
    const start = source.startsWith("\uFEFF") ? 1 : 0;
    const opening = sourceLine(source, start);
    if (opening.text !== "---" || !opening.terminated) return null;

    let cursor = opening.next;
    while (cursor <= source.length) {
        const line = sourceLine(source, cursor);
        if (line.text === "---") {
            return { start, bodyStart: line.next };
        }
        if (!line.terminated) return null;
        cursor = line.next;
    }
    return null;
}

function parseProperties(source) {
    const envelope = officialPropertiesEnvelope(source);
    if (envelope === null) return { content: source, metadata: null };

    const withoutBom = source.slice(envelope.start);
    const undecoded = Symbol("undecoded Properties payload");
    let decoded = undecoded;
    const parsed = matter(withoutBom, {
        language: policy.metadataOracle.options.language,
        delimiters: policy.metadataOracle.options.delimiters,
        engines: {
            yaml: (payload) => {
                decoded = loadYaml(payload, {
                    schema: JSON_SCHEMA,
                    json: false
                });
                return decoded;
            }
        }
    });
    const expectedBody = source.slice(envelope.bodyStart);
    if (parsed.content !== expectedBody) {
        throw new Error("gray-matter did not agree with the official exact-fence envelope");
    }
    if (decoded === undecoded) {
        if (!parsed.isEmpty) throw new Error("gray-matter did not invoke the configured YAML engine");
        decoded = undefined;
    }
    return { content: parsed.content, metadata: projectMetadata(decoded) };
}

function parseJsonString(source, start) {
    if (source[start] !== '"') return null;
    let escaped = false;
    for (let cursor = start + 1; cursor < source.length; cursor++) {
        const character = source[cursor];
        if (escaped) escaped = false;
        else if (character === "\\") escaped = true;
        else if (character === '"') {
            const raw = source.slice(start, cursor + 1);
            return { value: JSON.parse(raw), end: cursor + 1 };
        }
    }
    return null;
}

function parseDestination(raw) {
    let cursor = 0;
    const skipSpace = () => {
        while (/\s/.test(raw[cursor] ?? "")) cursor++;
    };
    const consume = (token) => {
        skipSpace();
        if (!raw.startsWith(token, cursor)) return false;
        cursor += token.length;
        return true;
    };
    const string = () => {
        skipSpace();
        const parsed = parseJsonString(raw, cursor);
        if (parsed) cursor = parsed.end;
        return parsed?.value;
    };
    const complete = () => {
        skipSpace();
        return cursor === raw.length;
    };

    if (consume("url(")) {
        const value = string();
        if (value === undefined || !consume(")") || !complete()) return null;
        return urlDestination(value);
    }

    cursor = 0;
    if (!consume("cross(") || !consume("path=")) return null;
    const path = string();
    if (path === undefined || !consume(",") || !consume("anchor=")) return null;
    skipSpace();
    let anchor;
    if (raw.startsWith("null", cursor)) {
        cursor += 4;
        anchor = null;
    } else {
        anchor = string();
        if (anchor === undefined) return null;
    }
    if (!consume(")") || !complete()) return null;
    return crossDestination(path, anchor);
}

function normalizeChildren(children) {
    const result = [];
    for (const child of children) {
        const previous = result[result.length - 1];
        if (previous?.kind === "Text" && child.kind === "Text") {
            previous.fields.literal += child.fields.literal;
        } else {
            result.push(child);
        }
    }
    return result;
}

function fromMdast(node, unknown, source) {
    const kind = mdastKinds[node.type];
    if (!kind) {
        unknown.add(node.type);
        return { kind: `?${node.type}`, fields: {}, children: [] };
    }

    const fields = {};
    if (node.type === "text") fields.literal = node.value;
    if (node.type === "heading") fields.level = String(node.depth);
    if (node.type === "list") {
        fields.flavor = node.ordered ? "ordered" : "bullet";
        fields.start = node.ordered ? String(node.start ?? 1) : "null";
        fields.tight = String(!node.spread);
    }
    if (node.type === "listItem") {
        fields.marker = node.data?.taskChar ?? "null";
    }
    if (node.type === "inlineCode") fields.literal = node.value;
    if (node.type === "code") {
        fields.literal = `${node.value ?? ""}\n`;
        fields.info = node.lang ?? "null";
    }
    if (node.type === "link") {
        fields.dest = urlDestination(node.url);
        fields.title = node.title ?? "null";
    }
    if (node.type === "image") {
        fields.dest = urlDestination(node.url);
        fields.title = node.title ?? "null";
    }
    if (node.type === "wikilink") {
        const spelling = source.slice(node.position?.start.offset ?? 0, node.position?.end.offset ?? 0);
        const bodyStart = spelling.startsWith("![[") ? 3 : 2;
        const hasLabelDelimiter = spelling.slice(bodyStart, -2).includes("|");
        fields.embedded = String(node.embedded);
        fields.dest = crossDestination(node.path, node.heading ? node.heading.replace(/^\^/, "") : null);
        fields.label = node.alias === "" && !hasLabelDelimiter ? "null" : node.alias;
    }

    const children =
        node.type === "image" && node.alt
            ? [{ kind: "Text", fields: { literal: node.alt }, children: [] }]
            : normalizeChildren((node.children ?? []).map((child) => fromMdast(child, unknown, source)));

    return {
        kind,
        fields,
        children
    };
}

function parseMetadataDump(value) {
    if (value === undefined || value === "null") return null;
    const parsed = JSON.parse(String(value));
    if (!Array.isArray(parsed)) throw new Error("canonical metadata field is not an array or null");
    return parsed;
}

function parseCanonicalDumpWithRootFields(dump) {
    const tree = parseCanonicalDump(dump);
    tree.fields = parseCanonicalFields(dump.split("\n", 1)[0]?.trim() ?? "Document");
    return tree;
}

function fromMarkdownCore(node, includeMetadata = false) {
    const fields = {};
    for (const name of comparedFields[node.kind] ?? []) {
        let value = node.fields[name];
        if ((node.kind === "Link" || node.kind === "Image") && name === "dest" && value == null) {
            const legacyName = node.kind === "Link" ? "destination" : "source";
            value = urlDestination(String(node.fields[legacyName] ?? ""));
        } else if (name === "dest" && value != null) {
            value = parseDestination(String(value));
            if (value == null) throw new Error(`invalid canonical destination: ${String(node.fields[name])}`);
        }
        fields[name] = value ?? (name === "literal" ? "" : "null");
    }
    if (node.kind === "Document" && includeMetadata) {
        fields.metadata = parseMetadataDump(node.fields.metadata);
    }
    return {
        kind: node.kind,
        fields,
        children: normalizeChildren(node.children.map((child) => fromMarkdownCore(child)))
    };
}

function render(node, depth = 0) {
    const fields = Object.entries(node.fields)
        .map(([name, value]) => ` ${name}=${JSON.stringify(value)}`)
        .join("");
    const lines = [`${"  ".repeat(depth)}${node.kind}${fields}`];
    for (const child of node.children) lines.push(render(child, depth + 1));
    return lines.join("\n");
}

function digest(value) {
    return createHash("sha256").update(value).digest("hex");
}

function compare(input) {
    const unknown = new Set();
    const properties = parseProperties(input);
    const parsed = processor.parse(properties.content);
    const transformed = processor.runSync(parsed, properties.content);
    const oracleTree = fromMdast(transformed, unknown, properties.content);
    if (properties.metadata !== null) oracleTree.fields.metadata = properties.metadata;
    const ourTree = fromMarkdownCore(
        parseCanonicalDumpWithRootFields(
            execFileSync(ours, ["--profile", "gfm-extended"], {
                input,
                encoding: "utf8",
                maxBuffer: 1 << 24
            })
        ),
        properties.metadata !== null
    );
    return { oracle: render(oracleTree), ours: render(ourTree), unknown };
}

function containsKind(tree, kind) {
    if (tree.type === kind) return true;
    return (tree.children ?? []).some((child) => containsKind(child, kind));
}

// Agreement is meaningless unless the selected parser demonstrably recognizes
// the syntax under test. These canaries exercise registration and transforms,
// including comment removal and the custom-task source lookup.
for (const [input, expectedKind] of [
    ["[[Note]]\n", "wikilink"],
    ["==marked==\n", "highlight"]
]) {
    const tree = processor.runSync(processor.parse(input), input);
    if (!containsKind(tree, expectedKind)) {
        process.stderr.write(`obsidian parity: oracle canary did not produce ${expectedKind}\n`);
        process.exit(1);
    }
}
const commentCanary = processor.runSync(processor.parse("%%hidden%%\n"), "%%hidden%%\n");
if (commentCanary.children.length !== 0) {
    process.stderr.write("obsidian parity: oracle canary did not remove an Obsidian comment\n");
    process.exit(1);
}
const taskCanary = processor.runSync(processor.parse("- [?] task\n"), "- [?] task\n");
if (taskCanary.children[0]?.children?.[0]?.data?.taskChar !== "?") {
    process.stderr.write("obsidian parity: oracle canary did not preserve a custom task character\n");
    process.exit(1);
}

const propertiesCanary = parseProperties(
    '---\nArbitrary Name: value\nenabled: true\nitems: [one, "[[Two]]"]\ndate: 2026-09-03\n---\n# Body\n'
);
if (
    propertiesCanary.content !== "# Body\n" ||
    JSON.stringify(propertiesCanary.metadata) !==
        JSON.stringify([
            { name: "Arbitrary Name", value: { kind: "scalar", value: { kind: "text", value: "value" } } },
            { name: "enabled", value: { kind: "scalar", value: { kind: "bool", value: true } } },
            {
                name: "items",
                value: {
                    kind: "list",
                    values: [
                        { kind: "text", value: "one" },
                        { kind: "text", value: "[[Two]]" }
                    ]
                }
            },
            { name: "date", value: { kind: "scalar", value: { kind: "text", value: "2026-09-03" } } }
        ])
) {
    process.stderr.write("obsidian parity: Properties oracle canary produced the wrong semantic value\n");
    process.exit(1);
}
for (const nonHeader of ["text\n---\nname: value\n---\n", "---yaml\nname: value\n---\n", "--- \nname: value\n---\n"]) {
    if (parseProperties(nonHeader).metadata !== null) {
        process.stderr.write("obsidian parity: Properties oracle accepted a non-header candidate\n");
        process.exit(1);
    }
}
for (const emptyProperties of ["\uFEFF---\n---\n", "---\n   \n---\n", "---\n# note\n---\n"]) {
    if (JSON.stringify(parseProperties(emptyProperties).metadata) !== "[]") {
        process.stderr.write(
            `obsidian parity: Properties oracle rejected empty metadata ${JSON.stringify(emptyProperties)}\n`
        );
        process.exit(1);
    }
}
const propertyNameCanary = parseProperties("---\n1.5: decimal\ntrue: boolean\n---\n").metadata;
if (JSON.stringify(propertyNameCanary?.map((record) => record.name)) !== '["1.5","true"]') {
    process.stderr.write("obsidian parity: Properties oracle did not expose scalar property names as strings\n");
    process.exit(1);
}
for (const invalid of [
    "---\nnull\n---\n",
    "---\nname: one\nname: two\n---\n",
    '---\n"1": quoted\n1: plain\n---\n',
    "---\n- one\n- two\n---\n",
    "---\nname:\n  nested: value\n---\n",
    "---\nname: |\n  two lines\n---\n",
    "---\nitems: [true, null]\n---\n"
]) {
    let rejected = false;
    try {
        parseProperties(invalid);
    } catch {
        rejected = true;
    }
    if (!rejected) {
        process.stderr.write(
            `obsidian parity: Properties oracle accepted invalid projection ${JSON.stringify(invalid)}\n`
        );
        process.exit(1);
    }
}

for (const [raw, expected] of [
    ['cross(path="Folder/Note", anchor="Heading one")', crossDestination("Folder/Note", "Heading one")],
    ['cross(path="Folder/Note",anchor="Heading one")', crossDestination("Folder/Note", "Heading one")],
    ['url("path with spaces/(one)")', urlDestination("path with spaces/(one)")]
]) {
    const captured = parseCanonicalDump(`Document\nLink dest=${raw}\n`).children[0]?.fields.dest;
    const normalized = captured == null ? null : parseDestination(captured);
    if (JSON.stringify(normalized) !== JSON.stringify(expected)) {
        process.stderr.write(`obsidian parity: canonical destination parser rejected or truncated ${raw}\n`);
        process.exit(1);
    }
}
const metadataDumpCanary = [{ name: "x", value: { kind: "scalar", value: { kind: "text", value: "a b" } } }];
const capturedMetadata = parseCanonicalDumpWithRootFields(
    `Document metadata=${JSON.stringify(metadataDumpCanary)} children=0\n`
).fields.metadata;
if (JSON.stringify(parseMetadataDump(capturedMetadata)) !== JSON.stringify(metadataDumpCanary)) {
    process.stderr.write("obsidian parity: canonical metadata parser rejected or truncated a record array\n");
    process.exit(1);
}

const invalidPolicy = [];
const gapIds = new Set();
const gapInputs = new Set();
for (const gap of policy.baselineGaps) {
    if (!gap.id || gapIds.has(gap.id)) invalidPolicy.push(`duplicate or empty gap id: ${String(gap.id)}`);
    if (gapInputs.has(gap.input)) invalidPolicy.push(`duplicate gap input: ${JSON.stringify(gap.input)}`);
    if (!/^[0-9a-f]{64}$/.test(gap.oracleDigest)) invalidPolicy.push(`invalid oracle digest: ${gap.id}`);
    if (!/^[0-9a-f]{64}$/.test(gap.markdownCoreDigest)) {
        invalidPolicy.push(`invalid markdown-core digest: ${gap.id}`);
    }
    gapIds.add(gap.id);
    gapInputs.add(gap.input);
}
if (invalidPolicy.length) {
    process.stderr.write(`obsidian parity: invalid ${policyPath}\n  ${invalidPolicy.join("\n  ")}\n`);
    process.exit(1);
}

const gaps = new Map(policy.baselineGaps.map((entry) => [entry.input, entry]));
const seenGaps = new Set();
const failures = [];
const unknownKinds = new Set();
const cases = policy.corpus.flatMap((file) => readExamples(root, file));
if (cases.length === 0) {
    process.stderr.write(`obsidian parity: corpus produced no examples: ${policy.corpus.join(", ")}\n`);
    process.exit(1);
}
const duplicateCorpusInputs = cases.filter(
    (testCase, index) => cases.findIndex((candidate) => candidate.input === testCase.input) !== index
);
if (duplicateCorpusInputs.length) {
    process.stderr.write(
        `obsidian parity: corpus contains duplicate input(s): ${duplicateCorpusInputs
            .map((testCase) => JSON.stringify(testCase.input))
            .join(", ")}\n`
    );
    process.exit(1);
}

for (const testCase of cases) {
    let result;
    try {
        result = compare(testCase.input);
    } catch (error) {
        failures.push({ ...testCase, failure: String(error).slice(0, 500) });
        continue;
    }
    for (const kind of result.unknown) unknownKinds.add(kind);
    const gap = gaps.get(testCase.input);
    if (result.oracle === result.ours) {
        if (gap) failures.push({ ...testCase, settledGap: gap, ...result });
    } else if (gap) {
        const oracleDigest = digest(result.oracle);
        const markdownCoreDigest = digest(result.ours);
        if (verbose) {
            process.stdout.write(`  ${gap.id}: oracle=${oracleDigest} markdown-core=${markdownCoreDigest}\n`);
        }
        if (gap.oracleDigest !== oracleDigest || gap.markdownCoreDigest !== markdownCoreDigest) {
            failures.push({ ...testCase, changedGap: gap, oracleDigest, markdownCoreDigest, ...result });
        } else {
            seenGaps.add(testCase.input);
        }
    } else {
        failures.push({ ...testCase, ...result });
    }
}

const corpusInputs = new Set(cases.map((testCase) => testCase.input));
for (const [input, gap] of gaps) {
    if (!corpusInputs.has(input)) failures.push({ source: policyPath, input, unreachableGap: gap });
}

if (unknownKinds.size) {
    process.stderr.write(
        `obsidian parity FAILED: unmapped oracle node kind(s): ${[...unknownKinds].sort().join(", ")}\n`
    );
    process.exit(1);
}

process.stdout.write(
    `obsidian parity: ${String(cases.length)} inputs, ` +
        `${String(seenGaps.size)}/${String(gaps.size)} registered gaps reproduced\n`
);
process.stdout.write(`  oracle: ${policy.oracle.package}@${policy.oracle.version}\n`);
process.stdout.write(
    `  metadata oracle: ${policy.metadataOracle.frontmatter.package}@${policy.metadataOracle.frontmatter.version} + ` +
        `${policy.metadataOracle.yaml.package}@${policy.metadataOracle.yaml.version}\n`
);
process.stdout.write(`  corpus: ${policy.corpus.join(", ")}\n`);

if (failures.length) {
    process.stderr.write(`\nobsidian parity FAILED: ${String(failures.length)} corpus policy violation(s)\n`);
    for (const entry of failures.slice(0, verbose ? failures.length : 5)) {
        process.stderr.write(`\n  ${entry.source}\n  ${JSON.stringify(entry.input)}\n`);
        if (entry.failure) {
            process.stderr.write(`    harness error: ${entry.failure}\n`);
        } else if (entry.settledGap) {
            process.stderr.write(
                `    registered gap ${entry.settledGap.id} now agrees; remove it from ${policyPath}\n`
            );
        } else if (entry.unreachableGap) {
            process.stderr.write(
                `    registered gap ${entry.unreachableGap.id} is no longer exercised; restore or retire it explicitly\n`
            );
        } else if (entry.changedGap) {
            process.stderr.write(
                `    registered gap ${entry.changedGap.id} changed shape\n` +
                    `    oracle digest: ${entry.oracleDigest}\n` +
                    `    markdown-core digest: ${entry.markdownCoreDigest}\n`
            );
            process.stderr.write(`    --- oracle ---\n${entry.oracle.replace(/^/gm, "    ")}\n`);
            process.stderr.write(`    --- markdown-core ---\n${entry.ours.replace(/^/gm, "    ")}\n`);
        } else {
            process.stderr.write(`    --- oracle ---\n${entry.oracle.replace(/^/gm, "    ")}\n`);
            process.stderr.write(`    --- markdown-core ---\n${entry.ours.replace(/^/gm, "    ")}\n`);
        }
    }
    if (!verbose && failures.length > 5) {
        process.stderr.write(`\n  ... ${String(failures.length - 5)} more; re-run with --verbose\n`);
    }
    process.exit(1);
}

process.stdout.write("obsidian parity gate passed.\n");
