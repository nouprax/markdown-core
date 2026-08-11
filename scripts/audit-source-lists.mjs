#!/usr/bin/env node
/**
 * Source-list audit.
 *
 * The C library's file list is written out by hand FIVE times, in five
 * syntaxes, because five build systems consume it and none of them can read
 * another's. Nothing has ever checked that the five agree, and on
 * 2026-08-10 three of them did not: the ES build still compiled four files
 * that had been deleted (`session.c`, `adopt.c`, `incremental.c`,
 * `lookups.c`) and was missing three that exist, so the ES package shipped
 * without `concrete.c` in it at all; the Android build was missing
 * `concrete.c`; and both SwiftPM manifests listed the same four ghosts.
 *
 * Each list rotted differently, which is the tell: they are not five copies
 * of one fact, they are five facts that happen to have been equal once.
 *
 * The CMake graph is the authority because it is the one that is built and
 * tested on every commit. The other four follow it. Order is not checked —
 * a compiler does not care — but membership is, and so is existence: a list
 * naming a file that is not there is how all three of these broke.
 *
 *   node scripts/audit-source-lists.mjs [--fix]
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");

/** Every `*.c` in a CMake `set(...)` block, which is how the authority spells it. */
function cmakeSources(relative, variable) {
    const text = read(relative);
    const start = text.indexOf(`set(${variable}`);
    if (start < 0) {
        throw new Error(`${relative}: no set(${variable} ...) block`);
    }
    const end = text.indexOf(")", start);
    return [...text.slice(start, end).matchAll(/([A-Za-z0-9_-]+\.c)\b/g)].map((m) => m[1]);
}

const authority = {
    core: cmakeSources("packages/markdown-core/core/CMakeLists.txt", "LIBRARY_SOURCES"),
    extensions: cmakeSources("packages/markdown-core/extensions/CMakeLists.txt", "LIBRARY_SOURCES")
};

/** The `*.c` names a follower lists for one directory.
 *
 * Each follower spells the same list its own way — a SwiftPM manifest wants
 * `"core/blocks.c"`, an ES build script keeps two plain arrays and joins the
 * directory on afterwards, an Android CMakeLists interpolates a directory
 * variable — so each gets its own reader rather than one regex pretending
 * they are the same shape. A reader that quietly matches nothing is the
 * failure mode here, so an empty result is an error, not an empty set.
 */
function names(relative, label, pattern) {
    const found = [...read(relative).matchAll(pattern)].map((m) => m.groups.file);
    if (found.length === 0) {
        throw new Error(`${relative}: the ${label} reader matched nothing; the file's shape changed`);
    }
    return found;
}

/** The names inside `const <name> = [ ... ]`, which is how the ES build spells it. */
function esArray(relative, variable) {
    const text = read(relative);
    const start = text.indexOf(`const ${variable} = [`);
    if (start < 0) {
        throw new Error(`${relative}: no const ${variable} = [ ... ]`);
    }
    const end = text.indexOf("]", start);
    return [...text.slice(start, end).matchAll(/"([A-Za-z0-9_-]+\.c)"/g)].map((m) => m[1]);
}

const swiftPattern = (dir) => new RegExp(`"${dir}\\/(?<file>[A-Za-z0-9_-]+\\.c)"`, "g");
const androidPattern = (dir) => new RegExp(`MARKDOWN_CORE_${dir}_DIR\\}\\/(?<file>[A-Za-z0-9_-]+\\.c)`, "g");

const followers = {
    "Package.swift": {
        core: names("Package.swift", "core", swiftPattern("core")),
        extensions: names("Package.swift", "extensions", swiftPattern("extensions"))
    },
    "packages/swift-markdown-core/Package.release.swift": {
        core: names("packages/swift-markdown-core/Package.release.swift", "core", swiftPattern("core")),
        extensions: names(
            "packages/swift-markdown-core/Package.release.swift",
            "extensions",
            swiftPattern("extensions")
        )
    },
    "packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt": {
        core: names(
            "packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt",
            "core",
            androidPattern("CORE")
        ),
        extensions: names(
            "packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt",
            "extensions",
            androidPattern("EXTENSIONS")
        )
    },
    "packages/es-markdown-core/scripts/build.mjs": {
        core: esArray("packages/es-markdown-core/scripts/build.mjs", "core"),
        extensions: esArray("packages/es-markdown-core/scripts/build.mjs", "extensions")
    }
};

let failed = false;
const report = (message) => {
    console.error(message);
    failed = true;
};

// Existence first: a list naming a file that is not there is the failure all
// three of the rotted lists actually had.
const directories = { core: "packages/markdown-core/core", extensions: "packages/markdown-core/extensions" };
for (const [name, buckets] of [["CMake (authority)", authority], ...Object.entries(followers)]) {
    for (const [bucket, files] of Object.entries(buckets)) {
        for (const file of files) {
            if (!fs.existsSync(path.join(root, directories[bucket], file))) {
                report(`${name}: names ${bucket}/${file}, which does not exist`);
            }
        }
    }
}

const expected = new Set([
    ...authority.core.map((f) => `core/${f}`),
    ...authority.extensions.map((f) => `extensions/${f}`)
]);

for (const [name, buckets] of Object.entries(followers)) {
    const actual = new Set([
        ...buckets.core.map((f) => `core/${f}`),
        ...buckets.extensions.map((f) => `extensions/${f}`)
    ]);
    const missing = [...expected].filter((f) => !actual.has(f)).sort();
    const extra = [...actual].filter((f) => !expected.has(f)).sort();
    if (missing.length) {
        report(`${name}: missing ${missing.join(", ")}`);
    }
    if (extra.length) {
        report(`${name}: names ${extra.join(", ")}, which the CMake graph does not build`);
    }
}

if (failed) {
    console.error("\nSource-list audit failed: the five hand-written lists do not agree.");
    process.exit(1);
}
console.log(`Source-list audit passed: ${expected.size} sources, 5 lists in agreement.`);
