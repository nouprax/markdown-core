#!/usr/bin/env node
/** The allocator is not a parse parameter, and the seam that lets the tests
 * replace it is the linker. Both halves are easy to undo by accident, and
 * neither failure shows up as a failing test:
 *
 *   - Reintroducing a `markdown_core_mem` parameter or field puts the
 *     allocator back on the parse and, if it reaches a node, back on the node.
 *     Nothing breaks; the plumbing just grows back.
 *   - Adding ANY other symbol to `core/alloc.c` breaks substitution. An
 *     archive member is pulled only to resolve an undefined symbol, so the
 *     test object's definitions win today precisely because nothing else in
 *     the library needs anything from that file. Give it a second reason to be
 *     pulled and the substitution becomes a duplicate-definition error -- or,
 *     worse on a linker that tolerates it, silently stops taking effect and
 *     the OOM sweep passes without injecting anything.
 *
 * See `core/alloc.h` for why the seam cannot be a swappable global instead.
 */

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const pkg = path.join(root, "packages/markdown-core");

const failures = [];

/** Every `*.c` and `*.h` under `core/` and `elements/` -- the shipped library. */
function librarySources() {
    return ["core", "elements"].flatMap((dir) =>
        fs
            .readdirSync(path.join(pkg, dir))
            .filter((name) => name.endsWith(".c") || name.endsWith(".h"))
            .map((name) => `${dir}/${name}`)
    );
}

/* (1) The allocator type is gone and must not come back into the library. */
for (const file of librarySources()) {
    if (file === "core/alloc.h") continue;
    const source = fs.readFileSync(path.join(pkg, file), "utf8");
    const stripped = source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
    if (/\bmarkdown_core_mem\b/.test(stripped)) {
        failures.push(`${file}: names markdown_core_mem; the allocator is not a parse or node parameter`);
    }
}

/* (2) `alloc.c` defines the three substitution points and nothing else. */
const allocPath = path.join(pkg, "core/alloc.c");
const alloc = fs.readFileSync(allocPath, "utf8");
const body = alloc.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const EXPECTED = ["markdown_core_alloc", "markdown_core_realloc", "markdown_core_free"];
const defined = [...body.matchAll(/^[A-Za-z_][\w *]*?\b(\w+)\s*\([^;]*\)\s*\{/gm)].map((m) => m[1]);
if (defined.length !== EXPECTED.length || EXPECTED.some((name) => !defined.includes(name))) {
    failures.push(`core/alloc.c defines [${defined.join(", ")}]; it must define exactly [${EXPECTED.join(", ")}]`);
}
/* A file-scope variable is a symbol too, and a writable one would also fail
 * the installed-archive check in audit-package-contents.sh. */
const fileScopeStatements = body
    .split("\n")
    .map((line) => line.trim())
    .filter((line) => /^[A-Za-z_][\w *]*\b\w+\s*(=|;|\[)/.test(line) && !line.startsWith("#"));
if (fileScopeStatements.length) {
    failures.push(`core/alloc.c declares ${fileScopeStatements.length} symbol(s) besides its three functions`);
}

/* (3) The tests must actually define the three, or they substitute nothing. */
const SUBSTITUTERS = ["tests/api/main.c", "tests/runners/oom_runner.c"];
for (const file of SUBSTITUTERS) {
    const source = fs.readFileSync(path.join(pkg, file), "utf8");
    const missing = EXPECTED.filter((name) => !new RegExp(`^\\w[\\w *]*\\b${name}\\s*\\(`, "m").test(source));
    if (missing.length) {
        failures.push(`${file}: does not define [${missing.join(", ")}], so it substitutes nothing`);
    }
}

/* (4) A probe counter read outside an armed region is a test that passes as
 * `0 <= 0`. `grid_opening_memory` did exactly that after the probes moved to
 * the link seam: it reset the byte counters by hand, never enabled them, and
 * its memory assertions compared zero against zero for every shape and size.
 * A counter that is only READ where it is armed cannot go quiet like that. */
const ARMS = {
    properties_peak_bytes: "properties_probe_arm",
    properties_live_bytes: "properties_probe_arm",
    payload_live: "payload_probe_arm",
    payload_allocations: "payload_probe_arm",
    text_allocation_calls: "text_counting = 1"
};
/* The interposer, the arming helpers and the accounting helper are where these
 * live; `marker_free_count` needs no arming because its branch is ungated. */
const PROBE_INFRASTRUCTURE = new Set([
    "markdown_core_alloc",
    "markdown_core_realloc",
    "markdown_core_free",
    "payload_probe_arm",
    "payload_probe_disarm",
    "properties_probe_arm",
    "properties_probe_disarm",
    "properties_account",
    "allocation_refused"
]);
{
    const probeFile = "tests/api/main.c";
    const probeSource = fs.readFileSync(path.join(pkg, probeFile), "utf8");
    const lines = probeSource.split("\n");
    const definitions = [];
    lines.forEach((line, index) => {
        const match = /^[A-Za-z_][\w *]*?\b(\w+)\([^;]*\)\s*\{\s*$/.exec(line);
        if (match) definitions.push({ line: index, name: match[1] });
    });
    const enclosing = (index) => {
        let found = null;
        for (const definition of definitions) {
            if (definition.line <= index) found = definition.name;
            else break;
        }
        return found;
    };
    const bodyOf = (name) => {
        const start = definitions.find((definition) => definition.name === name).line;
        const collected = [];
        let opened = false;
        for (let i = start; i < lines.length; i += 1) {
            collected.push(lines[i]);
            if (lines[i].includes("{")) opened = true;
            if (opened && lines[i] === "}") break;
        }
        return collected.join("\n");
    };
    const readers = new Map();
    lines.forEach((line, index) => {
        if (/^\s*(\*|\/\*|static\s)/.test(line)) return;
        for (const counter of Object.keys(ARMS)) {
            if (!line.includes(counter)) continue;
            const owner = enclosing(index);
            if (!owner || PROBE_INFRASTRUCTURE.has(owner)) continue;
            if (!readers.has(owner)) readers.set(owner, new Set());
            readers.get(owner).add(counter);
        }
    });
    for (const [owner, counters] of readers) {
        const body = bodyOf(owner);
        for (const counter of counters) {
            if (!body.includes(ARMS[counter])) {
                failures.push(`${probeFile}: ${owner} reads ${counter} without arming it (${ARMS[counter]})`);
            }
        }
    }
    if (!readers.size) {
        failures.push(`${probeFile}: found no probe-counter readers; the audit is not reaching the tests`);
    }
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-allocator-seam: ${failure}`);
    }
    process.exit(1);
}
console.log("audit-allocator-seam: one allocator, three substitution points, no markdown_core_mem in the library");
