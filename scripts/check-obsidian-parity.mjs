#!/usr/bin/env node
/**
 * Obsidian-flavored Markdown supplementary parity gate.
 *
 * The official Obsidian help snapshot owns the language. This gate executes
 * the most-used current npm OFM parser over the intersection it implements.
 * A separately pinned source-preserving YAML document parser supplies
 * executable evidence for Obsidian Properties after the exact profile envelope
 * has been recognized. The gate compares a scope-free semantic tree and keeps
 * every current product gap explicit and fail-closed.
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import remarkObsidian from "@quartz-community/remark-obsidian";
import remarkGfm from "remark-gfm";
import remarkParse from "remark-parse";
import { unified } from "unified";
import { isAlias, isMap, isScalar, isSeq, parseAllDocuments } from "yaml";

import { readExamples } from "./lib/fixture-corpus.mjs";
import { parseCanonicalDump, parseCanonicalFields } from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policyPath = "specs/oracles/obsidian/deltas.json";
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));
const verbose = process.argv.includes("--verbose");
const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");

if (
    policy.metadataOracle.options.envelope !== "obsidian-exact-leading-fence" ||
    policy.metadataOracle.options.schema !== "json-scalars-with-string-fallback" ||
    policy.metadataOracle.options.version !== "1.2" ||
    policy.metadataOracle.options.keepSourceTokens !== true ||
    policy.metadataOracle.options.uniqueKeys !== false
) {
    process.stderr.write(`obsidian parity: invalid Properties oracle options in ${policyPath}\n`);
    process.exit(1);
}

if (!fs.existsSync(ours)) {
    process.stderr.write(`obsidian parity: missing ${path.relative(root, ours)}\nBuild it with: pnpm build:c\n`);
    process.exit(1);
}

const require = createRequire(import.meta.url);
for (const expected of [policy.oracle, policy.metadataOracle.yaml]) {
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

const yamlStringFallback = {
    identify: (value) => typeof value === "string",
    default: true,
    tag: "tag:yaml.org,2002:str",
    test: /^/,
    resolve: (value) => value,
    stringify: ({ value }) => JSON.stringify(value)
};

const yamlScalarTags = new Set([
    "tag:yaml.org,2002:str",
    "tag:yaml.org,2002:null",
    "tag:yaml.org,2002:bool",
    "tag:yaml.org,2002:int",
    "tag:yaml.org,2002:float"
]);
const yamlMapTags = new Set(["tag:yaml.org,2002:map"]);
const yamlSequenceTags = new Set(["tag:yaml.org,2002:seq"]);

function jsonScalarsWithStringFallback(tags) {
    const unresolved = tags.findIndex((tag) => tag.tag === "");
    if (unresolved < 0) throw new Error("yaml JSON schema no longer exposes its unresolved-scalar fallback");
    return [...tags.slice(0, unresolved), yamlStringFallback, ...tags.slice(unresolved + 1)];
}

function assertSupportedTag(node, allowed) {
    if (node.tag !== undefined && !allowed.has(node.tag)) {
        throw new Error(`unsupported Properties YAML tag: ${node.tag}`);
    }
}

function resolveMetadataAlias(node, context, project) {
    if (!isAlias(node)) return project(node, context);
    if (context.activeAliases.has(node)) throw new Error("cyclic Properties alias");
    const resolved = node.resolve(context.document);
    if (resolved === undefined) throw new Error(`undefined Properties alias: ${node.source}`);
    context.activeAliases.add(node);
    try {
        return project(resolved, context);
    } finally {
        context.activeAliases.delete(node);
    }
}

function projectMetadataScalar(node, context) {
    return resolveMetadataAlias(node, context, (resolved) => {
        if (!isScalar(resolved)) throw new Error("Properties value is not a scalar");
        assertSupportedTag(resolved, yamlScalarTags);

        const value = resolved.value;
        if (resolved.type === "PLAIN" && resolved.source === "" && value === "") return { kind: "null" };
        if (resolved.tag === "tag:yaml.org,2002:str") {
            if (typeof value !== "string" || /[\r\n]/.test(value)) {
                throw new Error("Properties text is multiline");
            }
            return { kind: "text", value };
        }
        if (resolved.tag === "tag:yaml.org,2002:null") return { kind: "null" };
        if (resolved.tag === "tag:yaml.org,2002:bool") {
            if (typeof value !== "boolean") throw new Error("invalid Properties boolean");
            return { kind: "bool", value };
        }
        if (
            resolved.tag === "tag:yaml.org,2002:int" ||
            resolved.tag === "tag:yaml.org,2002:float" ||
            typeof value === "number" ||
            typeof value === "bigint"
        ) {
            if (typeof value === "number" && !Number.isFinite(value)) {
                throw new Error("non-finite Properties number");
            }
            if (typeof resolved.source !== "string") throw new Error("Properties number has no source lexeme");
            return { kind: "number", value: resolved.source };
        }
        if (value === null) return { kind: "null" };
        if (typeof value === "boolean") return { kind: "bool", value };
        if (typeof value === "string" && !/[\r\n]/.test(value)) return { kind: "text", value };
        throw new Error(`unsupported Properties scalar: ${Object.prototype.toString.call(value)}`);
    });
}

function projectMetadataValue(node, context) {
    if (node === null) return { kind: "scalar", value: { kind: "null" } };
    return resolveMetadataAlias(node, context, (resolved) => {
        if (!isSeq(resolved)) return { kind: "scalar", value: projectMetadataScalar(resolved, context) };
        assertSupportedTag(resolved, yamlSequenceTags);
        const values = resolved.items.map((item) => {
            if (item === null) throw new Error("Properties lists contain no empty items");
            const value = projectMetadataScalar(item, context);
            if (value.kind !== "number" && value.kind !== "text") {
                throw new Error("Properties lists contain only text and number items");
            }
            return value;
        });
        return { kind: "list", values };
    });
}

function propertyName(node) {
    if (!isScalar(node)) throw new Error("Properties name is not a directly authored scalar");
    assertSupportedTag(node, yamlScalarTags);
    if (node.type !== "PLAIN" && node.type !== "QUOTE_SINGLE" && node.type !== "QUOTE_DOUBLE") {
        throw new Error("Properties name uses an unsupported scalar style");
    }
    const name = node.source;
    if (typeof name !== "string" || name.length === 0 || /[\r\n]/.test(name)) {
        throw new Error("Properties name is empty or multiline");
    }
    return name;
}

function cstRange(value, range = { start: Infinity, end: -Infinity }) {
    if (Array.isArray(value)) {
        for (const item of value) cstRange(item, range);
    } else if (value !== null && typeof value === "object") {
        if (Number.isInteger(value.offset) && typeof value.source === "string") {
            range.start = Math.min(range.start, value.offset);
            range.end = Math.max(range.end, value.offset + value.source.length);
        }
        for (const child of Object.values(value)) cstRange(child, range);
    }
    return range;
}

function projectMetadata(document, payloadStart) {
    if (document.errors.length > 0 || document.warnings.length > 0) {
        throw new Error(document.errors[0]?.message ?? document.warnings[0]?.message ?? "invalid Properties YAML");
    }
    if (document.directives.docStart || document.directives.docEnd || document.directives.yaml.explicit) {
        throw new Error("Properties payload contains a YAML stream or document indicator");
    }
    if (document.contents === null) return { records: [], recordRanges: [] };
    if (!isMap(document.contents)) throw new Error("Properties payload is not a top-level mapping");
    assertSupportedTag(document.contents, yamlMapTags);

    const context = { document, activeAliases: new Set() };
    const names = new Set();
    const records = [];
    const recordRanges = [];
    for (const pair of document.contents.items) {
        const name = propertyName(pair.key);
        if (names.has(name)) throw new Error(`duplicate Properties name: ${name}`);
        names.add(name);
        records.push({ name, value: projectMetadataValue(pair.value, context) });

        const relative = cstRange(pair.srcToken);
        if (!Number.isFinite(relative.start) || !Number.isFinite(relative.end)) {
            throw new Error(`Properties record has no source range: ${name}`);
        }
        recordRanges.push([payloadStart + relative.start, payloadStart + relative.end]);
    }
    return { records, recordRanges };
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
            return {
                start,
                payloadStart: opening.next,
                payloadEnd: cursor,
                metadataEnd: cursor + 3,
                bodyStart: line.next
            };
        }
        if (!line.terminated) return null;
        cursor = line.next;
    }
    return null;
}

function parseProperties(source) {
    const envelope = officialPropertiesEnvelope(source);
    if (envelope === null) return { content: source, metadata: null };

    const payload = source.slice(envelope.payloadStart, envelope.payloadEnd);
    const documents = parseAllDocuments(payload, {
        schema: "json",
        customTags: jsonScalarsWithStringFallback,
        keepSourceTokens: true,
        uniqueKeys: false,
        resolveKnownTags: false,
        version: "1.2",
        strict: true,
        logLevel: "silent"
    });
    if (documents.length > 1) throw new Error("Properties payload contains multiple YAML documents");
    const projected =
        documents.length === 0
            ? { records: [], recordRanges: [] }
            : projectMetadata(documents[0], envelope.payloadStart);
    return {
        content: source.slice(envelope.bodyStart),
        metadata: projected.records,
        evidence: {
            metadataRange: [envelope.start, envelope.metadataEnd],
            recordRanges: projected.recordRanges
        }
    };
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
for (const emptyProperties of [
    "\uFEFF---\n---\n",
    "---\n   \n---\n",
    "---\n# note\n---\n",
    "---\n#\n---\n",
    "---\n  #\n---\n",
    "---\n# first\n\t# second\n---\n",
    "---\r\n#\r\n---\r\n"
]) {
    if (JSON.stringify(parseProperties(emptyProperties).metadata) !== "[]") {
        process.stderr.write(
            `obsidian parity: Properties oracle rejected empty metadata ${JSON.stringify(emptyProperties)}\n`
        );
        process.exit(1);
    }
}
const sourceFaithfulProperties = parseProperties(
    '---\nzeta: last\n1: &large 9007199254740993\n1.0: 1.0\n1e2: 1e2\n-0: -0\n~: tilde\ntrue: boolean\n"escaped\\u0020name": *large\n---\n'
);
if (
    JSON.stringify(sourceFaithfulProperties.metadata?.map((record) => record.name)) !==
    '["zeta","1","1.0","1e2","-0","~","true","escaped name"]'
) {
    process.stderr.write("obsidian parity: Properties oracle did not preserve decoded names in source order\n");
    process.exit(1);
}
if (
    JSON.stringify(
        sourceFaithfulProperties.metadata
            ?.filter((record) => ["1", "1.0", "1e2", "-0", "escaped name"].includes(record.name))
            .map((record) => record.value.value.value)
    ) !== '["9007199254740993","1.0","1e2","-0","9007199254740993"]'
) {
    process.stderr.write("obsidian parity: Properties oracle lost exact numeric or alias value evidence\n");
    process.exit(1);
}
const rangedSource = "---\nfirst: one\nitems:\n  - two\n  - 3.0\n---\nBody\n";
const rangedProperties = parseProperties(rangedSource);
const rangedClosing = rangedSource.lastIndexOf("---\n");
if (
    rangedProperties.evidence?.metadataRange[0] !== 0 ||
    rangedProperties.evidence.metadataRange[1] !== rangedClosing + 3 ||
    rangedProperties.evidence.recordRanges.length !== 2 ||
    rangedProperties.evidence.recordRanges.some(
        ([start, end], index, ranges) =>
            start < 4 ||
            end > rangedClosing ||
            start >= end ||
            (index > 0 && start < ranges[index - 1][1]) ||
            !rangedSource.slice(start, end).includes(index === 0 ? "first" : "items")
    )
) {
    process.stderr.write("obsidian parity: Properties oracle lost source-token range evidence\n");
    process.exit(1);
}
const taggedProperties = parseProperties("---\ntext: !!str 1\nnumber: !!float 1.0\n---\n").metadata;
if (
    JSON.stringify(taggedProperties) !==
    '[{"name":"text","value":{"kind":"scalar","value":{"kind":"text","value":"1"}}},{"name":"number","value":{"kind":"scalar","value":{"kind":"number","value":"1.0"}}}]'
) {
    process.stderr.write("obsidian parity: Properties oracle lost supported standard scalar tags\n");
    process.exit(1);
}
for (const invalid of [
    "---\nnull\n---\n",
    "---\nname: one\nname: two\n---\n",
    '---\n"1": quoted\n1: plain\n---\n',
    "---\n- one\n- two\n---\n",
    "---\nname:\n  nested: value\n---\n",
    "---\nname: |\n  two lines\n---\n",
    "---\nitems: [true, null]\n---\n",
    "---\n[a, b]: value\n---\n",
    "---\nfirst: &name value\n*name: alias key\n---\n",
    "---\nname: !application value\n---\n",
    "---\nname: *missing\n---\n",
    "---\nname: &loop [*loop]\n---\n",
    "---\n...\n---\n",
    "---\nfirst: one\n...\n---\n",
    "---\nfirst: one\n...\nsecond: two\n---\n"
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
    `  metadata oracle: ${policy.metadataOracle.yaml.package}@${policy.metadataOracle.yaml.version}\n`
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
