#!/usr/bin/env node
/** The construction assertion (core/node.h) is the only structural self-check
 * this tree has: the engine attaches a child only where its parent may hold
 * it, and a Debug build asserts so at every attachment. An assertion that is
 * compiled out is indistinguishable from one that holds, and the check this
 * one replaced never executed for its whole life for exactly that reason: its
 * define reached one target out of five, and no preset configured `Debug`.
 *
 * This audit holds the assertion against the artifacts rather than the text:
 * the Debug configuration is read out of the preset file and the CI graph, and
 * the absence of `NDEBUG` out of that configuration's real compiler flags.
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const DEFINE = "NDEBUG";

const failures = [];

/* 1. A Debug configure preset exists, a build preset builds it and a test
 *    preset runs it. Without this the assertion is compiled in and still
 *    never runs. */
const presets = JSON.parse(fs.readFileSync(path.join(root, "CMakePresets.json"), "utf8"));
const debugConfigure = presets.configurePresets.filter((preset) => preset.cacheVariables?.CMAKE_BUILD_TYPE === "Debug");
if (debugConfigure.length !== 1) {
    failures.push(`CMakePresets.json must hold exactly one Debug configure preset; found ${debugConfigure.length}`);
}
const debugPresetName = debugConfigure[0]?.name;
if (debugPresetName && !presets.buildPresets.some((preset) => preset.configurePreset === debugPresetName)) {
    failures.push(`no build preset drives the "${debugPresetName}" configure preset`);
}
const debugTestPresets = presets.testPresets.filter((preset) => preset.configurePreset === debugPresetName);
if (debugPresetName && !debugTestPresets.length) {
    failures.push(`no test preset runs the "${debugPresetName}" configure preset; the assertion would never execute`);
}

/* 2. CI builds that tree and runs that suite. A preset nothing invokes is the
 *    same dead code in a different file. */
const ci = fs.readFileSync(path.join(root, ".github/workflows/ci.yml"), "utf8");
for (const job of ["c-debug-test-build", "c-debug-test"]) {
    if (!new RegExp(`^ {4}${job}:$`, "m").test(ci)) {
        failures.push(`CI has no ${job} job; the Debug configuration is built or run by nothing`);
    }
}
if (!ci.includes("C_DEBUG_TEST:")) {
    failures.push("tests-ready does not require the Debug suite, so its failure would not block a merge");
}

/* 3. No C translation unit of the package compiles with `NDEBUG` in a real
 *    Debug configuration, so every assertion is compiled in. This is the part
 *    that cannot be faked by reading source: it reads the generated compiler
 *    flags. */
const tree = path.join(root, "build/debug");
if (!fs.existsSync(path.join(tree, "CMakeCache.txt"))) {
    execFileSync("cmake", ["--preset", debugPresetName ?? "debug"], { cwd: root, stdio: "pipe" });
}
const flagFiles = [];
(function collectFlags(dir) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) {
            collectFlags(full);
        } else if (entry.name === "flags.make") {
            flagFiles.push(full);
        }
    }
})(path.join(tree, "packages/markdown-core"));

let checked = 0;
for (const file of flagFiles) {
    const text = fs.readFileSync(file, "utf8");
    /* C only. The assertion is in C engine code; the C++ facade test compiles
     * no engine source. */
    if (!/^C_DEFINES|^C_FLAGS/m.test(text)) {
        continue;
    }
    checked++;
    if (new RegExp(`-D${DEFINE}\\b`).test(text)) {
        failures.push(`${path.relative(tree, file)} compiles C with ${DEFINE}`);
    }
}
if (!checked) {
    failures.push("the Debug tree generated no C compiler flag files; this audit is reaching nothing");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-construction-assertions: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-construction-assertions: none of the ${checked} C flag sets of the Debug tree defines ${DEFINE}; ` +
        `a preset and a CI job run it`
);
