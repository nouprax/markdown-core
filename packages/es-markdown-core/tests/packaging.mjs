import assert from "node:assert/strict";
import { copyFile, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const mode = process.argv[2] ?? "all";
if (!["all", "consumer", "packaging", "types"].includes(mode)) throw new Error(`unknown mode: ${mode}`);
const temporary = await mkdtemp(path.join(tmpdir(), "es-markdown-core-consumer-"));
try {
    const packed = spawnSync("npm", ["pack", packageDirectory, "--json", "--pack-destination", temporary], {
        encoding: "utf8",
        env: { ...process.env, npm_config_cache: path.join(temporary, "npm-cache") }
    });
    if (packed.status !== 0) throw new Error(packed.stderr);
    const report = JSON.parse(packed.stdout)[0];
    if (mode === "all" || mode === "packaging") {
        const files = report.files.map((file) => file.path).sort();
        const sourceFiles = await sourceArtifactFiles(path.join(packageDirectory, "src"));
        const expected = [
            "LICENSE",
            "README.md",
            ...sourceFiles.map((file) => `dist/${file}`),
            "dist/markdown-core.wasm",
            "package.json"
        ].sort();
        assert.deepEqual(files, expected);
        const manifest = JSON.parse(await readFile(path.join(packageDirectory, "package.json"), "utf8"));
        assert.deepEqual(Object.keys(manifest.exports), [".", "./markdown-core.wasm"]);
        console.log("packaging: npm artifact contents and exports passed");
    }

    if (mode !== "packaging") {
        await writeFile(path.join(temporary, "package.json"), JSON.stringify({ type: "module", private: true }));
        const installed = spawnSync("npm", ["install", "--ignore-scripts", `./${report.filename}`], {
            cwd: temporary,
            encoding: "utf8",
            env: { ...process.env, npm_config_cache: path.join(temporary, "npm-cache") }
        });
        if (installed.status !== 0) throw new Error(installed.stderr);
    }

    if (mode === "all" || mode === "types") {
        await copyFile(path.join(packageDirectory, "tests/types/consumer.ts"), path.join(temporary, "consumer.ts"));
        await copyFile(path.join(packageDirectory, "tests/types/tsconfig.json"), path.join(temporary, "tsconfig.json"));
        const typecheck = spawnSync(
            path.resolve(packageDirectory, "../../node_modules/.bin/tsc"),
            ["-p", "tsconfig.json"],
            { cwd: temporary, encoding: "utf8" }
        );
        if (typecheck.status !== 0) throw new Error(typecheck.stdout + typecheck.stderr);
        console.log("types: packed npm artifact resolved through exports.types successfully");
    }

    if (mode === "all" || mode === "consumer") {
        const consumer = spawnSync(
            "node",
            [
                "--input-type=module",
                "--eval",
                [
                    "import * as api from '@nouprax/es-markdown-core';",
                    "const d = api.Document.parse('# npm consumer');",
                    "if (d.content[0].kind !== 'heading') process.exit(2);",
                    "if ('memory' in api || 'initialize' in api) process.exit(3);",
                    "if (d.dump() !== api.TreeDumper.dump(d)) process.exit(4);"
                ].join("\n")
            ],
            { cwd: temporary, encoding: "utf8" }
        );
        if (consumer.status !== 0) throw new Error(consumer.stderr || `consumer exited ${consumer.status}`);
        console.log("consumer: packed npm artifact imported and parsed successfully");
    }
} finally {
    await rm(temporary, { recursive: true, force: true });
}

async function sourceArtifactFiles(directory, prefix = "") {
    const result = [];
    for (const entry of await readdir(directory, { withFileTypes: true })) {
        const relative = path.join(prefix, entry.name);
        if (entry.isDirectory()) {
            result.push(...(await sourceArtifactFiles(path.join(directory, entry.name), relative)));
        } else if (entry.name.endsWith(".ts")) {
            const modulePath = relative.slice(0, -3).split(path.sep).join("/");
            result.push(`${modulePath}.js`, `${modulePath}.d.ts`);
        }
    }
    return result;
}
