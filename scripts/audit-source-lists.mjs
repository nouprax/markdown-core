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
 * tested on every commit. The others follow it. Order is not checked —
 * a compiler does not care — but membership is, and so is existence: a list
 * naming a file that is not there is how all three of these broke.
 *
 * FOUR of the five are here today. The fifth is registered as absent with an
 * owner and printed on every run; see `REGISTERED_ABSENT` below.
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

/**
 * A follower this repository does not have yet.
 *
 * `packages/swift-markdown-core/Package.release.swift` is the trimmed SwiftPM
 * manifest the source archive is published from. It postdates `580d10c` and
 * arrived here with Step 0's wholesale `scripts/` restore, not with the
 * package — so this audit named a file the tree has never contained and DIED
 * on it, `ENOENT` out of `read()`, before comparing anything. A gate that
 * throws is a gate nobody can read, and it blocked Step 3a, whose whole
 * hazard is deleting `core/arena.c` from some of these lists and not others.
 *
 * An absence is registered, not tolerated: it must be named here with its
 * owner, it is printed on every run, and the pass line reports how many lists
 * were actually compared, so this can never read as "all five agree".
 */
const REGISTERED_ABSENT = {
    "packages/swift-markdown-core/Package.release.swift": {
        why: "the release manifest postdates the 1.0.3 baseline; the tree has no Swift release archive yet",
        owner: "15C — release plumbing pointing at live paths (§4.1)"
    }
};

const absent = [];
/** A follower's two buckets, or `null` if the file is registered as not here yet. */
function follower(relative, buckets) {
    if (fs.existsSync(path.join(root, relative))) {
        return buckets();
    }
    const registered = REGISTERED_ABSENT[relative];
    if (registered === undefined) {
        throw new Error(`${relative}: this list does not exist and is not registered as absent`);
    }
    absent.push({ relative, ...registered });
    return null;
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

const ANDROID_CMAKE = "packages/kotlin-markdown-core/android-runtime/src/main/cpp/CMakeLists.txt";
const ES_BUILD = "packages/es-markdown-core/scripts/build.mjs";
const SWIFT_RELEASE = "packages/swift-markdown-core/Package.release.swift";

const declared = {
    "Package.swift": follower("Package.swift", () => ({
        core: names("Package.swift", "core", swiftPattern("core")),
        extensions: names("Package.swift", "extensions", swiftPattern("extensions"))
    })),
    [SWIFT_RELEASE]: follower(SWIFT_RELEASE, () => ({
        core: names(SWIFT_RELEASE, "core", swiftPattern("core")),
        extensions: names(SWIFT_RELEASE, "extensions", swiftPattern("extensions"))
    })),
    [ANDROID_CMAKE]: follower(ANDROID_CMAKE, () => ({
        core: names(ANDROID_CMAKE, "core", androidPattern("CORE")),
        extensions: names(ANDROID_CMAKE, "extensions", androidPattern("EXTENSIONS"))
    })),
    [ES_BUILD]: follower(ES_BUILD, () => ({
        core: esArray(ES_BUILD, "core"),
        extensions: esArray(ES_BUILD, "extensions")
    }))
};

const followers = Object.fromEntries(Object.entries(declared).filter(([, buckets]) => buckets !== null));

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

const compared = 1 + Object.keys(followers).length;
const declaredCount = 1 + Object.keys(declared).length;

for (const entry of absent) {
    console.log(`  not here yet: ${entry.relative} — ${entry.why}. Owner: ${entry.owner}.`);
}

if (failed) {
    console.error(`\nSource-list audit failed: ${String(compared)} hand-written lists do not agree.`);
    process.exit(1);
}
console.log(
    `Source-list audit passed: ${String(expected.size)} sources, ` +
        `${String(compared)} of ${String(declaredCount)} lists in agreement` +
        (absent.length === 0 ? "." : `, ${String(absent.length)} registered absent.`)
);
