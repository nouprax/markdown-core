/* A per-document stage guard. Each stage of each document stays within 2% of
 * its base count, so a cheaper stage, another document or a median cannot hide
 * a regression. The 2% allowance is a review budget, not timing noise or a
 * claim about elapsed performance; both revisions run in one job. This gate
 * sees only work inside source_to_buffer and buffer_to_ast, and no report
 * figure sees parser creation: work moved there passes it without making
 * parsing cheaper. Such a move is caught in review, not here. */
export const STAGE_IR_LIMIT = 1.02;
/* The measured parse stages, in order; the budget holds for every one. */
export const STAGES = Object.freeze(["source_to_buffer", "buffer_to_ast"]);

export function stageBudget(current, baseline) {
    const indexed = new Map(baseline.map((row) => [row.case, row]));
    if (baseline.length === 0 || indexed.size !== baseline.length || current.length !== baseline.length) {
        throw new Error("stage budget requires equal, unique workloads");
    }
    const seen = new Set();
    return current.flatMap((row) => {
        const previous = indexed.get(row.case);
        if (seen.has(row.case) || !previous || row.sha256 !== previous.sha256 || row.bytes !== previous.bytes) {
            throw new Error(`stage budget input mismatch: ${row.case}`);
        }
        seen.add(row.case);
        return STAGES.map((stage) => {
            const read = (entry) => entry.engines["markdown-core"].stages[stage]?.cost.Ir;
            const before = read(previous);
            const after = read(row);
            if (![before, after].every((value) => Number.isSafeInteger(value) && value > 0)) {
                throw new Error(`stage budget requires positive instruction counts: ${row.case} ${stage}`);
            }
            return {
                case: row.case,
                stage,
                before,
                after,
                ratio: after / before,
                passed: after <= before * STAGE_IR_LIMIT
            };
        });
    });
}
