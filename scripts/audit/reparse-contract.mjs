/** THE RE-PARSE CONTRACT (docs/plans/2026-09-29-incremental-parsing.md, 5.4),
 * held on the sources. A restart reopens a checkpoint's spine and a rejoin
 * splices the old parse back, so every element states, in its descriptor,
 * what a restart needs to know:
 *
 * - E1: source bytes are read through the input index. The storage the index
 *   reads (the pieces, the session text, its cursor and the copies of lines
 *   that cross pieces) is touched by the index, the text tree and the session
 *   that owns the text, and by nothing else.
 * - E2: a closed block is written through `markdown_core_parser_write_closed`,
 *   and a dialect that writes one says through `writes_below` which closed
 *   blocks a later line may write.
 * - E3: a container kind that reopens carries its line state in the word
 *   `carry_save` returns; `carry_restore` and `carry_save` belong to such a
 *   descriptor.
 * - E4: a container's finalize is a fold of its children's summaries:
 *   `fold_child` and `fold_apply` come together, on a descriptor whose kinds
 *   reopen, and its finish step runs the fold (`markdown_core_parser_fold`).
 *
 * `descriptors` are the element inventory's ({symbol, file, source, body});
 * `sources` are every library source ({file, source}), with paths relative to
 * the package. Returns the failures. */
const INDEX_STORE = /->\s*(input_piece|input_pieces|input_text|input_cursor|input_copies)\b/;
const INDEX_OWNERS = new Set(["core/blocks.c", "core/parser.h", "core/text_tree.c", "core/text_tree.h"]);
const TEXT_TREE = /\bmarkdown_core_text_tree_\w+\s*\(/;
const TEXT_OWNERS = new Set([...INDEX_OWNERS, "elements/session.c"]);
const WRITE_CLOSED = /\bmarkdown_core_parser_write_closed\s*\(/;
const FOLD = /\bmarkdown_core_parser_fold\s*\(/;

const strip = (source) => source.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const has = (body, hook) => new RegExp(`\\.${hook}\\s*=`).test(body);

export function auditReparseContract(descriptors, sources) {
    const failures = [];
    let writes = 0;
    for (const { file, source } of sources) {
        const code = strip(source);
        if (!INDEX_OWNERS.has(file) && INDEX_STORE.test(code)) {
            failures.push(`${file}: reads the input index's storage (E1)`);
        }
        if (!TEXT_OWNERS.has(file) && TEXT_TREE.test(code)) {
            failures.push(`${file}: reads the session text outside the input index (E1)`);
        }
        if (WRITE_CLOSED.test(code) && file.startsWith("elements/")) writes++;
    }
    const writable = descriptors.filter(({ body }) => has(body, "writes_below")).length;
    if (writes && !writable) {
        failures.push("an element writes a closed block, and no descriptor says which a later line may write (E2)");
    }
    if (writable && !writes) {
        failures.push("a descriptor says a later line may write a closed block, and no element writes one (E2)");
    }
    let reopeners = 0;
    let folders = 0;
    for (const { symbol, file, source, body } of descriptors) {
        const reopens = has(body, "reopen_kinds");
        reopeners += reopens;
        if (reopens && !has(body, "carry_save")) {
            failures.push(`${file}: ${symbol} reopens a kind and declares no carry_save (E3)`);
        }
        for (const hook of ["carry_save", "carry_restore", "relation_ends"]) {
            if (has(body, hook) && !reopens) {
                failures.push(`${file}: ${symbol} declares ${hook} for kinds that do not reopen (E3)`);
            }
        }
        if (has(body, "carry_restore") && !has(body, "carry_save")) {
            failures.push(`${file}: ${symbol} restores a carried word it never saves (E3)`);
        }
        const folds = has(body, "fold_child");
        folders += folds;
        if (folds !== has(body, "fold_apply")) {
            failures.push(`${file}: ${symbol} declares one of fold_child and fold_apply (E4)`);
        }
        if (folds && !reopens) {
            failures.push(`${file}: ${symbol} folds kinds that do not reopen (E4)`);
        }
        if (folds && !(has(body, "finish_step") && FOLD.test(strip(source)))) {
            failures.push(`${file}: ${symbol} folds, and its finish step does not run the fold (E4)`);
        }
    }
    if (!reopeners || !folders || !writes) {
        failures.push("found no element that reopens, folds or writes; the audit is not reaching the sources");
    }
    return failures;
}
