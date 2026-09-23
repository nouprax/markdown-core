/**
 * Reads the repository's spec-fixture format — a 32-backtick `example` fence,
 * the input, a `.` line, the expected canonical dump, and the closing fence.
 *
 * The parity gates read only the input half. Their whole point is that two
 * independent parsers agree, so consulting the stored expected dump would put
 * this repository's own output back in the loop.
 */

import fs from "node:fs";
import path from "node:path";

const FENCE = "`".repeat(32);

export function readExamples(root, relativePath, { includeExampleNumber = false } = {}) {
    const absolute = path.resolve(root, relativePath);
    const lines = fs.readFileSync(absolute, "utf8").split("\n");
    const cases = [];
    const headings = [];
    let example = 0;
    for (let i = 0; i < lines.length; i++) {
        const heading = /^(#{1,6})\s+(.+?)\s*$/.exec(lines[i]);
        if (heading) {
            const depth = heading[1].length;
            headings.length = depth;
            headings[depth - 1] = heading[2];
        }
        if (!lines[i].startsWith(`${FENCE} example`)) continue;
        example += 1;
        const attributes = lines[i].slice(`${FENCE} example`.length).trim();
        const body = [];
        let cursor = i + 1;
        for (; cursor < lines.length && lines[cursor] !== "."; cursor++) body.push(lines[cursor]);
        let end = cursor + 1;
        for (; end < lines.length && !lines[end].startsWith(FENCE); end++);
        if (attributes !== "disabled") {
            // The fixtures write tabs as U+2192 so they survive editing.
            cases.push({
                ...(includeExampleNumber ? { example } : {}),
                source: `${relativePath}:${String(i + 1)}`,
                attributes,
                tags: attributes === "" ? [] : attributes.split(/\s+/),
                gfmExtension: headings.some((title) => /\(extension\)$/.test(title)),
                input: `${body.join("\n").replace(/→/g, "\t")}\n`
            });
        }
        i = end;
    }
    return cases;
}

export function selectExamples(examples, selection, policyPath = "corpus policy") {
    if (selection === undefined) return examples;
    if (selection === "commonmark-sections") return examples.filter((example) => !example.gfmExtension);
    if (selection === "gfm-extension-sections") return examples.filter((example) => example.gfmExtension);
    if (selection === "tagged") return examples.filter((example) => example.tags.length > 0);
    if (selection === "untagged") return examples.filter((example) => example.tags.length === 0);
    throw new Error(`${policyPath}: unknown corpus selection ${JSON.stringify(selection)}`);
}
