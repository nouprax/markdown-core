#!/usr/bin/env node
// The call cycles of the Rust core, against scripts/tools/rust-recursion.json.
//
// A cycle in the call graph is recursion, written or not: the compiler's drop
// glue for a recursive type and derived traits are calls in the IR like any
// other. A cycle whose depth grows with the input overflows the call stack,
// which Rust cannot catch, so every cycle must be one the list names:
// `bounded` ones with the reason their depth is, and `grown` ones, whose
// depth the input sets, which must run each level in
// `markdown_core::util::stack::room` (room on the stack, from `stacker`):
// one of the cycle's functions is that function.
//
// An entry names its cycle by `functions`, each a path a member equals or,
// ending in `::`, begins with.
//
// The lib is compiled to LLVM IR at opt-level 0 (no call is inlined away),
// and its v0 symbol names read back with binutils' c++filt. A cycle
// matches an entry when it holds one of the entry's functions, so a
// function that joins a known cycle passes; a cycle no entry matches fails,
// and so does an entry no cycle matches, which is stale.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const crate = path.join(root, "packages/markdown-core-rs");
const target = process.env.RECURSION_TARGET_DIR ?? path.join(root, "build/rust-recursion");
const policy = JSON.parse(fs.readFileSync(path.join(root, "scripts/tools/rust-recursion.json"), "utf8"));

fs.rmSync(path.join(target, "debug/deps"), { recursive: true, force: true });
execFileSync(
    "cargo",
    ["rustc", "--lib", "--locked", "--quiet", "--", "--emit=llvm-ir", "-C", "codegen-units=1", "-C", "opt-level=0"],
    { cwd: crate, stdio: "inherit", env: { ...process.env, CARGO_TARGET_DIR: target } }
);
const deps = path.join(target, "debug/deps");
const files = fs.readdirSync(deps).filter((name) => name.startsWith("markdown_core") && name.endsWith(".ll"));
if (files.length !== 1) {
    throw new Error(`expected one markdown_core .ll in ${deps}, found ${files.length}`);
}

/** The paths of v0 Rust symbols, without crate disambiguators and hashes:
 * `markdown_core::tokenizer::Tokenizer::push`. */
function demangle(symbols) {
    const out = execFileSync("c++filt", { input: symbols.join("\n"), encoding: "utf8", maxBuffer: 1 << 28 });
    return out
        .split("\n")
        .slice(0, symbols.length)
        .map((name) => name.replace(/\[[0-9a-f]+\]/g, "").replace(/::h[0-9a-f]{16}$/, ""));
}

const graph = new Map();
let current = null;
for (const line of fs.readFileSync(path.join(deps, files[0]), "utf8").split("\n")) {
    if (line.startsWith("define ")) {
        const name = /@("[^"]+"|[\w$.]+)\(/.exec(line);
        current = name ? name[1].replace(/"/g, "") : null;
        if (current && !graph.has(current)) {
            graph.set(current, new Set());
        }
    } else if (line.startsWith("}")) {
        current = null;
    } else if (current && /\b(call|invoke)\b/.test(line)) {
        for (const callee of line.matchAll(/@("[^"]+"|[\w$.]+)\(/g)) {
            graph.get(current).add(callee[1].replace(/"/g, ""));
        }
    }
}

// Tarjan's strongly connected components, with an explicit stack.
const index = new Map();
const low = new Map();
const stack = [];
const held = new Set();
const cycles = [];
let counter = 0;
for (const start of graph.keys()) {
    if (index.has(start)) {
        continue;
    }
    const work = [[start, [...graph.get(start)].filter((callee) => graph.has(callee)), 0]];
    index.set(start, counter);
    low.set(start, counter++);
    stack.push(start);
    held.add(start);
    while (work.length) {
        const frame = work[work.length - 1];
        const [node, callees] = frame;
        if (frame[2] < callees.length) {
            const callee = callees[frame[2]++];
            if (!index.has(callee)) {
                index.set(callee, counter);
                low.set(callee, counter++);
                stack.push(callee);
                held.add(callee);
                work.push([callee, [...graph.get(callee)].filter((next) => graph.has(next)), 0]);
            } else if (held.has(callee)) {
                low.set(node, Math.min(low.get(node), index.get(callee)));
            }
            continue;
        }
        work.pop();
        if (work.length) {
            const parent = work[work.length - 1][0];
            low.set(parent, Math.min(low.get(parent), low.get(node)));
        }
        if (low.get(node) === index.get(node)) {
            const members = [];
            let member;
            do {
                member = stack.pop();
                held.delete(member);
                members.push(member);
            } while (member !== node);
            if (members.length > 1 || graph.get(node).has(node)) {
                cycles.push(members);
            }
        }
    }
}

{
    const names = demangle(cycles.flat());
    let at = 0;
    for (let i = 0; i < cycles.length; i++) {
        const size = cycles[i].length;
        cycles[i] = [...new Set(names.slice(at, at + size))].sort();
        at += size;
    }
}
const ROOM = "markdown_core::util::stack::room";
const used = new Set();
const unknown = [];
const roomless = [];
for (const members of cycles) {
    const entry = policy.cycles.find((candidate) =>
        candidate.functions.some((name) =>
            members.some((member) => (name.endsWith("::") ? member.startsWith(name) : member === name))
        )
    );
    if (entry) {
        used.add(entry.id);
        if (entry.status === "grown" && !members.some((member) => member.startsWith(ROOM))) {
            roomless.push(entry.id);
        }
    } else {
        unknown.push(members);
    }
}
const stale = policy.cycles.filter((entry) => !used.has(entry.id));
const grown = policy.cycles.filter((entry) => entry.status === "grown" && used.has(entry.id));

process.stdout.write(`rust recursion: ${cycles.length} call cycles, ${grown.length} grown in room on the stack\n`);
if (unknown.length || stale.length || roomless.length) {
    for (const members of unknown) {
        process.stdout.write(`\nnew call cycle (${members.length} functions):\n`);
        for (const name of members) {
            process.stdout.write(`  ${name}\n`);
        }
    }
    for (const entry of stale) {
        process.stdout.write(`\nstale entry ${entry.id}: no cycle holds any of its functions\n`);
    }
    for (const id of roomless) {
        process.stdout.write(`\ngrown cycle ${id} runs no level in ${ROOM}\n`);
    }
    process.stdout.write(
        "\nRecursion whose depth grows with the input overflows the call stack. Run each level in\n" +
            `${ROOM} and add the cycle to scripts/tools/rust-recursion.json as grown, or add it as\n` +
            "bounded, saying why its depth is.\n"
    );
    process.exit(1);
}
