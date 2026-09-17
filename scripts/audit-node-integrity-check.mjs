#!/usr/bin/env node
/** `markdown_core_node_check` is the only structural self-check this tree has,
 * and for its whole life it never executed. Two independent reasons stacked,
 * and neither shows up as a failing test -- a check that is compiled out is
 * indistinguishable from a check that passes:
 *
 *   - `MARKDOWN_CORE_DEBUG_NODES` was set with `set(CMAKE_C_FLAGS_DEBUG ...)`
 *     inside `core/`. `set()` reaches subdirectories, never siblings, and the
 *     archive every test runner links -- `libmarkdown-core-public-static` --
 *     is built in `elements/`. The define reached one target out of five.
 *   - No preset configured `Debug`, so even that one target was never built
 *     by anything CI or `ctest` runs.
 *
 * This audit holds both halves against the artifacts rather than the text:
 * the define is read out of a real Debug configuration's compiler flags, and
 * the configuration is read out of the preset file and the CI graph.
 */

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const DEFINE = "MARKDOWN_CORE_DEBUG_NODES";

const failures = [];

/* 1. Exactly one place defines it, and that place is the package root.
 *
 * The regression this forbids is the original bug: the definition drifting
 * back down into a subdirectory, where it silently covers a subset of the
 * targets that compile the engine. */
const cmakeLists = [];
(function collect(dir) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) {
            collect(full);
        } else if (entry.name === "CMakeLists.txt") {
            cmakeLists.push(full);
        }
    }
})(path.join(root, "packages/markdown-core"));

const definers = cmakeLists.filter((file) => fs.readFileSync(file, "utf8").includes(DEFINE));
const packageRoot = path.join(root, "packages/markdown-core/CMakeLists.txt");
if (definers.length !== 1 || definers[0] !== packageRoot) {
    failures.push(
        `${DEFINE} must be set exactly once, in packages/markdown-core/CMakeLists.txt, before add_subdirectory; ` +
            `found it in [${definers.map((file) => path.relative(root, file)).join(", ") || "nothing"}]`
    );
}

/* 2. A Debug configure preset exists, a test preset runs it, and pnpm exposes
 *    it. Without this the define reaches every target and still never runs. */
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
    failures.push(`no test preset runs the "${debugPresetName}" configure preset; the check would never execute`);
}

/* 3. CI builds that tree and runs that suite. A preset nothing invokes is the
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

/* 4. The define actually lands on every C translation unit of the package in a
 *    real Debug configuration. This is the part that cannot be faked by
 *    reading source: it reads the generated compiler flags. */
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
    /* C only. The macro guards C engine code; the C++ facade test compiles no
     * engine source and CMAKE_CXX_FLAGS_DEBUG is deliberately untouched. */
    if (!/^C_DEFINES|^C_FLAGS/m.test(text)) {
        continue;
    }
    checked++;
    if (!text.includes(DEFINE)) {
        failures.push(`${path.relative(tree, file)} compiles C without ${DEFINE}`);
    }
}
if (!checked) {
    failures.push("the Debug tree generated no C compiler flag files; this audit is reaching nothing");
}

if (failures.length) {
    for (const failure of failures) {
        console.error(`audit-node-integrity-check: ${failure}`);
    }
    process.exit(1);
}
console.log(
    `audit-node-integrity-check: ${DEFINE} is set once at the package root and reaches ` +
        `all ${checked} C flag sets of the Debug tree; a preset and a CI job run it`
);
