import assert from "node:assert/strict";
import test from "node:test";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const driver = path.join(root, "scripts/benchmark-stages.mjs");

/**
 * The argument checks refuse before anything is built, so these run in
 * milliseconds and never touch a compiler.
 */
function refuse(args, environment = {}) {
    const result = spawnSync(process.execPath, [driver, ...args], {
        encoding: "utf8",
        cwd: root,
        env: { ...process.env, ...environment }
    });
    return { status: result.status, message: `${result.stderr ?? ""}`.split("\n")[0] };
}

/**
 * --scale decides how many sizes of every case are measured, so a value that
 * is quietly read as a different number measures a different experiment than
 * the caller asked for, and the report does not say which.
 *
 * Number.parseInt reads leading digits and discards the rest, so the shape has
 * to be checked before the parse; and digits alone are not enough, because a
 * long enough run of them is Infinity and a large enough value is rounded to
 * one the caller did not name.
 */
const MALFORMED = [
    ["a trailing unit", "2x"],
    ["a fraction", "1.5"],
    ["exponent notation", "1e3"],
    ["leading space", " 3"],
    ["hexadecimal", "0x10"],
    ["not a number at all", "abc"],
    ["309 digits, which parses to Infinity", "9".repeat(309)],
    ["one past the safe integers, which is rounded down", "9007199254740993"],
    ["ten to the twentieth, exact as text but not countable", "1".padEnd(21, "0")]
];

for (const [what, value] of MALFORMED) {
    test(`--scale refuses ${what}`, () => {
        const { status, message } = refuse(["--scale", value]);
        assert.notEqual(status, 0, `--scale ${value} was accepted`);
        assert.match(message, /--scale must be a positive integer/u);
    });
}

test("an empty --scale is a missing value rather than a malformed one", () => {
    const { status, message } = refuse(["--scale", ""]);
    assert.notEqual(status, 0);
    assert.match(message, /--scale needs a value/u);
});

test("--scale refuses zero", () => {
    const { status, message } = refuse(["--scale", "0"]);
    assert.notEqual(status, 0);
    assert.match(message, /--scale must be a positive integer/u);
});

test("--scale takes digits naming a number held exactly, leading zeros and all", () => {
    for (const value of ["1", "2", "007", "64"]) {
        const { message } = refuse(["--scale", value, "--case", "no-such-case-exists"]);
        /* The run still refuses -- there is no such case, and on a machine
         * without the pinned oracle it does not get that far. Which refusal it
         * is depends on the machine, so the assertion is the one thing that
         * does not: the scale was not what was rejected. */
        assert.doesNotMatch(message, /--scale must be a positive integer/u, `--scale ${value} was refused`);
    }
});

/**
 * A mistyped case name is answerable from the manifest alone, so it is
 * answered before the oracle, the toolchain or either build is looked at. A
 * checkout without the pinned cmark installed otherwise reports that instead,
 * which is a true statement about the machine and not the answer to what the
 * caller got wrong.
 */
test("an unknown case is refused before anything is installed or built", () => {
    const { status, message } = refuse(["--case", "no-such-case-exists"]);
    assert.notEqual(status, 0);
    assert.match(message, /no corpus case is named no-such-case-exists/u);
    assert.match(message, /the manifest has /u);
});

/**
 * A response file keeps its flags in a file the report never reads, so two
 * runs whose identity tables match can have compiled different objects. The
 * contents are what matters and the path is all that is recorded, so the
 * arrangement is refused rather than recorded as if it said something.
 */
for (const name of ["CFLAGS", "LDFLAGS"]) {
    test(`a response file in ${name} is refused`, () => {
        const { status, message } = refuse(["--case", "block-heading"], { [name]: "@/tmp/profile.rsp" });
        assert.notEqual(status, 0);
        assert.match(message, new RegExp(`${name} names the response file @/tmp/profile.rsp`, "u"));
    });

    /* The shell decides where a flag ends, not whitespace: quoted and escaped
     * spellings are one argument to the compiler and several words to a naive
     * split, and the compiler reads the argument. */
    for (const [shape, value] of [
        ["single-quoted, with spaces", "'@/tmp/flags with space.rsp'"],
        ["double-quoted", '"@/tmp/profile.rsp"'],
        ["escaped spaces", "@/tmp/flags\\ with\\ space.rsp"]
    ]) {
        test(`a response file in ${name} is refused when ${shape}`, () => {
            const { status, message } = refuse(["--case", "block-heading"], { [name]: value });
            assert.notEqual(status, 0);
            assert.match(message, new RegExp(`${name} names the response file @/tmp/`, "u"));
        });
    }

    test(`${name} that the shell cannot split is refused`, () => {
        const { status, message } = refuse(["--case", "block-heading"], { [name]: "'@/tmp/unbalanced" });
        assert.notEqual(status, 0);
        assert.match(message, new RegExp(`${name} cannot be split into arguments`, "u"));
    });

    /* An @ inside a path is not a response file -- `llvm@17` is an ordinary
     * Homebrew include directory, and only a leading @ names a file. The
     * overlap refusal that follows shows the flags got past this check. */
    test(`an @ inside a path in ${name} is not mistaken for one`, () => {
        const { message } = refuse(["--case", "block-heading", "--out", "build/benchmark/nested"], {
            [name]: "-I/opt/homebrew/opt/llvm@17/include"
        });
        assert.match(message, /overlaps the profile build tree/u);
    });

    test(`ordinary flags in ${name} are not mistaken for one`, () => {
        const { message } = refuse(["--case", "block-heading", "--out", "build/benchmark/nested"], {
            [name]: "-DNDEBUG -O2"
        });
        assert.match(message, /overlaps the profile build tree/u);
    });
}

test("an unknown flag is refused rather than ignored", () => {
    const { status, message } = refuse(["--jobs", "4"]);
    assert.notEqual(status, 0);
    assert.match(message, /unknown argument: --jobs/u);
});
