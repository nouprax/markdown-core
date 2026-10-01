#!/usr/bin/env node
/** The re-parse contract E1-E4 (reparse-contract.mjs) over the library's
 * sources and the attached element descriptors. */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { readElementInventory } from "../shared/element-inventory.mjs";
import { auditReparseContract } from "./reparse-contract.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

const sources = ["core", "elements"].flatMap((dir) =>
    fs
        .readdirSync(path.join(pkg, dir))
        .filter((name) => /\.[ch]$/.test(name))
        .map((name) => ({ file: `${dir}/${name}`, source: fs.readFileSync(path.join(pkg, dir, name), "utf8") }))
);
const { ordered } = readElementInventory(path.join(pkg, "elements"));
const failures = auditReparseContract(ordered, sources);
if (failures.length) {
    for (const failure of failures) console.error(`audit-reparse-contract: ${failure}`);
    process.exit(1);
}
console.log(`audit-reparse-contract: ${ordered.length} descriptors and ${sources.length} sources keep E1-E4`);
