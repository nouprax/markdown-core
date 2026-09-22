/* A per-document source-stage guard. A cheaper AST stage or a median cannot
 * hide a source regression. The 2% allowance is a review budget, not timing
 * noise or a claim about elapsed performance; both revisions run in one job.
 * This gate sees only work inside source_to_buffer. Moving work into setup
 * can pass it while increasing complete parse cost: review parsePathIr and
 * outsideStagesIr alongside this budget, including tiny inputs. */
export const SOURCE_IR_LIMIT = 1.02;

export function sourceBudget(current, baseline) {
    const key = (row) => `${row.case}\0${row.scale}`;
    const indexed = new Map(baseline.map((row) => [key(row), row]));
    if (baseline.length === 0 || indexed.size !== baseline.length || current.length !== baseline.length) {
        throw new Error("source budget requires equal, unique workloads");
    }
    const seen = new Set();
    return current.map((row) => {
        const previous = indexed.get(key(row));
        if (seen.has(key(row)) || !previous || row.sha256 !== previous.sha256 || row.bytes !== previous.bytes) {
            throw new Error(`source budget input mismatch: ${row.case} x${row.scale}`);
        }
        seen.add(key(row));
        const read = (entry) => entry.engines["markdown-core"].stages.source_to_buffer.cost.Ir;
        const before = read(previous);
        const after = read(row);
        if (![before, after].every((value) => Number.isSafeInteger(value) && value > 0)) {
            throw new Error(`source budget requires positive instruction counts: ${row.case}`);
        }
        return {
            case: row.case,
            scale: row.scale,
            before,
            after,
            ratio: after / before,
            passed: after <= before * SOURCE_IR_LIMIT
        };
    });
}
