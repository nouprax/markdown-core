import assert from "node:assert/strict";
import test from "node:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { ENGINES, assertSetupUncounted, verifySetup } from "../run.mjs";

const root = path.resolve(fileURLToPath(new URL("../../..", import.meta.url)));
const driver = path.join(root, "scripts/benchmark/run.mjs");

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

/**
 * A profile shaped like the Core runner's under `--separate-callers=1`, with
 * setup counted: each node named `callee'caller`, the dialect built and sealed
 * inside the parse entry.
 */
function coreProfile({ counted = true, extra = [] } = {}) {
    const node = (callee, caller) => `${callee}'${caller}`;
    const entry = node("bench_parse_document", "main");
    const facade = node("markdown_core_parse_document_with_setup", "bench_parse_document");
    const parserNew = node("S_parser_new", "markdown_core_parse_document_with_setup");
    const seal = node("markdown_core_dialect_seal", "S_parser_new");
    const self = new Map([
        [entry, [10]],
        [facade, [20]],
        [parserNew, [30]],
        [node("S_parse_source.part.0", "markdown_core_parse_document_with_setup"), [300]],
        [node("S_finish_parse", "markdown_core_parse_document_with_setup"), [200]],
        ...(counted
            ? [
                  [node("markdown_core_core_elements", "markdown_core_parse_document_with_setup"), [4]],
                  [node("markdown_core_dialect_builder_init", "markdown_core_parse_document_with_setup"), [6]],
                  [node("markdown_core_dialect_builder_dispose", "markdown_core_parse_document_with_setup"), [17]],
                  [node("markdown_core_dialect_measure", "S_parser_new"), [40]],
                  [seal, [50]],
                  [node("memset", "markdown_core_dialect_seal"), [100]]
              ]
            : [])
    ]);
    const edge = (caller, callee, ir) => [`${caller}\0${callee}`, { caller, callee, calls: 1, cost: [ir] }];
    const edges = new Map([
        edge("main", entry, 1),
        edge(entry, facade, 1),
        edge(facade, parserNew, 1),
        edge(facade, node("S_parse_source.part.0", "markdown_core_parse_document_with_setup"), 300),
        edge(facade, node("S_finish_parse", "markdown_core_parse_document_with_setup"), 200),
        ...(counted
            ? [
                  edge(facade, node("markdown_core_core_elements", "markdown_core_parse_document_with_setup"), 4),
                  edge(
                      facade,
                      node("markdown_core_dialect_builder_init", "markdown_core_parse_document_with_setup"),
                      6
                  ),
                  edge(
                      facade,
                      node("markdown_core_dialect_builder_dispose", "markdown_core_parse_document_with_setup"),
                      17
                  ),
                  edge(parserNew, node("markdown_core_dialect_measure", "S_parser_new"), 40),
                  edge(parserNew, seal, 150),
                  edge(seal, node("memset", "markdown_core_dialect_seal"), 100)
              ]
            : []),
        ...extra.map(([caller, callee]) => edge(caller, callee, 1))
    ]);
    return { events: ["Ir"], self, edges };
}

test("the Core setup declaration holds against a profile that counted setup", () => {
    verifySetup(coreProfile(), ENGINES["markdown-core"].setup);
});

test("a setup declaration that is missing, outside the entry, shared or nested fails", () => {
    const setup = ENGINES["markdown-core"].setup;
    assert.throws(() => verifySetup(coreProfile({ counted: false }), setup), /no setup call/u);
    assert.throws(
        () => verifySetup(coreProfile({ extra: [["main", "free'main"]] }), [{ caller: "main", callee: "free" }]),
        /outside bench_parse_document/u
    );
    // A toggle excludes every entry into the function, so a parse-time caller would lose parse work.
    assert.throws(
        () =>
            verifySetup(
                coreProfile({
                    extra: [
                        [
                            "S_finish_parse'markdown_core_parse_document_with_setup",
                            "markdown_core_dialect_measure'S_finish_parse"
                        ]
                    ]
                }),
                setup
            ),
        /also entered from S_finish_parse/u
    );
    // A nested toggle would switch counting back on inside the outer call.
    const nested = [
        ["markdown_core_dialect_seal'S_parser_new", "helper'markdown_core_dialect_seal"],
        ["helper'markdown_core_dialect_seal", "inner_setup'helper"]
    ];
    assert.throws(
        () =>
            verifySetup(coreProfile({ extra: nested }), [
                { caller: "S_parser_new", callee: "markdown_core_dialect_seal" },
                { caller: "helper", callee: "inner_setup" }
            ]),
        /setup call markdown_core_dialect_seal reaches setup call inner_setup/u
    );
});

test("a measured profile must carry no cost inside a setup call", () => {
    const setup = ENGINES["markdown-core"].setup;
    assertSetupUncounted(coreProfile({ counted: false }), setup);
    assert.throws(() => assertSetupUncounted(coreProfile(), setup), /setup function markdown_core_\w+ was counted/u);
});

test("every engine declares its grammar setup, and Core's names are functions it defines", () => {
    assert.deepEqual(Object.keys(ENGINES).toSorted(), ["cmark", "cmark-gfm", "markdown-core"]);
    for (const definition of Object.values(ENGINES)) assert.ok(Array.isArray(definition.setup));
    assert.deepEqual(ENGINES.cmark.setup, []);
    const sources = ["core/blocks.c", "core/dialect.c", "elements/core-elements.c"]
        .map((file) => fs.readFileSync(path.join(root, "packages/markdown-core", file), "utf8"))
        .join("\n");
    for (const { caller, callee } of ENGINES["markdown-core"].setup) {
        for (const name of [caller, callee])
            assert.match(sources, new RegExp(`\\b${name}\\([^;]*\\)\\s*\\{`, "u"), name);
    }
});
