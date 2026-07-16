#!/usr/bin/env node

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

const repository = path.resolve(process.argv[2] ?? "");
const full = process.argv.includes("--full");
const signed = process.argv.includes("--signed");
assert.ok(repository && existsSync(repository), "usage: audit-maven-publications.mjs REPOSITORY [--full] [--signed]");
const version = readFileSync(path.resolve("VERSION"), "utf8").trim();
const groupDirectory = path.join(repository, "com/nouprax");
const requiredCoordinates = [
    "kotlin-markdown-core",
    "kotlin-markdown-core-jvm",
    "kotlin-markdown-core-android",
    "kotlin-markdown-core-android-runtime"
];
if (full) requiredCoordinates.push("kotlin-markdown-core-linuxx64", "kotlin-markdown-core-macosarm64");

for (const coordinate of requiredCoordinates) {
    const directory = path.join(groupDirectory, coordinate, version);
    assert.ok(existsSync(directory), `missing Maven coordinate com.nouprax:${coordinate}:${version}`);
    const files = readdirSync(directory);
    for (const extension of [".pom", ".module"]) {
        assert.ok(
            files.some((file) => file === `${coordinate}-${version}${extension}`),
            `${coordinate} is missing ${extension}`
        );
    }
    assert.ok(
        files.some((file) => file.endsWith("-sources.jar")),
        `${coordinate} is missing sources`
    );
    assert.ok(
        files.some((file) => file.endsWith("-javadoc.jar")),
        `${coordinate} is missing javadoc`
    );
    auditPom(path.join(directory, `${coordinate}-${version}.pom`), coordinate);
    auditModule(path.join(directory, `${coordinate}-${version}.module`));
}

if (full) {
    requireArtifact("kotlin-markdown-core-jvm", ".jar", ["-sources.jar", "-javadoc.jar"]);
    requireArtifact("kotlin-markdown-core-android", ".aar");
    requireArtifact("kotlin-markdown-core-android-runtime", ".aar");
    requireArtifact("kotlin-markdown-core-linuxx64", ".klib");
    requireArtifact("kotlin-markdown-core-macosarm64", ".klib");

    const rootModulePath = path.join(
        groupDirectory,
        "kotlin-markdown-core",
        version,
        `kotlin-markdown-core-${version}.module`
    );
    const rootModule = JSON.parse(readFileSync(rootModulePath, "utf8"));
    const referencedModules = new Set(
        rootModule.variants.flatMap((variant) => (variant["available-at"] ? [variant["available-at"].module] : []))
    );
    for (const coordinate of [
        "kotlin-markdown-core-jvm",
        "kotlin-markdown-core-android",
        "kotlin-markdown-core-linuxx64",
        "kotlin-markdown-core-macosarm64"
    ]) {
        assert.ok(referencedModules.has(coordinate), `root Gradle metadata does not reference ${coordinate}`);
    }

    const jvmDirectory = path.join(groupDirectory, "kotlin-markdown-core-jvm", version);
    const jvmJar = path.join(jvmDirectory, `kotlin-markdown-core-jvm-${version}.jar`);
    const entries = execFileSync("unzip", ["-Z1", jvmJar], { encoding: "utf8" }).trim().split("\n");
    for (const required of [
        "com/nouprax/markdown/core/native/linux-x64/libmarkdown_core_kotlin.so",
        "com/nouprax/markdown/core/native/macos-arm64/libmarkdown_core_kotlin.dylib"
    ]) {
        assert.ok(entries.includes(required), `JVM artifact is missing ${required}`);
    }
    assert.ok(
        entries.every((entry) => !/(^|\/)(canonical-ast|manifest\.json)(\/|$)|\.ast$/.test(entry)),
        "JVM artifact contains shared conformance data"
    );
}

const repositoryFiles = walk(repository);
for (const archive of repositoryFiles.filter((file) => /\.(?:aar|jar|klib)$/u.test(file))) {
    auditProductArchive(path.join(repository, archive), archive);
}
const namespace = path.join("com", "nouprax") + path.sep;
assert.ok(
    repositoryFiles.every((file) => file.startsWith(namespace)),
    "Maven publication repository contains files outside com.nouprax"
);
assert.ok(
    repositoryFiles.every((file) => path.basename(file) !== "_remote.repositories"),
    "Maven publication repository contains local-repository markers"
);
if (full) {
    assert.ok(
        repositoryFiles.every((file) => !/maven-metadata(?:-local)?\.xml/.test(file)),
        "Central bundle contains Maven metadata"
    );
}
if (signed) {
    const baseArtifacts = repositoryFiles.filter(
        (file) =>
            !file.endsWith(".asc") &&
            !file.endsWith(".md5") &&
            !file.endsWith(".sha1") &&
            !file.endsWith(".sha256") &&
            !file.endsWith(".sha512")
    );
    assert.ok(baseArtifacts.length > 0, "Maven repository is empty");
    for (const artifact of baseArtifacts) {
        for (const suffix of [".asc", ".md5", ".sha1", ".sha256", ".sha512"]) {
            assert.ok(existsSync(`${path.join(repository, artifact)}${suffix}`), `${artifact} is missing ${suffix}`);
        }
        verifyChecksum(path.join(repository, artifact), "md5");
        verifyChecksum(path.join(repository, artifact), "sha1");
        verifyChecksum(path.join(repository, artifact), "sha256");
        verifyChecksum(path.join(repository, artifact), "sha512");
        verifyChecksum(`${path.join(repository, artifact)}.asc`, "md5");
        verifyChecksum(`${path.join(repository, artifact)}.asc`, "sha1");
        verifyChecksum(`${path.join(repository, artifact)}.asc`, "sha256");
        verifyChecksum(`${path.join(repository, artifact)}.asc`, "sha512");
    }
}

console.log(`Maven publication audit passed for ${version}${full ? " (full host aggregate)" : ""}.`);

function auditPom(file, coordinate) {
    const pom = readFileSync(file, "utf8");
    for (const expected of [
        "<groupId>com.nouprax</groupId>",
        `<artifactId>${coordinate}</artifactId>`,
        `<version>${version}</version>`,
        "<name>Kotlin Markdown Core",
        "<url>https://github.com/nouprax/markdown-core</url>",
        "<name>BSD-2-Clause</name>",
        "<scm>",
        "<developers>",
        "<id>nouprax</id>"
    ]) {
        assert.ok(pom.includes(expected), `${path.basename(file)} is missing POM metadata: ${expected}`);
    }
    assert.ok(!pom.includes("SNAPSHOT"), `${path.basename(file)} contains a snapshot version`);
    assert.ok(!/<scope>test<\/scope>/iu.test(pom), `${path.basename(file)} publishes a test-scoped dependency`);
    assert.ok(
        !/(?:junit|kotlin-test|testng|mockito|kotest|hamcrest|opentest4j)/iu.test(pom),
        `${path.basename(file)} publishes a test framework dependency`
    );
}

function auditModule(file) {
    const metadata = JSON.parse(readFileSync(file, "utf8"));
    const component = metadata.component;
    if (component.group) assert.equal(component.group, "com.nouprax");
    assert.equal(component.version, version);
    for (const variant of metadata.variants) {
        if (variant["available-at"]) {
            assert.equal(variant["available-at"].group, "com.nouprax");
            assert.equal(variant["available-at"].version, version);
            const resolved = path.resolve(path.dirname(file), variant["available-at"].url);
            if (full) assert.ok(existsSync(resolved), `${path.basename(file)} references missing ${resolved}`);
        }
        for (const artifact of variant.files ?? []) {
            const artifactPath = path.join(path.dirname(file), artifact.url);
            assert.ok(existsSync(artifactPath), `${path.basename(file)} references missing ${artifact.url}`);
            const bytes = readFileSync(artifactPath);
            assert.equal(artifact.size, bytes.length, `${artifact.url} size drifted from module metadata`);
            for (const algorithm of ["sha512", "sha256", "sha1", "md5"]) {
                assert.equal(artifact[algorithm], digest(algorithm, bytes), `${artifact.url} ${algorithm} drifted`);
            }
        }
        for (const dependency of variant.dependencies ?? []) {
            const coordinate = `${dependency.group ?? ""}:${dependency.module ?? ""}`;
            assert.ok(
                !/(?:junit|kotlin-test|testng|mockito|kotest|hamcrest|opentest4j)/iu.test(coordinate),
                `${path.basename(file)} publishes test dependency ${coordinate}`
            );
        }
    }
}

function auditProductArchive(file, displayName) {
    if (statSync(file).size === 0) return;
    let listing;
    try {
        listing = execFileSync("unzip", ["-Z1", file], { encoding: "utf8" });
    } catch (error) {
        if (String(error.stdout ?? "").trim() === "Empty zipfile.") return;
        throw error;
    }
    const entries = listing.trim().split("\n").filter(Boolean);
    const forbidden = entries.filter(
        (entry) =>
            /(^|\/)(?:tests?|testfixtures?|androidtest|commontest|jvmtest|linuxx64test|macosarm64test|fixtures?|benchmarks?|consumers?)(\/|$)/iu.test(
                entry
            ) ||
            /(^|\/)[^/]*(?:test|tests|benchmark)[^/]*\.(?:class|java|kt|kotlin_metadata)$/iu.test(entry) ||
            /(^|\/)(?:canonical-ast|manifest\.json)(\/|$)|\.ast$/iu.test(entry)
    );
    assert.deepEqual(forbidden, [], `${displayName} contains test-only entries: ${forbidden.join(", ")}`);

    if (file.endsWith(".aar")) {
        const extracted = mkdtempSync(path.join(tmpdir(), "markdown-core-aar-audit-"));
        try {
            execFileSync("unzip", ["-qq", file, "-d", extracted]);
            for (const nested of walk(extracted).filter((entry) => entry.endsWith(".jar"))) {
                auditProductArchive(path.join(extracted, nested), `${displayName}!/${nested}`);
            }
        } finally {
            rmSync(extracted, { recursive: true, force: true });
        }
    }
}

function requireArtifact(coordinate, suffix, excludedSuffixes = []) {
    const directory = path.join(groupDirectory, coordinate, version);
    const matches = readdirSync(directory).filter(
        (file) => file.endsWith(suffix) && excludedSuffixes.every((excluded) => !file.endsWith(excluded))
    );
    assert.ok(matches.length > 0, `${coordinate} is missing ${suffix}`);
}

function verifyChecksum(file, algorithm) {
    const recorded = readFileSync(`${file}.${algorithm}`, "utf8").trim();
    assert.equal(recorded, digest(algorithm, readFileSync(file)), `${file}.${algorithm} does not match`);
}

function digest(algorithm, bytes) {
    return createHash(algorithm).update(bytes).digest("hex");
}

function walk(directory, prefix = "") {
    const result = [];
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
        const relativePath = path.join(prefix, entry.name);
        const absolutePath = path.join(directory, entry.name);
        if (entry.isDirectory()) result.push(...walk(absolutePath, relativePath));
        else if (statSync(absolutePath).isFile()) result.push(relativePath);
    }
    return result;
}
