#!/usr/bin/env node

import { lstat, mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import path from "node:path";

const command = process.argv[2];
const args = new Map();
for (let index = 3; index < process.argv.length; index += 2) {
    const key = process.argv[index];
    const value = process.argv[index + 1];
    if (!key?.startsWith("--") || value === undefined) {
        throw new Error(usage());
    }
    args.set(key.slice(2), value);
}

if (command === "collect") {
    await collect();
} else if (command === "validate") {
    await validate();
} else {
    throw new Error(usage());
}

async function collect() {
    const log = required("log");
    const buildDirectory = required("build-dir");
    const output = required("output");
    const sourceSha = required("source-sha");
    const origin = required("origin");
    if (!new Set(["head", "main-build", "fallback-build"]).has(origin)) {
        throw new Error(`unsupported benchmark origin: ${origin}`);
    }

    const metricLines = (await readFile(log, "utf8"))
        .split(/\r?\n/u)
        .filter((line) => line.startsWith("metric ") || line.startsWith("baseline "));
    if (metricLines.length !== 1) {
        throw new Error(`expected exactly one benchmark metric line, found ${metricLines.length}`);
    }

    const fields = new Map();
    for (const token of metricLines[0].split(/\s+/u).slice(1)) {
        const separator = token.indexOf("=");
        if (separator <= 0) throw new Error(`malformed benchmark field: ${token}`);
        const name = token.slice(0, separator);
        if (fields.has(name)) throw new Error(`duplicate benchmark field: ${name}`);
        fields.set(name, token.slice(separator + 1));
    }
    // Baselines produced before this schema existed named the one fixed C
    // operation as a boundary. Accept that exact legacy spelling when an old
    // PR base must be built, then discard it: it is not a comparison dimension.
    if (fields.has("boundary")) {
        if (fields.get("boundary") !== "native_parse") {
            throw new Error(`unsupported legacy benchmark boundary: ${fields.get("boundary")}`);
        }
        fields.delete("boundary");
    }
    if (!fields.has("workload_version")) fields.set("workload_version", "1");
    const expectedFields = [
        "runtime",
        "workload",
        "workload_version",
        "bytes",
        "warmup",
        "repeats",
        "median_ns",
        "peak_rss_kib"
    ];
    if ([...fields.keys()].sort().join("\n") !== [...expectedFields].sort().join("\n")) {
        throw new Error(`benchmark fields changed: ${[...fields.keys()].sort().join(", ")}`);
    }

    const document = {
        schema: 1,
        sourceSha,
        origin,
        runtime: fields.get("runtime"),
        workload: fields.get("workload"),
        workloadVersion: integer(fields, "workload_version"),
        bytes: integer(fields, "bytes"),
        warmup: integer(fields, "warmup"),
        repeats: integer(fields, "repeats"),
        medianNs: integer(fields, "median_ns"),
        memoryKiB: integer(fields, "peak_rss_kib"),
        binaryBytes: await sharedLibrarySize(buildDirectory)
    };
    validateDocument(document, sourceSha);
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, `${JSON.stringify(document, null, 2)}\n`);
}

async function validate() {
    const input = required("input");
    const sourceSha = required("source-sha");
    const document = JSON.parse(await readFile(input, "utf8"));
    validateDocument(document, sourceSha);
}

function validateDocument(document, sourceSha) {
    if (!/^[0-9a-f]{40}$/u.test(sourceSha)) throw new Error(`invalid source SHA: ${sourceSha}`);
    const exactKeys = [
        "binaryBytes",
        "bytes",
        "medianNs",
        "memoryKiB",
        "origin",
        "repeats",
        "runtime",
        "schema",
        "sourceSha",
        "warmup",
        "workload",
        "workloadVersion"
    ];
    if (!document || Object.keys(document).sort().join("\n") !== exactKeys.join("\n")) {
        throw new Error("benchmark document fields changed");
    }
    if (
        document.schema !== 1 ||
        document.sourceSha !== sourceSha ||
        !new Set(["head", "main-build", "fallback-build"]).has(document.origin) ||
        document.runtime !== "c" ||
        document.workload !== "representative_large" ||
        document.workloadVersion !== 1 ||
        !positive(document.bytes) ||
        !nonnegative(document.warmup) ||
        !positive(document.repeats) ||
        !positive(document.medianNs) ||
        !nonnegative(document.memoryKiB) ||
        !positive(document.binaryBytes)
    ) {
        throw new Error("invalid benchmark document");
    }
}

async function sharedLibrarySize(root) {
    const candidates = [];
    async function visit(directory) {
        for (const entry of await readdir(directory, { withFileTypes: true })) {
            const entryPath = path.join(directory, entry.name);
            if (entry.isDirectory()) {
                await visit(entryPath);
            } else if (entry.isFile() && /^libmarkdown-core(?:\.so(?:\.\d+)*|(?:\.\d+)*\.dylib)$/u.test(entry.name)) {
                if (!(await lstat(entryPath)).isSymbolicLink()) candidates.push(entryPath);
            }
        }
    }
    await visit(root);
    if (candidates.length === 0) throw new Error("built markdown-core shared library was not found");
    const sizes = await Promise.all(candidates.map(async (candidate) => (await stat(candidate)).size));
    return Math.max(...sizes);
}

function integer(fields, name) {
    const value = Number(fields.get(name));
    if (!Number.isSafeInteger(value)) throw new Error(`${name} is not an integer`);
    return value;
}

function positive(value) {
    return Number.isSafeInteger(value) && value > 0;
}

function nonnegative(value) {
    return Number.isSafeInteger(value) && value >= 0;
}

function required(name) {
    const value = args.get(name);
    if (!value) throw new Error(`--${name} is required\n${usage()}`);
    return value;
}

function usage() {
    return "usage: pr-benchmark-result.mjs collect --log FILE --build-dir DIR --output FILE --source-sha SHA --origin ORIGIN | validate --input FILE --source-sha SHA";
}
