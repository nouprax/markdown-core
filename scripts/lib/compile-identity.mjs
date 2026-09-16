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
        /* Union across the units: every option any measured object received. */
        flags: [...new Set(distinct.flatMap((line) => line.split(" ")))].join(" "),
        digest: crypto
            .createHash("sha256")
            .update(units.map((unit) => `${unit.file}\u0000${unit.flags}`).join("\n"))
            .digest("hex")
    };
}
