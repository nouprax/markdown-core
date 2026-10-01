import assert from "node:assert/strict";
import test from "node:test";
import { auditReparseContract } from "../reparse-contract.mjs";

const descriptor = (symbol, body, source = "") => ({ symbol, file: `elements/${symbol}.c`, source, body });
const list = descriptor(
    "LIST",
    ".reopen_kinds = KINDS, .carry_save = save, .fold_child = child, .fold_apply = apply, .finish_step = step,",
    "static void step(void) { markdown_core_parser_fold(parser, node); }"
);
const para = descriptor("PARAGRAPH", ".writes_below = writes_below,");
const writer = { file: "elements/block_identifier.c", source: "markdown_core_parser_write_closed(parser, owner);" };
const index = { file: "core/blocks.c", source: "parser->input_pieces[0]; markdown_core_text_tree_byte(text, 0);" };

test("the dialect as it stands keeps the contract", () => {
    assert.deepEqual(auditReparseContract([list, para], [index, writer]), []);
});

test("E1: only the index, the text tree and the session read the source's storage", () => {
    for (const source of ["parser->input_piece + 1", "parser->input_copies", "markdown_core_text_tree_byte(t, 0)"]) {
        const failures = auditReparseContract([list, para], [index, writer, { file: "elements/x.c", source }]);
        assert.match(failures.join("\n"), /\(E1\)/, source);
    }
    const session = { file: "elements/session.c", source: "markdown_core_text_tree_edit(text, 0)" };
    assert.deepEqual(auditReparseContract([list, para], [index, writer, session]), []);
    /* A mention in a comment reads nothing. */
    const comment = { file: "elements/y.c", source: "/* parser->input_pieces */" };
    assert.deepEqual(auditReparseContract([list, para], [index, writer, comment]), []);
});

test("E2: a dialect that writes closed blocks says which a later line may write", () => {
    assert.match(auditReparseContract([list], [index, writer]).join("\n"), /no descriptor says/);
    assert.match(auditReparseContract([list, para], [index]).join("\n"), /no element writes one/);
});

test("E3 and E4: carried words and folds belong to kinds that reopen, and the fold runs", () => {
    const cases = [
        [descriptor("A", ".reopen_kinds = K,"), /declares no carry_save/],
        [descriptor("B", ".carry_save = s,"), /carry_save for kinds that do not reopen/],
        [descriptor("C", ".reopen_kinds = K, .carry_save = s, .relation_ends = r, .carry_restore = c,"), null],
        [descriptor("D", ".reopen_kinds = K, .carry_save = s, .fold_child = f,"), /one of fold_child and fold_apply/],
        [
            descriptor("E", ".reopen_kinds = K, .carry_save = s, .fold_child = f, .fold_apply = a,"),
            /does not run the fold/
        ],
        [
            descriptor("F", ".fold_child = f, .fold_apply = a, .finish_step = t,", "markdown_core_parser_fold(p, n);"),
            /folds kinds that do not reopen/
        ]
    ];
    for (const [extra, expected] of cases) {
        const failures = auditReparseContract([list, para, extra], [index, writer]).join("\n");
        if (expected) assert.match(failures, expected, extra.symbol);
        else assert.equal(failures, "", extra.symbol);
    }
});

test("an audit that reaches no reopening, folding or writing element fails", () => {
    assert.match(auditReparseContract([], []).join("\n"), /not reaching the sources/);
});
