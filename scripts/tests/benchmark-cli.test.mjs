import assert from "node:assert/strict";
import test from "node:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const driver = path.join(root, "scripts/benchmark.mjs");

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
        const { status, message } = refuse(["--case", "common-atx-1-paired-common"], {
            [name]: "@/tmp/profile.rsp"
        });
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
            const { status, message } = refuse(["--case", "common-atx-1-paired-common"], { [name]: value });
            assert.notEqual(status, 0);
            assert.match(message, new RegExp(`${name} names the response file @/tmp/`, "u"));
        });
    }

    /* The linker and the assembler read @FILE too, and their spelling puts it
     * after a comma rather than at the start of the argument. */
    for (const [shape, value] of [
        ["-Wl,", "-Wl,@/tmp/link.rsp"],
        ["-Wa,", "-Wa,@/tmp/as.rsp"],
        ["a later comma piece", "-Wl,-x,@/tmp/link.rsp"],
        ["-Xlinker", "-Xlinker @/tmp/link.rsp"]
    ]) {
        test(`a response file reached through ${shape} in ${name} is refused`, () => {
            const { status, message } = refuse(["--case", "common-atx-1-paired-common"], { [name]: value });
            assert.notEqual(status, 0);
            assert.match(message, new RegExp(`${name} names the response file @/tmp/`, "u"));
        });
    }

    /* dyld's placeholders are the one @ in linker arguments that names no
     * file: `-Wl,-rpath,@loader_path/../lib` is an ordinary macOS link flag,
     * and a check that refused it would be wrong rather than strict. */
    for (const placeholder of ["@loader_path", "@executable_path", "@rpath"]) {
        test(`${placeholder} in ${name} is not mistaken for a response file`, () => {
            const { message } = refuse(["--case", "common-atx-1-paired-common", "--out", "build/benchmark/nested"], {
                [name]: `-Wl,-rpath,${placeholder}/../lib`
            });
            assert.match(message, /overlaps the profile build tree/u);
        });
    }

    test(`${name} that the shell cannot split is refused`, () => {
        const { status, message } = refuse(["--case", "common-atx-1-paired-common"], {
            [name]: "'@/tmp/unbalanced"
        });
        assert.notEqual(status, 0);
        assert.match(message, new RegExp(`${name} cannot be split into arguments`, "u"));
    });

    /* An @ inside a path is not a response file -- `llvm@17` is an ordinary
     * Homebrew include directory, and only a leading @ names a file. The
     * overlap refusal that follows shows the flags got past this check. */
    test(`an @ inside a path in ${name} is not mistaken for one`, () => {
        const { message } = refuse(["--case", "common-atx-1-paired-common", "--out", "build/benchmark/nested"], {
            [name]: "-I/opt/homebrew/opt/llvm@17/include"
        });
        assert.match(message, /overlaps the profile build tree/u);
    });

    test(`ordinary flags in ${name} are not mistaken for one`, () => {
        const { message } = refuse(["--case", "common-atx-1-paired-common", "--out", "build/benchmark/nested"], {
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

test("selecting a grammar input includes its counterpart and complete boundary hosts", () => {
    const out = fs.mkdtempSync(path.join(os.tmpdir(), "grammar-selection-"));
    try {
        const documents = (name) => {
            const result = spawnSync(
                process.execPath,
                [driver, "--corpus-only", "--quiet", "--out", out, "--case", name],
                { encoding: "utf8", cwd: root }
            );
            assert.equal(result.status, 0, result.stderr);
            return JSON.parse(fs.readFileSync(path.join(out, "units.json"), "utf8"));
        };
        const paired = ["insertion-strong-paired-common", "insertion-strong-paired-dialect"];
        for (const name of paired) assert.deepEqual(Object.keys(documents(name)).sort(), paired);
        const boundary = ["boundary-common", "boundary-dialect", "host-common", "host-dialect"].map(
            (part) => `grid-cell-${part}`
        );
        for (const name of boundary) {
            const units = documents(name);
            assert.deepEqual(Object.keys(units).sort(), boundary);
            assert.equal(new Set(Object.values(units)).size, 1);
        }
        assert.match(refuse(["--case", "block-heading"]).message, /no corpus case is named/u);
        assert.match(refuse(["--grammar-corpus", "yes"]).message, /unknown argument/u);
        assert.match(refuse(["--scale", "2"]).message, /unknown argument/u);
    } finally {
        fs.rmSync(out, { recursive: true, force: true });
    }
});

test("baseline selection requires an exact revision and a real measurement", () => {
    assert.match(refuse(["--baseline-ref", "main"]).message, /full commit SHA/u);
    assert.match(refuse(["--baseline-ref", "a".repeat(40), "--corpus-only"]).message, /requires measurement/u);
});
