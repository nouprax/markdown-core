/**
 * WHAT A TRANSLATION UNIT WAS ACTUALLY COMPILED WITH.
 *
 * Shared by both benchmark drivers, and shared rather than copied for the
 * reason a second copy always is: the identity a report publishes has to be
 * the identity it checked, and two implementations of "the real compile line"
 * drift into two different claims that both read as true.
 */

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

/**
 * A build's cache variables are global, and a compile line is not built from
 * them alone: CMake adds directory-, target- and property-derived options that
 * live nowhere in the cache. `POSITION_INDEPENDENT_CODE` contributes `-fPIC`,
 * a target's own `target_compile_options` contribute whatever it asked for, and
 * each project sets its own language level and warning set. So two trees can
 * agree on every cache variable while their compile lines differ -- including
 * in options that change code generation.
 *
 * EVERY unit of the linked target, not the one holding the stage boundaries. A
 * stage's cost is inclusive, so it contains whatever the scanners and the
 * inline code did too, and CMake lets a single source carry its own options:
 * `elements/CMakeLists.txt` gives ten scanner sources `-Wno-unused-variable`
 * through `set_source_files_properties`, which is how Markdown Core's 59
 * objects come to have two distinct compile lines. Reading one file would see
 * one of them.
 *
 * The target has to be named because one source can be compiled several ways in
 * one tree. Markdown Core compiles `core/blocks.c` twice -- into the shared
 * library and into the static library the runner links -- so a lookup by file
 * alone would be a coin flip between two different compile lines.
 */
export function compiledFlags(root, buildDir, target, fail) {
    const database = path.join(buildDir, "compile_commands.json");
    if (!fs.existsSync(database)) {
        fail(`${path.relative(root, database)} was not generated, so the real compile line cannot be read`);
    }
    const object = `CMakeFiles/${target}.dir/`;
    const entries = JSON.parse(fs.readFileSync(database, "utf8")).filter((entry) =>
        (entry.output ?? "").includes(object)
    );
    if (!entries.length) fail(`${path.relative(root, database)} has no entries for target ${target}`);

    /* Include directories and the output and input paths are per-project by
     * construction and say nothing about code generation; everything else the
     * compiler was handed is kept, warnings included, because this is a record
     * of the build rather than a filter of it.
     *
     * The two paths are dropped by identity, using the database's own `file`
     * and `output`, rather than by how they are spelled. Dropping every token
     * that ends in .c or .o also swallowed the ARGUMENT of any option taking
     * one: `-imacros a.c` and `-imacros b.c` both normalised to `-imacros`,
     * and macros reach code generation. An option's argument is part of the
     * option; only the unit being compiled and the file it is written to are
     * per-project noise. */
    const normalize = (entry) => {
        const command = entry.command ?? (entry.arguments ?? []).join(" ");
        const tokens = command.match(/(?:[^\s"']+|"[^"]*"|'[^']*')+/gu) ?? [];
        const bare = (token) => token.replace(/^["']|["']$/gu, "");
        const input = path.resolve(entry.directory, entry.file);
        const output = entry.output ? path.resolve(entry.directory, bare(entry.output)) : null;
        const kept = [];
        for (let index = 1; index < tokens.length; index++) {
            const token = tokens[index];
            if (token === "-o" || token === "-I" || token === "-isystem") {
                index++;
                continue;
            }
            if (token.startsWith("-I") || token.startsWith("-isystem")) continue;
            const resolved = path.resolve(entry.directory, bare(token));
            if (resolved === input || (output !== null && resolved === output)) continue;
            kept.push(token);
        }
        return kept.join(" ");
    };

    /* Kept per unit and digested, so a per-source option anywhere in the engine
     * moves the identity. The distinct lines are what a reader is shown: one is
     * the ordinary case, and more than one says the engine is not compiled
     * uniformly, which is a fact about the measurement rather than an error. */
    const units = entries
        .map((entry) => ({
            file: path.relative(root, path.resolve(entry.directory, entry.file)),
            flags: normalize(entry)
        }))
        .sort((left, right) => left.file.localeCompare(right.file));
    const distinct = [...new Set(units.map((unit) => unit.flags))].sort();
    return {
        units: units.length,
        distinct,
        bySource: Object.fromEntries(units.map((unit) => [unit.file, unit.flags])),
        /* Union across the units: every option any measured object received. */
        flags: [...new Set(distinct.flatMap((line) => line.split(" ")))].join(" "),
        digest: crypto
            .createHash("sha256")
            .update(units.map((unit) => `${unit.file}\u0000${unit.flags}`).join("\n"))
            .digest("hex")
    };
}

/* Inventory is provenance, not a compiler option. Refactors may add, remove
 * or rename units while preserving the option sets. For surviving paths also
 * check each unit, so swapping existing option sets cannot hide a flag change.
 * Preserve option order: -O0 -O3 and -O3 -O0 do not mean the same thing. */
export function sameCompileOptions(left, right) {
    return (
        JSON.stringify(left.distinct) === JSON.stringify(right.distinct) &&
        Object.entries(left.bySource).every(([file, flags]) =>
            Object.hasOwn(right.bySource, file) ? right.bySource[file] === flags : true
        )
    );
}

/**
 * The flags a tree's compile and link lines actually carry.
 *
 * The preset's `CMAKE_C_FLAGS_RELEASE` is one of four cache variables that
 * reach a command line, and the other three come from the environment: CMake
 * initializes `CMAKE_C_FLAGS` from CFLAGS and puts it FIRST on every compile
 * line, and `CMAKE_EXE_LINKER_FLAGS` from LDFLAGS on every link line. The
 * link line is not a detail the instruction counts are indifferent to -- an
 * inherited `-static` moves the C library's code into the measured binary and
 * changes the stream every stage is counted from.
 *
 * So the description of a build is read out of its own cache rather than
 * taken from what the driver passed, and the whole set is read: a comparison
 * whose two trees agree on the compile flags and disagree on the link flags is
 * still a comparison between two binaries built differently.
 */
export function effectiveFlags(buildDir) {
    const cache = fs.readFileSync(path.join(buildDir, "CMakeCache.txt"), "utf8");
    const entry = (name) => new RegExp(`^${name}:[A-Z]+=(.*)$`, "mu").exec(cache)?.[1] ?? "";
    const join = (...names) => names.map(entry).join(" ").replace(/\s+/gu, " ").trim();
    return {
        compile: join("CMAKE_C_FLAGS", "CMAKE_C_FLAGS_RELEASE"),
        link: join("CMAKE_EXE_LINKER_FLAGS", "CMAKE_EXE_LINKER_FLAGS_RELEASE")
    };
}

/**
 * A BUILD TREE IS REUSED ONLY WHILE IT WAS BUILT BY WHAT THE REPORT NAMES.
 *
 * CMake reuses unchanged objects, and "unchanged" is about sources rather than
 * about the compiler: upgrade the compiler at the same path and a configure
 * reuses everything, so the report records the new banner and digest while some
 * or all of the measured binaries came out of the old one. Where a comparison
 * has two trees and only one of them is cleaned, the ratio is then between two
 * compilers.
 *
 * So each tree carries a stamp of what produced it, and a tree whose stamp does
 * not match is discarded rather than reused. The stamp's CONTENT belongs to the
 * driver -- each one knows what its own report claims -- and only the mechanism
 * is here, shared so the second benchmark does not grow a second version of it.
 */
const STAMP = "markdown-core-profile-stamp.txt";

export function discardTree(buildDir, stamp) {
    if (!fs.existsSync(buildDir)) return;
    const file = path.join(buildDir, STAMP);
    const current = fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "";
    if (current === stamp) return;
    fs.rmSync(buildDir, { recursive: true, force: true });
}

export function markTree(buildDir, stamp) {
    fs.writeFileSync(path.join(buildDir, STAMP), stamp);
}
