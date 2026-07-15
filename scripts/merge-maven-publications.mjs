#!/usr/bin/env node

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";

const [linuxBundle, macosBundle, output] = process.argv.slice(2).map((value) => path.resolve(value));
assert.ok(linuxBundle && macosBundle && output, "usage: merge-maven-publications.mjs LINUX_BUNDLE MACOS_BUNDLE OUTPUT");
assert.ok(existsSync(path.join(linuxBundle, "repository")), "Linux Maven repository is missing");
assert.ok(existsSync(path.join(macosBundle, "repository")), "macOS Maven repository is missing");
const macosJvmJar = path.join(macosBundle, "macos-jvm.jar");
assert.ok(existsSync(macosJvmJar), "macOS JVM payload JAR is missing");

rmSync(output, { recursive: true, force: true });
cpSync(path.join(linuxBundle, "repository"), output, { recursive: true });
copyUnique(path.join(macosBundle, "repository"), output);

for (const relativePath of walk(output)) {
    if (/maven-metadata(?:-local)?\.xml(?:\..+)?$/.test(relativePath)) {
        rmSync(path.join(output, relativePath));
    }
}

const version = readFileSync(path.resolve("VERSION"), "utf8").trim();
const jvmDirectory = path.join(output, "com/nouprax/kotlin-markdown-core-jvm", version);
const jvmJar = path.join(jvmDirectory, `kotlin-markdown-core-jvm-${version}.jar`);
assert.ok(existsSync(jvmJar), "Linux JVM publication JAR is missing");

const temporary = mkdtempSync(path.join(tmpdir(), "markdown-core-maven-merge-"));
try {
    const nativePath = "com/nouprax/markdown/core/native/macos-arm64/libmarkdown_core_kotlin.dylib";
    execFileSync("unzip", ["-qq", macosJvmJar, nativePath, "-d", temporary]);
    execFileSync("jar", ["--update", "--file", jvmJar, "-C", temporary, nativePath]);
} finally {
    rmSync(temporary, { recursive: true, force: true });
}

const modulePath = path.join(jvmDirectory, `kotlin-markdown-core-jvm-${version}.module`);
const moduleMetadata = JSON.parse(readFileSync(modulePath, "utf8"));
const bytes = readFileSync(jvmJar);
for (const variant of moduleMetadata.variants) {
    for (const file of variant.files ?? []) {
        if (file.url !== path.basename(jvmJar)) continue;
        file.size = bytes.length;
        file.sha512 = digest("sha512", bytes);
        file.sha256 = digest("sha256", bytes);
        file.sha1 = digest("sha1", bytes);
        file.md5 = digest("md5", bytes);
    }
}
writeFileSync(modulePath, `${JSON.stringify(moduleMetadata, null, 2)}\n`);

const entries = execFileSync("unzip", ["-Z1", jvmJar], { encoding: "utf8" });
for (const required of [
    "com/nouprax/markdown/core/native/linux-x64/libmarkdown_core_kotlin.so",
    "com/nouprax/markdown/core/native/macos-arm64/libmarkdown_core_kotlin.dylib"
]) {
    assert.ok(entries.split("\n").includes(required), `aggregated JVM JAR is missing ${required}`);
}

console.log(`Merged Linux and macOS Maven publications in ${output}`);

function copyUnique(source, destination) {
    for (const entry of readdirSync(source, { withFileTypes: true })) {
        const sourcePath = path.join(source, entry.name);
        const destinationPath = path.join(destination, entry.name);
        if (entry.isDirectory()) {
            if (!existsSync(destinationPath)) cpSync(sourcePath, destinationPath, { recursive: true });
            else copyUnique(sourcePath, destinationPath);
        } else {
            assert.ok(!existsSync(destinationPath), `host publication collision: ${destinationPath}`);
            cpSync(sourcePath, destinationPath);
        }
    }
}

function walk(directory, prefix = "") {
    const files = [];
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
        const relativePath = path.join(prefix, entry.name);
        if (entry.isDirectory()) files.push(...walk(path.join(directory, entry.name), relativePath));
        else if (statSync(path.join(directory, entry.name)).isFile()) files.push(relativePath);
    }
    return files;
}

function digest(algorithm, bytes) {
    return createHash(algorithm).update(bytes).digest("hex");
}
