#!/usr/bin/env node
/**
 * Context coverage audit.
 *
 * The context query (docs/plans/2026-10-23-lsp-completion-engine.md, 3.1)
 * reports a slot as a slot kind and an owner kind, and the contract says
 * which owners each slot kind has (scripts/tooling/generate-node-kinds.mjs,
 * the slot model). Each owner's slot is read by its own scanner, so a pair
 * no example reaches is a scanner no test has seen report.
 *
 * This audit reads the slots the examples of the context fixture expect and
 * requires every pair of the contract among them. A slot kind that every
 * attribute site or every block identifier owner has (`any`) needs one
 * example; its owners are the sites, and the fixture names those it tests.
 * PENDING lists the pairs no scanner reports yet, which must not appear in
 * the fixture either: a pair leaves the list when its example lands.
 *
 *   node scripts/audit/check-context-coverage.mjs
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { buildModel } from "../tooling/generate-node-kinds.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");

const FIXTURE = "packages/markdown-core/tests/context/context.txt";

/** The pairs whose scanner has not landed yet: none since plan step 2e. */
const PENDING = new Set();

const model = buildModel(
    JSON.parse(read("docs/specs/canonical-ast.json")),
    JSON.parse(read("packages/markdown-core/node-types.json"))
);
const tested = new Set(
    [...read(FIXTURE).matchAll(/^slot ([a-z]+) ([A-Za-z]+) /gm)].map(([, slot, owner]) => `${slot}/${owner}`)
);

const errors = [];
for (const slot of model.slots) {
    if (slot.any && ![...tested].some((pair) => pair.startsWith(`${slot.name}/`))) {
        errors.push(`${slot.name}: no example of any owner`);
    }
    for (const owner of slot.owners) {
        const pair = `${slot.name}/${owner}`;
        if (PENDING.has(pair)) {
            if (tested.has(pair)) errors.push(`${pair}: has an example, so it is no longer pending`);
        } else if (!tested.has(pair)) {
            errors.push(`${pair}: no example`);
        }
    }
}
for (const pair of PENDING) {
    const [name, owner] = pair.split("/");
    if (!model.slots.some((slot) => slot.name === name && slot.owners.includes(owner))) {
        errors.push(`${pair}: pending, but the contract has no such pair`);
    }
}

if (errors.length) {
    console.error(`${FIXTURE}:\n  ${errors.join("\n  ")}`);
    console.error("\nContext coverage audit failed: a slot owner's scanner has no example.");
    process.exit(1);
}
console.log(
    `Context coverage audit passed: ${String(tested.size)} slot and owner pairs have examples, ` +
        `${String(PENDING.size)} pending.`
);
