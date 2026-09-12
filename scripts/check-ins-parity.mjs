#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parse } from "yaml";
import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import {
    oracle,
    fromTokens,
    fromCanonical,
    assertCanaries,
    validatePolicy,
    verifyComparison
} from "./lib/ins-oracle.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const read = (file) => fs.readFileSync(path.join(root, file), "utf8");
const policy = JSON.parse(read("specs/oracles/markdown-it-ins/deltas.json"));
const cases = JSON.parse(read("specs/oracles/markdown-it-ins/corpus.json"));
const entries = validatePolicy(policy, cases);
const require = createRequire(import.meta.url);
const manifest = JSON.parse(read("package.json"));
const lock = parse(read("pnpm-lock.yaml"));
for (const pin of policy.oracle) {
    const installed = require(`${pin.package}/package.json`);
    if (
        installed.version !== pin.version ||
        manifest.devDependencies[pin.package] !== pin.version ||
        lock.packages[`${pin.package}@${pin.version}`]?.resolution.integrity !== pin.integrity
    )
        throw new Error(`insertion oracle pin mismatch: ${pin.package}`);
}
if (
    policy.oracle.length !== 2 ||
    policy.oracle[0].package !== "markdown-it" ||
    policy.oracle[1].package !== "markdown-it-ins"
)
    throw new Error("missing insertion oracle pins");
assertCanaries();
const cli = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
if (!fs.existsSync(cli)) throw new Error("Build the product CLI with pnpm build:c");
let reproduced = 0;
const failures = [];
for (const testCase of cases) {
    const expected = JSON.stringify(fromTokens(oracle.parse(testCase.input, {})));
    const actual = JSON.stringify(
        fromCanonical(parseCanonicalDump(execFileSync(cli, [], { input: testCase.input, encoding: "utf8" })))
    );
    try {
        if (verifyComparison(entries.get(testCase.id), expected, actual)) reproduced++;
    } catch (error) {
        failures.push(`${testCase.id}: ${error.message}`);
    }
}
if (failures.length) throw new Error(failures.join("\n"));
process.stdout.write(
    `insertion parity: ${cases.length} inputs, ${reproduced}/${entries.size} exact divergences reproduced; no baseline gaps\n`
);
