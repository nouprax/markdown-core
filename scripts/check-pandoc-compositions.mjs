#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { compositionEvidenceCases, compositionFuzzCases, compareCompositions } from "./lib/pandoc-compositions.mjs";
import { root, source, withOracle, fromPandoc, fromCanonical, assertCanaries } from "./lib/pandoc-oracle.mjs";
import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";

const { values } = parseArgs({
    options: {
        output: { type: "string", default: "build/pandoc-compositions.json" },
        "require-agreement": { type: "boolean", default: false }
    },
    allowPositionals: false
});
const read = (name) => JSON.parse(fs.readFileSync(path.join(root, `specs/oracles/pandoc/${name}.json`), "utf8"));
const cases = [
    ...compositionEvidenceCases(source, read("corpus"), read("composition-seeds")),
    ...compositionFuzzCases(source, read("corpus"), read("composition-seeds"))
];
const cli = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
withOracle((run) => {
    assertCanaries(run);
    const report = compareCompositions(
        cases,
        (input, from) => fromPandoc(run(input, from)),
        (input) => fromCanonical(parseCanonicalDump(execFileSync(cli, [], { input, encoding: "utf8" })))
    );
    const output = path.resolve(root, values.output);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, `${JSON.stringify(report, null, 2)}\n`);
    process.stdout.write(
        `Pandoc composition probe: ${report.caseCount} composition cases, ${report.agreements} agreements, ` +
            `${report.differences} observed differences\nReport: ${output}\n` +
            "This probe does not certify P12 closure.\n"
    );
    if (values["require-agreement"] && report.differences !== 0) process.exitCode = 1;
});
