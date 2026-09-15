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
function refuse(args) {
    const result = spawnSync(process.execPath, [driver, ...args], { encoding: "utf8", cwd: root });
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

test("an unknown flag is refused rather than ignored", () => {
    const { status, message } = refuse(["--jobs", "4"]);
    assert.notEqual(status, 0);
    assert.match(message, /unknown argument: --jobs/u);
});
