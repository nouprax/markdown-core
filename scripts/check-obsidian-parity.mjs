#!/usr/bin/env node
/**
 * Obsidian-flavored Markdown supplementary parity gate.
 *
 * The official Obsidian help snapshot owns the language. This gate executes
 * the most-used current npm OFM parser over the intersection it implements,
 * compares a scope-free semantic tree, and keeps every current product gap
 * explicit and fail-closed.
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

import { readExamples } from "./lib/fixture-corpus.mjs";
import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const policyPath = "specs/oracles/obsidian/deltas.json";
const policy = JSON.parse(fs.readFileSync(path.join(root, policyPath), "utf8"));
const verbose = process.argv.includes("--verbose");
const ours = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");

if (!fs.existsSync(ours)) {
    process.stderr.write(`obsidian parity: missing ${path.relative(root, ours)}\nBuild it with: pnpm build:c\n`);
    process.exit(1);
}

const require = createRequire(import.meta.url);
const installedPackage = JSON.parse(
    fs.readFileSync(require.resolve("@quartz-community/remark-obsidian/package.json"), "utf8")
);
if (installedPackage.version !== policy.oracle.version) {
    process.stderr.write(
        `obsidian parity: policy pins ${policy.oracle.package}@${policy.oracle.version}, ` +
            `but ${installedPackage.version} is installed\nRun: pnpm install --frozen-lockfile\n`
    );
    process.exit(1);
}

const processor = unified().use(remarkParse).use(remarkGfm).use(remarkObsidian, policy.oracle.options);

const mdastKinds = {
    root: "Document",
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
    wikilink: "CrossLink",
    highlight: "Mark"
};

const comparedFields = {
    Text: ["literal"],
    List: ["flavor", "start", "tight"],
    ListItem: ["marker"],
    Code: ["literal"],
    CodeBlock: ["literal", "info"],
    Link: ["destination", "title"],
    Image: ["source", "title"],
    CrossLink: ["embedded", "path", "route", "dest", "label"]
};

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
        fields.destination = node.url;
        fields.title = node.title ?? "null";
    }
    if (node.type === "image") {
        fields.source = node.url;
        fields.title = node.title ?? "null";
    }
    if (node.type === "wikilink") {
        const spelling = source.slice(node.position?.start.offset ?? 0, node.position?.end.offset ?? 0);
        const bodyStart = spelling.startsWith("![[") ? 3 : 2;
        const hasLabelDelimiter = spelling.slice(bodyStart, -2).includes("|");
        fields.embedded = String(node.embedded);
        fields.path = node.path;
        fields.route = node.heading ? (node.heading.startsWith("^") ? "block" : "fragment") : "none";
        fields.dest = node.heading ? node.heading.replace(/^\^/, "") : "null";
        fields.label = node.alias === "" && !hasLabelDelimiter ? "null" : node.alias;
    }

    return {
        kind,
        fields,
        children: normalizeChildren((node.children ?? []).map((child) => fromMdast(child, unknown, source)))
    };
}

function fromMarkdownCore(node) {
    const fields = {};
    for (const name of comparedFields[node.kind] ?? []) {
        fields[name] = String(node.fields[name] ?? (name === "literal" ? "" : "null"));
    }
    return {
        kind: node.kind,
        fields,
        children: normalizeChildren(node.children.map(fromMarkdownCore))
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
    const parsed = processor.parse(input);
    const transformed = processor.runSync(parsed, input);
    const oracleTree = fromMdast(transformed, unknown, input);
    const ourTree = fromMarkdownCore(
        parseCanonicalDump(
            execFileSync(ours, ["--profile", "gfm-extended"], {
                input,
                encoding: "utf8",
                maxBuffer: 1 << 24
            })
        )
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
