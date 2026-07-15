#!/usr/bin/env node

import assert from "node:assert/strict";

let input = "";
for await (const chunk of process.stdin) input += chunk;
const response = JSON.parse(input);
const checks = response.check_runs ?? [];
for (const name of ["Required gates", "CodeQL gate"]) {
    const matching = checks.filter((check) => check.name === name);
    assert.ok(matching.length > 0, `${name} is missing for the release commit`);
    assert.ok(
        matching.some((check) => check.status === "completed" && check.conclusion === "success"),
        `${name} is not successful`
    );
}
console.log("Release commit has successful Required gates and CodeQL gate evidence.");
