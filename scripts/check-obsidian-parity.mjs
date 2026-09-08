#!/usr/bin/env node
/**
 * Obsidian-flavored Markdown supplementary parity gate.
 *
 * The official Obsidian help snapshot owns the language. This gate executes
 * the most-used current npm OFM parser over the intersection it implements.
 * A separately pinned source-preserving YAML document parser supplies
 * executable evidence for Obsidian Properties after the exact profile envelope
 * has been recognized. The gate compares a scope-free semantic tree and keeps
 * unfinished features and deliberate syntax differences explicit and fail-closed.
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
import { crossDestination, parseCanonicalDump, parseDestination, urlDestination } from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policyPath = "specs/oracles/obsidian/deltas.json";
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));
const verbose = process.argv.includes("--verbose");
// The conformance harness, not the installed CLI: the dialect has no switches,
// and the layer this gate judges is selected through the harness alone.
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

const yamlStringFallback = {
    identify: (value) => typeof value === "string",
    default: true,
    tag: "tag:yaml.org,2002:str",
    test: /^/,
    resolve: (value) => value,
    stringify: ({ value }) => JSON.stringify(value)
};

const metadataFields = new Set([
    "name",
    "title",
    "subtitle",
    "time",
    "date",
    "authors",
    "keywords",
    "abstract",
    "state",
    "comment"
]);

function jsonScalarsWithStringFallback(tags) {
    const unresolved = tags.findIndex((tag) => tag.tag === "");
    if (unresolved < 0) throw new Error("yaml JSON schema no longer exposes its unresolved-scalar fallback");
    return [...tags.slice(0, unresolved), yamlStringFallback, ...tags.slice(unresolved + 1)];
}

function assertDirectNode(node) {
    if (node && (isAlias(node) || node.anchor !== undefined || node.tag !== undefined)) {
        throw new Error("anchor, alias or tag is outside the metadata grammar");
    }
}

function singleLineScalar(node) {
    assertDirectNode(node);
    if (!isScalar(node) || !["PLAIN", "QUOTE_SINGLE", "QUOTE_DOUBLE"].includes(node.type)) {
        throw new Error("unsupported single-line scalar form");
    }
    const raw = node.srcToken?.source ?? "";
    if (/[\r\n]/.test(raw) || (typeof node.value === "string" && /[\r\n]/.test(node.value))) {
        throw new Error("multiline scalar is outside the single-line grammar");
    }
    if (node.type === "QUOTE_DOUBLE") JSON.parse(raw); // Only JSON escapes, no YAML escape repertoire.
}

function projectMetadataScalar(node, allowLiteral = false) {
    assertDirectNode(node);
    if (allowLiteral && isScalar(node) && node.type === "BLOCK_LITERAL") {
        if (node.srcToken?.props?.find((token) => token.type === "block-scalar-header")?.source !== "|") {
            throw new Error("only bare literal | is supported");
        }
        return { kind: "text", value: node.value };
    }
    singleLineScalar(node);
    const value = node.value;
    if (node.type === "PLAIN" && node.source === "" && value === "") return { kind: "null" };
    if (value === null) return { kind: "null" };
    if (typeof value === "boolean") return { kind: "bool", value };
    if (typeof value === "number" || typeof value === "bigint") return { kind: "number", value: node.source };
    if (typeof value === "string") return { kind: "text", value };
    throw new Error("unsupported metadata scalar");
}

function projectMetadataValue(node, name) {
    assertDirectNode(node);
    if (node === null) return { kind: "scalar", value: { kind: "null" } };
    if (!isSeq(node)) {
        return { kind: "scalar", value: projectMetadataScalar(node, name === "abstract" || name === "comment") };
    }
    return {
        kind: "list",
        values: node.items.map((item) => {
            if (item === null) throw new Error("empty list item");
            const value = projectMetadataScalar(item);
            if (value.kind !== "text" && value.kind !== "number") throw new Error("unsupported list item");
            return value;
        })
    };
}

function propertyName(node) {
    singleLineScalar(node);
    if (!metadataFields.has(node.source)) throw new Error("unknown metadata field");
    return node.source;
}

function projectMetadata(document) {
    if (document.errors.length > 0 || document.warnings.length > 0) {
        throw new Error(document.errors[0]?.message ?? document.warnings[0]?.message ?? "invalid Properties YAML");
    }
    if (document.directives.docStart || document.directives.docEnd || document.directives.yaml.explicit) {
        throw new Error("Properties payload contains a YAML stream or document indicator");
    }
    const fields = Object.fromEntries([...metadataFields].map((name) => [name, null]));
    if (document.contents === null) return fields;
    if (!isMap(document.contents)) throw new Error("Properties payload is not a top-level mapping");
    assertDirectNode(document.contents);
    if (document.contents.flow) throw new Error("metadata supports field lines, not root objects");
    const names = new Set();
    for (const pair of document.contents.items) {
        if (pair.srcToken?.start?.some((token) => token.type === "explicit-key-ind")) {
            throw new Error("explicit keys are unsupported");
        }
        const name = propertyName(pair.key);
        if (names.has(name)) throw new Error(`duplicate Properties name: ${name}`);
        names.add(name);
        fields[name] = projectMetadataValue(pair.value, name);
    }
    return fields;
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
    return {
        content: source.slice(envelope.bodyStart),
        metadata:
            documents.length === 0
                ? Object.fromEntries([...metadataFields].map((name) => [name, null]))
                : projectMetadata(documents[0]),
        evidence: { metadataRange: [envelope.start, envelope.metadataEnd] }
    };
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

function parseMetadataValue(text) {
    const scalar = /^scalar\(([\s\S]*)\)$/.exec(text);
    const atom = (value) => {
        if (value === "null") return { kind: "null" };
        const branch = /^(bool|number|text)\(([\s\S]*)\)$/.exec(value);
        if (!branch) throw new Error("invalid metadata atom");
        return { kind: branch[1], value: JSON.parse(branch[2]) };
    };
    if (scalar) return { kind: "scalar", value: atom(scalar[1]) };
    if (!text.startsWith("list([") || !text.endsWith("])")) throw new Error("invalid metadata list");
    const body = text.slice(6, -2),
        items = [];
    const pattern = /(number|text)\(("(?:\\.|[^"\\])*")\)(?:,|$)/gy;
    let end = 0,
        match;
    while ((match = pattern.exec(body))) {
        items.push({ kind: match[1], value: JSON.parse(match[2]) });
        end = pattern.lastIndex;
    }
    if (end !== body.length) throw new Error("invalid metadata list item");
    return { kind: "list", values: items };
}
function parseMetadataDump(node) {
    const metadata = node.children.filter((child) => child.kind === "Metadata");
    if (!metadata.length) return null;
    if (metadata.length !== 1) throw new Error("multiple metadata values");
    if (metadata[0].children.length) throw new Error("metadata has no child records");
    return Object.fromEntries(
        [...metadataFields].map((name) => {
            const value = metadata[0].fields[name];
            if (value === undefined) throw new Error(`missing metadata field ${name}`);
            return [name, value === null || value === "null" ? null : parseMetadataValue(value)];
        })
    );
}

// O3: the `comment-removal` projection. The oracle removes a `%%` comment
// while parsing and drops a paragraph the removal leaves empty; the product
// keeps every comment as a `Comment` node. Both trees are compared without
// the comments, so the intersection judged is what surrounds them.
function withoutComments(children) {
    const kept = [];
    for (const child of children) {
        if (child.kind === "Comment") continue;
        if (child.kind === "Paragraph" && child.children.length === 0) continue;
        kept.push(child);
    }
    return kept;
}

function fromMarkdownCore(node, includeMetadata = false) {
    const fields = {};
    for (const name of comparedFields[node.kind] ?? []) {
        let value = node.fields[name];
        if (name === "dest") {
            value = parseDestination(String(value));
            if (value == null) throw new Error(`invalid canonical destination: ${String(node.fields[name])}`);
        }
        fields[name] = value ?? (name === "literal" ? "" : "null");
    }
    if (node.kind === "Document" && includeMetadata) {
        fields.metadata = parseMetadataDump(node);
    }
    return {
        kind: node.kind,
        fields,
        children: normalizeChildren(
            withoutComments(
                node.children.filter((child) => child.kind !== "Metadata").map((child) => fromMarkdownCore(child))
            )
        )
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
        parseCanonicalDump(
            execFileSync(ours, [], {
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
// O2: the content-model projection is intentional. The oracle keeps the
// authored Markdown as one Text child; product fixtures own inline parsing.
const formattedMarkInput = "==a *b* c==\n";
const formattedMarkOracle = processor.runSync(processor.parse(formattedMarkInput), formattedMarkInput).children[0]
    ?.children[0];
const formattedMarkProduct = parseCanonicalDump(execFileSync(ours, [], { input: formattedMarkInput, encoding: "utf8" }))
    .children[0]?.children[0];
if (
    formattedMarkOracle?.type !== "highlight" ||
    formattedMarkOracle.children?.length !== 1 ||
    formattedMarkOracle.children[0]?.type !== "text" ||
    formattedMarkOracle.children[0]?.value !== "a *b* c" ||
    formattedMarkProduct?.kind !== "Mark" ||
    formattedMarkProduct.children.map((child) => child.kind).join(",") !== "Text,Emphasis,Text" ||
    formattedMarkProduct.children[1]?.children[0]?.fields.literal !== "b"
) {
    throw new Error("obsidian parity: highlight-content-model canary failed");
}

const commentCanary = processor.runSync(processor.parse("%%hidden%%\n"), "%%hidden%%\n");
if (commentCanary.children.length !== 0) {
    process.stderr.write("obsidian parity: oracle canary did not remove an Obsidian comment\n");
    process.exit(1);
}
// O3: the `comment-removal` projection compares recognition, not absence. The
// product must parse the same input as one Comment inside one Paragraph, and
// the projection must then reduce that tree to the oracle's empty root.
const commentProduct = parseCanonicalDump(execFileSync(ours, [], { input: "%%hidden%%\n", encoding: "utf8" }));
if (
    commentProduct.children.length !== 1 ||
    commentProduct.children[0]?.kind !== "Paragraph" ||
    commentProduct.children[0].children.length !== 1 ||
    commentProduct.children[0].children[0]?.kind !== "Comment" ||
    commentProduct.children[0].children[0].fields.literal !== "hidden" ||
    fromMarkdownCore(commentProduct).children.length !== 0
) {
    throw new Error("obsidian parity: comment-removal canary failed");
}
const taskCanary = processor.runSync(processor.parse("- [?] task\n"), "- [?] task\n");
if (taskCanary.children[0]?.children?.[0]?.data?.taskChar !== "?") {
    process.stderr.write("obsidian parity: oracle canary did not preserve a custom task character\n");
    process.exit(1);
}

const propertiesCanary = parseProperties(
    '---\nname: Note\ntitle: Title\nsubtitle: Subtitle\nstate: true\nauthors: [Ada, "[[Lin]]"]\ndate: 2026-09-08\n---\n# Body\n'
);
if (
    propertiesCanary.content !== "# Body\n" ||
    JSON.stringify(
        Object.keys(propertiesCanary.metadata)
            .filter((name) => propertiesCanary.metadata[name] !== null)
            .sort()
    ) !== '["authors","date","name","state","subtitle","title"]'
) {
    throw new Error("fixed metadata field oracle canary failed");
}
for (const nonHeader of ["text\n---\nname: value\n---\n", "---yaml\nname: value\n---\n", "--- \nname: value\n---\n"]) {
    if (parseProperties(nonHeader).metadata !== null) throw new Error("oracle accepted a non-header candidate");
}
for (const emptyProperties of ["\uFEFF---\n---\n", "---\n   \n---\n", "---\n# note\n---\n", "---\r\n#\r\n---\r\n"]) {
    if (Object.values(parseProperties(emptyProperties).metadata).some((value) => value !== null))
        throw new Error("oracle rejected empty metadata");
}
const sourceFaithfulProperties = parseProperties(
    '---\nname: 9007199254740993\ntime: 1.0\nstate: 1e2\ndate: -0\n"tit\\u006ce": "text"\n---\n'
);
if (
    JSON.stringify(
        ["name", "time", "state", "date"].map((name) => sourceFaithfulProperties.metadata[name].value.value)
    ) !== '["9007199254740993","1.0","1e2","-0"]' ||
    sourceFaithfulProperties.metadata.title.value.value !== "text"
) {
    throw new Error("oracle lost exact numeric spelling or decoded names");
}
const rangedSource = "---\nname: one\nauthors:\n  - two\n  - 3.0\n---\nBody\n";
const rangedProperties = parseProperties(rangedSource);
const rangedClosing = rangedSource.lastIndexOf("---\n");
if (
    rangedProperties.evidence?.metadataRange[0] !== 0 ||
    rangedProperties.evidence.metadataRange[1] !== rangedClosing + 3
) {
    throw new Error("oracle lost metadata envelope range evidence");
}
for (const invalid of [
    "null",
    "name: one\nname: two",
    "unknown: value",
    "- one\n- two",
    "name:\n  nested: value",
    "name: |\n  text",
    "abstract: >-\n  text",
    "comment: |-\n  text",
    "comment: |2\n  text",
    "authors: [true, null]",
    "[a, b]: value",
    "name: &anchor value",
    "name: *missing",
    "name: !!str 1",
    "name: !application value",
    "name: &loop [*loop]",
    "name: one\n  two",
    "name: 'one\n  two'",
    '{"name": "value"}',
    "{draft, name: Note}",
    '!!map {"name": "value"}',
    'name: "bad\\x41"',
    "...",
    "name: one\n..."
]) {
    let outsideDomain = false;
    try {
        parseProperties(`---\n${invalid}\n---\n`);
    } catch {
        outsideDomain = true;
    }
    if (!outsideDomain) throw new Error(`oracle comparison admitted out-of-domain input ${JSON.stringify(invalid)}`);
}
// Literal prose is compared as atomic text, including blank lines and hashes.
for (const payload of [
    "abstract: |\n  one\n\n  two\n\ncomment: |\n  # prose\nstate: ready\n",
    "abstract: |\n\n  first\n    deeper\n\ncomment: |\n\n",
    "  abstract: |\n    one\n      two\n  state: ready\n"
]) {
    const input = `---\n${payload}---\n`;
    const oracle = parseProperties(input).metadata;
    const product = parseMetadataDump(parseCanonicalDump(execFileSync(ours, [], { input, encoding: "utf8" })));
    if (JSON.stringify(product) !== JSON.stringify(oracle)) throw new Error("literal prose oracle mismatch");
}

for (const [input, label, dest, embedded] of [
    ["[[Note]]\n", "null", crossDestination("Note", null), "false"],
    ["[[Note|]]\n", "", crossDestination("Note", null), "false"],
    [
        "[[Folder/Note#Heading#Child|Display text]]\n",
        "Display text",
        crossDestination("Folder/Note", "Heading#Child"),
        "false"
    ],
    ["![[Note#^block-id]]\n", "null", crossDestination("Note", "block-id"), "true"]
]) {
    const parsed = processor.runSync(processor.parse(input), input);
    const oracle = fromMdast(parsed, new Set(), input).children[0]?.children[0];
    const oursTree = fromMarkdownCore(parseCanonicalDump(execFileSync(ours, [], { input, encoding: "utf8" })), false);
    const actual = oursTree.children[0]?.children[0];
    for (const value of [oracle, actual]) {
        if (
            value?.kind !== "CrossLink" ||
            value.fields.label !== label ||
            value.fields.embedded !== embedded ||
            JSON.stringify(value.fields.dest) !== JSON.stringify(dest) ||
            value.children.length !== 0
        ) {
            throw new Error(`obsidian parity: cross-link projection canary failed for ${JSON.stringify(input)}`);
        }
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
// Plain punctuation is inert inside text. Literal contents never open nested fields.
for (const punctuation of ["[", "]", "{", "}", ",", "#", "'", '"']) {
    const input = `---\nname: text ${punctuation} literal\nabstract: |\n  body [ { ' literal\nstate: 3\n---\n`;
    const oracle = parseProperties(input).metadata;
    const product = parseMetadataDump(parseCanonicalDump(execFileSync(ours, [], { input, encoding: "utf8" })));
    if (JSON.stringify(product) !== JSON.stringify(oracle))
        throw new Error("plain punctuation changed member ownership");
}
// Recovery belongs to the product grammar; malformed YAML is outside the document oracle.
const recoveredProperties = parseCanonicalDump(
    execFileSync(ours, [], {
        input: "---\nname: 1\nnot YAML\n...\nunknown: 3\nauthors: [true]\nstate: 2\n---\nbody\n",
        encoding: "utf8"
    })
);
const recoveredMetadata = recoveredProperties.children.find((item) => item.kind === "Metadata");
const recoveredFields = parseMetadataDump(recoveredProperties);
if (
    recoveredMetadata?.children.length !== 0 ||
    JSON.stringify(
        Object.keys(recoveredFields)
            .filter((name) => recoveredFields[name] !== null)
            .sort()
    ) !== '["name","state"]'
) {
    throw new Error("ignored metadata members leaked into the AST or consumed valid neighbors");
}
const metadataDumpCanary = Object.fromEntries(
    [...metadataFields].map((name) => [
        name,
        name === "name" ? { kind: "scalar", value: { kind: "text", value: "a b" } } : null
    ])
);
const capturedMetadata = parseCanonicalDump(
    'Document scope=1:1..3:3 anchor=null attributes={} children=0\n└── Metadata scope=1:1..3:3 name=scalar(text("a b")) title=null subtitle=null time=null date=null authors=null keywords=null abstract=null state=null comment=null children=0\n'
);
if (JSON.stringify(parseMetadataDump(capturedMetadata)) !== JSON.stringify(metadataDumpCanary)) {
    throw new Error("obsidian parity: metadata parser rejected direct fields");
}

const registeredEntries = [...policy.baselineGaps, ...(policy.expectedDivergences ?? [])];
const invalidPolicy = [];
const entryIds = new Set();
const entryInputs = new Set();
for (const entry of registeredEntries) {
    if (!entry.id || entryIds.has(entry.id)) invalidPolicy.push(`duplicate or empty entry id: ${String(entry.id)}`);
    if (entryInputs.has(entry.input)) invalidPolicy.push(`duplicate entry input: ${JSON.stringify(entry.input)}`);
    if (!/^[0-9a-f]{64}$/.test(entry.oracleDigest)) invalidPolicy.push(`invalid oracle digest: ${entry.id}`);
    if (!/^[0-9a-f]{64}$/.test(entry.markdownCoreDigest)) {
        invalidPolicy.push(`invalid markdown-core digest: ${entry.id}`);
    }
    entryIds.add(entry.id);
    entryInputs.add(entry.input);
}
if (invalidPolicy.length) {
    process.stderr.write(`obsidian parity: invalid ${policyPath}\n  ${invalidPolicy.join("\n  ")}\n`);
    process.exit(1);
}

const entries = new Map(registeredEntries.map((entry) => [entry.input, entry]));
const seenEntries = new Set();
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
    const entry = entries.get(testCase.input);
    if (result.oracle === result.ours) {
        if (entry) failures.push({ ...testCase, settledEntry: entry, ...result });
    } else if (entry) {
        const oracleDigest = digest(result.oracle);
        const markdownCoreDigest = digest(result.ours);
        if (verbose) {
            process.stdout.write(`  ${entry.id}: oracle=${oracleDigest} markdown-core=${markdownCoreDigest}\n`);
        }
        if (entry.oracleDigest !== oracleDigest || entry.markdownCoreDigest !== markdownCoreDigest) {
            failures.push({ ...testCase, changedEntry: entry, oracleDigest, markdownCoreDigest, ...result });
        } else {
            seenEntries.add(testCase.input);
        }
    } else {
        failures.push({ ...testCase, ...result });
    }
}

const corpusInputs = new Set(cases.map((testCase) => testCase.input));
for (const [input, entry] of entries) {
    if (!corpusInputs.has(input)) failures.push({ source: policyPath, input, unreachableEntry: entry });
}

if (unknownKinds.size) {
    process.stderr.write(
        `obsidian parity FAILED: unmapped oracle node kind(s): ${[...unknownKinds].sort().join(", ")}\n`
    );
    process.exit(1);
}

process.stdout.write(
    `obsidian parity: ${String(cases.length)} inputs, ` +
        `${String(seenEntries.size)}/${String(entries.size)} registered entries reproduced\n`
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
        } else if (entry.settledEntry) {
            process.stderr.write(
                `    registered entry ${entry.settledEntry.id} now agrees; remove it from ${policyPath}\n`
            );
        } else if (entry.unreachableEntry) {
            process.stderr.write(
                `    registered entry ${entry.unreachableEntry.id} is no longer exercised; restore or retire it explicitly\n`
            );
        } else if (entry.changedEntry) {
            process.stderr.write(
                `    registered entry ${entry.changedEntry.id} changed shape\n` +
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
