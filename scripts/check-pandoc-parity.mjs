#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { parseCanonicalDump } from "./lib/upstream-cmark.mjs";
import {
    root,
    withOracle,
    fromPandoc,
    fromCanonical,
    assertCanaries,
    validatePolicy,
    verifyComparison
} from "./lib/pandoc-oracle.mjs";

const read = (name) => JSON.parse(fs.readFileSync(path.join(root, `specs/oracles/pandoc/${name}.json`), "utf8"));
const cases = read("corpus").cases;
const policy = read("deltas");
const entries = validatePolicy(policy, cases);
const cli = path.join(root, "build/cmake/packages/markdown-core/core/markdown-core");
withOracle((run) => {
    assertCanaries(run);
    let differences = 0;
    const failures = [];
    for (const testCase of cases) {
        try {
            const expected = fromPandoc(run(testCase.input, testCase.from));
            const actual = fromCanonical(
                parseCanonicalDump(execFileSync(cli, [], { input: testCase.input, encoding: "utf8" }))
            );
            if (verifyComparison(testCase, expected, actual, entries.get(testCase.id))) differences++;
        } catch (error) {
            failures.push(`${testCase.id}: ${error.message}`);
        }
    }
    if (failures.length) throw new Error(failures.join("\n"));
    process.stdout.write(
        `Pandoc parity: ${cases.length} cases, ${differences} exact registered differences (${policy.entries.filter((entry) => entry.status === "gap").length} remaining feature gaps)\n`
    );
});
