#!/usr/bin/env node
/**
 * The repository's single coverage gate.
 *
 * Every execution platform's producer ends by calling this script with its
 * toolchain-native report. The thresholds, the exemptions, and the record of
 * live in `specs/coverage/policy.json` and nowhere else, so no producer can
 * hold a second opinion about whether its platform passed.
 *
 *   node scripts/check-coverage.mjs --platform <id> --format <fmt> --input <file>
 *                                   [--source-root <dir>]... [--update-ledger]
 *
 * Formats: llvm-cov (C, Swift), lcov (ES), jacoco (Kotlin).
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
    applyPathRewrites,
    enforce,
    formatPercent,
    fromJacocoXml,
    fromLcov,
    fromLlvmCovExport,
    loadPolicy,
    rewriteLedger,
    UNRESOLVED_PREFIX
} from "./lib/coverage.mjs";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

function usage(message) {
    process.stderr.write(`${message}\n`);
    process.stderr.write(
        "usage: check-coverage.mjs --platform <id> --format <llvm-cov|lcov|jacoco> --input <file>" +
            " [--source-root <dir>]... [--update-ledger]\n"
    );
    process.exit(2);
}

function parseArguments(argv) {
    const options = { sourceRoots: [], updateLedger: false };
    for (let index = 0; index < argv.length; index += 1) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (flag === "--update-ledger") {
            options.updateLedger = true;
            continue;
        }
        if (value === undefined) usage(`missing value for ${flag}`);
        if (flag === "--platform") options.platform = value;
        else if (flag === "--format") options.format = value;
        else if (flag === "--input") options.input = value;
        else if (flag === "--base") options.base = value;
        else if (flag === "--source-root") options.sourceRoots.push(value);
        else usage(`unknown argument: ${flag}`);
        index += 1;
    }
    if (!options.platform) usage("--platform is required");
    if (!options.format) usage("--format is required");
    if (!options.input) usage("--input is required");
    return options;
}

const options = parseArguments(process.argv.slice(2));
const inputPath = path.resolve(root, options.input);
if (!fs.existsSync(inputPath)) {
    process.stderr.write(`coverage gate: no report at ${options.input}\n`);
    process.exit(1);
}
const text = fs.readFileSync(inputPath, "utf8");
const { policy, policyPath } = loadPolicy(root);
const configuration = policy.platforms?.[options.platform];
if (!configuration) usage(`specs/coverage/policy.json declares no platform "${options.platform}"`);
const context = { root, base: options.base ? path.resolve(root, options.base) : root };

let files;
if (options.format === "llvm-cov") files = fromLlvmCovExport(text, context);
else if (options.format === "lcov") files = fromLcov(text, context);
else if (options.format === "jacoco") files = fromJacocoXml(text, context, options.sourceRoots);
else usage(`unknown --format: ${options.format}`);

const unresolved = files.filter((file) => file.path.startsWith(UNRESOLVED_PREFIX));
if (unresolved.length) {
    // Silently dropping these would shrink the measured set, and a smaller
    // measured set reads as better coverage. The gate refuses the report
    // instead.
    process.stderr.write(
        `coverage gate [${options.platform}]: ${String(unresolved.length)} reported file(s) could not be resolved to a repository path:\n`
    );
    for (const file of unresolved) {
        process.stderr.write(`  ${file.path.slice(UNRESOLVED_PREFIX.length)}\n`);
    }
    process.stderr.write("Add the missing --source-root, or fix the producer.\n");
    process.exit(1);
}

files = applyPathRewrites(files, configuration.pathRewrite);
const result = enforce({ policy, platform: options.platform, files });

if (result.measured.length === 0) {
    // An empty measured set is the failure mode a coverage gate is most likely
    // to hide: a producer that instruments nothing, resolves no path, or runs
    // no test reports a clean sheet unless the gate refuses one.
    process.stderr.write(
        `coverage gate [${options.platform}]: the report covers no file under the policy's include roots.\n` +
            "This means the producer instrumented nothing or emitted unresolvable paths, not that coverage is complete.\n"
    );
    process.exit(1);
}

if (options.updateLedger) {
    const remaining = rewriteLedger({
        policy,
        policyPath,
        platform: options.platform,
        measured: result.measured,
        required: result.required
    });
    process.stdout.write(
        `coverage ledger [${options.platform}]: rewrote specs/coverage/policy.json — ${String(remaining)} file(s) still short of 100%.\n`
    );
    process.exit(0);
}

const width = 10;
process.stdout.write(`coverage [${options.platform}] over ${String(result.measured.length)} measured file(s)\n`);
for (const metric of result.declaredMetrics) {
    const totals = result.totals[metric];
    const note = result.unsupported.has(metric) ? "  (declared unsupported on this platform)" : "";
    process.stdout.write(
        `  ${metric.padEnd(width)} ${String(totals.covered)}/${String(totals.total)} = ${formatPercent(totals)}${note}\n`
    );
}
if (result.skipped.length) {
    process.stdout.write(`  exempt     ${String(result.skipped.length)} file(s) declared in the policy\n`);
}

if (result.tightenable.length) {
    process.stdout.write(
        `\ncoverage ledger [${options.platform}]: ${String(result.tightenable.length)} entr(y|ies) now loose or obsolete:\n`
    );
    for (const file of result.tightenable) process.stdout.write(`  ${file.path}\n`);
    process.stdout.write("Run the platform's coverage task with --update-ledger to record the improvement.\n");
}

if (result.failures.length) {
    process.stderr.write(`\ncoverage gate [${options.platform}] FAILED — ${String(result.failures.length)} file(s):\n`);
    for (const failure of result.failures) {
        const reason = {
            "below-target": "below the 100% target and not in the ledger",
            regressed: "regressed past its recorded ledger allowance",
            "lost-files": "the report lost files it used to measure",
            unmeasured: "a required metric was never measured",
            "unexpectedly-measured": "a metric declared unsupported is in fact available"
        }[failure.kind];
        process.stderr.write(`  ${failure.path}\n    ${reason}: ${failure.detail}\n`);
    }
    process.stderr.write(
        "\nA file reaches this gate at 100% or it does not land. Unpinned surface is recorded in\n" +
            "specs/coverage/policy.json and may only shrink; see docs/specs/test-architecture.md.\n"
    );
    process.exit(1);
}

process.stdout.write(`\ncoverage gate [${options.platform}] passed.\n`);
