#!/usr/bin/env node

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const version = readFileSync(path.join(root, "VERSION"), "utf8").trim();
const stableSemver = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
assert.match(version, stableSemver, "VERSION must be a stable semantic version");

const expectedTag = `v${version}`;
const tagArgument = process.argv.find((argument) => argument.startsWith("--tag="));
if (tagArgument) {
    assert.equal(tagArgument.slice("--tag=".length), expectedTag, `release tag must be ${expectedTag}`);
}

const npmManifest = json("packages/es-markdown-core/package.json");
assert.equal(npmManifest.name, "@nouprax/es-markdown-core");
assert.equal(npmManifest.version, version, "npm version drifted from VERSION");
assert.equal(npmManifest.private, false);
assert.equal(npmManifest.publishConfig?.access, "public");
assert.equal(npmManifest.repository?.url, "git+https://github.com/nouprax/markdown-core.git");
assert.equal(npmManifest.repository?.directory, "packages/es-markdown-core");

const exactVersionFiles = [
    ["packages/markdown-core/core/include/markdown-core-version.h", `MARKDOWN_CORE_VERSION_STRING "${version}"`],
    ["README.md", `.package(url: "https://github.com/nouprax/markdown-core", from: "${version}")`],
    ["README.md", `implementation("com.nouprax:kotlin-markdown-core:${version}")`],
    ["packages/kotlin-markdown-core/README.md", `com.nouprax:kotlin-markdown-core:${version}`],
    ["packages/kotlin-markdown-core/consumers/kmp/build.gradle.kts", `com.nouprax:kotlin-markdown-core:${version}`],
    [
        "packages/kotlin-markdown-core/consumers/jvm-gradle/build.gradle.kts",
        `com.nouprax:kotlin-markdown-core-jvm:${version}`
    ],
    ["packages/kotlin-markdown-core/consumers/android/build.gradle.kts", `com.nouprax:kotlin-markdown-core:${version}`],
    ["packages/kotlin-markdown-core/consumers/jvm-maven/pom.xml", `<version>${version}</version>`]
];
for (const [relativePath, expected] of exactVersionFiles) {
    assert.ok(text(relativePath).includes(expected), `${relativePath} drifted from VERSION (${expected})`);
}

for (const relativePath of ["CMakeLists.txt", "packages/markdown-core/CMakeLists.txt"]) {
    assert.ok(text(relativePath).includes("VERSION"), `${relativePath} must derive the project version from VERSION`);
}
for (const relativePath of [
    "packages/kotlin-markdown-core/build.gradle.kts",
    "packages/kotlin-markdown-core/android-runtime/build.gradle.kts"
]) {
    assert.ok(
        text(relativePath).includes('rootProject.file("VERSION").readText().trim()'),
        `${relativePath} must derive the publication version from VERSION`
    );
}

if (!process.argv.includes("--skip-swift")) {
    const packageDump = JSON.parse(
        execFileSync("swift", ["package", "--disable-sandbox", "dump-package"], {
            cwd: root,
            encoding: "utf8",
            env: { ...process.env, CLANG_MODULE_CACHE_PATH: path.join(root, "build/swift-module-cache") }
        })
    );
    assert.equal(packageDump.name, "swift-markdown-core");
    const product = packageDump.products.find((candidate) => candidate.name === "MarkdownCore");
    assert.ok(product, "SwiftPM MarkdownCore product is missing");
    assert.deepEqual(product.targets, ["MarkdownCore"]);
}

const tags = execFileSync("git", ["tag", "--list"], { cwd: root, encoding: "utf8" }).trim().split("\n").filter(Boolean);
for (const tag of tags) {
    assert.match(tag, /^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/, `unexpected legacy tag: ${tag}`);
}
if (tags.length > 0 && !tags.includes(expectedTag)) {
    assert.ok(
        tags.every((tag) => compareSemver(tag.slice(1), version) < 0),
        `tag namespace contains a version at or after ${expectedTag}`
    );
}

console.log(`Release version contract passed for ${version} (${expectedTag}).`);

function text(relativePath) {
    return readFileSync(path.join(root, relativePath), "utf8");
}

function json(relativePath) {
    return JSON.parse(text(relativePath));
}

function compareSemver(left, right) {
    const leftParts = left.split(".").map(Number);
    const rightParts = right.split(".").map(Number);
    for (let index = 0; index < 3; index += 1) {
        if (leftParts[index] !== rightParts[index]) return leftParts[index] - rightParts[index];
    }
    return 0;
}
