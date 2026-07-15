#!/usr/bin/env node

import { lstat, mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import path from "node:path";

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index];
    const value = process.argv[index + 1];
    if (!key?.startsWith("--") || value === undefined) {
        throw new Error("usage: collect-pr-metrics.mjs --platform NAME --logs DIR --output FILE");
    }
    args.set(key.slice(2), value);
}

const platform = args.get("platform");
const logsDirectory = args.get("logs");
const output = args.get("output");
if (!platform || !logsDirectory || !output) {
    throw new Error("--platform, --logs, and --output are required");
}

const allowedRuntimes = new Set(["c", "swift", "kotlin", "es"]);
const allowedWorkloads = new Set(["representative_large", "large_document", "deep_nesting"]);

async function filesBelow(root) {
    const files = [];
    async function visit(current) {
        let entries;
        try {
            entries = await readdir(current, { withFileTypes: true });
        } catch {
            return;
        }
        for (const entry of entries) {
            const entryPath = path.join(current, entry.name);
            if (entry.isDirectory()) {
                await visit(entryPath);
            } else if (entry.isFile()) {
                files.push(entryPath);
            }
        }
    }
    await visit(root);
    return files;
}

const benchmarks = [];
const logFiles = await filesBelow(logsDirectory);
for (const logFile of logFiles.sort()) {
    const contents = await readFile(logFile, "utf8");
    for (const line of contents.split(/\r?\n/u)) {
        if (!line.includes("runtime=") || !line.includes("median_ns=")) continue;
        const fields = new Map();
        for (const token of line.split(/\s+/u)) {
            const separator = token.indexOf("=");
            if (separator > 0) fields.set(token.slice(0, separator), token.slice(separator + 1));
        }
        const runtime = fields.get("runtime");
        const workload = fields.get("workload");
        const medianNs = Number(fields.get("median_ns"));
        if (
            !allowedRuntimes.has(runtime) ||
            !allowedWorkloads.has(workload) ||
            !Number.isSafeInteger(medianNs) ||
            medianNs < 0
        ) {
            continue;
        }
        const memoryText = fields.get("peak_rss_kib") ?? fields.get("rss_kib");
        const memoryKiB = Number(memoryText);
        benchmarks.push({
            runtime,
            workload,
            medianNs,
            memoryKiB: Number.isSafeInteger(memoryKiB) && memoryKiB >= 0 ? memoryKiB : null
        });
    }
}

const sizeDefinitions =
    platform === "linux"
        ? [
              {
                  name: "c-shared-library",
                  root: "build/cmake",
                  matches: (file) => /libmarkdown-core\.so(?:\.\d+)*$/u.test(file)
              },
              {
                  name: "kotlin-jvm-jar",
                  root: "packages/kotlin-markdown-core/build/libs",
                  matches: (file) =>
                      /kotlin-markdown-core-jvm-[^/]+\.jar$/u.test(file) &&
                      !/(?:sources|javadoc|metadata)\.jar$/u.test(file)
              },
              {
                  name: "es-wasm",
                  root: "packages/es-markdown-core/dist",
                  matches: (file) => file.endsWith("/markdown-core.wasm")
              }
          ]
        : [];

const sizes = [];
for (const definition of sizeDefinitions) {
    const candidates = (await filesBelow(definition.root)).filter(definition.matches);
    let largest = null;
    for (const candidate of candidates) {
        const candidateLstat = await lstat(candidate);
        if (candidateLstat.isSymbolicLink()) continue;
        const candidateStat = await stat(candidate);
        if (!largest || candidateStat.size > largest.bytes) {
            largest = { name: definition.name, bytes: candidateStat.size };
        }
    }
    if (largest) sizes.push(largest);
}

const uniqueBenchmarks = [
    ...new Map(benchmarks.map((benchmark) => [`${benchmark.runtime}:${benchmark.workload}`, benchmark])).values()
].sort((left, right) => `${left.runtime}:${left.workload}`.localeCompare(`${right.runtime}:${right.workload}`));

await mkdir(path.dirname(output), { recursive: true });
await writeFile(output, `${JSON.stringify({ schema: 1, platform, benchmarks: uniqueBenchmarks, sizes }, null, 2)}\n`);
