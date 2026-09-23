// One interpretation of stage cohorts for the artifact and PR comment.
import { pairRatios } from "./corpus-pairs.mjs";
import { splitWithCases } from "./corpus-splits.mjs";

const fail = (message) => {
    throw new Error(message);
};
export function stageIr(engines, engine) {
    if (!engines[engine]) return null;
    return ["source_to_buffer", "buffer_to_ast"].reduce((sum, stage) => {
        const value = engines[engine].stages[stage].ir;
        if (!Number.isSafeInteger(value) || value < 0 || !Number.isSafeInteger(sum + value)) {
            throw new Error("Invalid stage instruction count");
        }
        return sum + value;
    }, 0);
}

export function stageComparisons(report) {
    /* A ratio is only a comparison where both engines did the same job. */
    /* The pairing, and which cases exist only to be the other half of one. An
     * isomorph is a CommonMark document written to match a dialect document,
     * not a construct anyone writes, so it belongs in the pair table and not in
     * the CommonMark median it would otherwise move. */
    const declarations = report.pairs;
    const paired = new Map(declarations.map((declaration) => [declaration.case, declaration]));
    const isIsomorph = new Set(declarations.map((declaration) => declaration.isomorph));
    const bySubstitution = new Set(declarations.filter((pair) => pair.substitution).map((pair) => pair.case));
    /* And which cases exist only to be the `with` half of a split. Such a
     * document is a host that already has a comparison, carrying a remainder
     * that has none, so it is neither a comparison nor a bound: its number is
     * the difference against its `without`, in "The remainder inside its
     * hosts" below. Ranking it would put a document written to carry an
     * unpairable production into the bound table as if that were its
     * measurement, and into the median of a group it was never part of. */
    const isSplitWith = splitWithCases(report);
    /* What each ranked case IS to the report, decided once. The groups, the
     * bound prose and the hot-path table all used to test the same flags in
     * their own order, and the order is the meaning: a paired case is a pair
     * whatever its `gfm` flag says, a split's `with` half is in no group, and
     * a twin whose dialect half was not measured is in no group either. */
    const roleOf = (item) => {
        if (item.isomorph)
            return item.isomorph.structural ? "pair" : item.isomorph.contract.review ? "reviewed" : "candidate";
        if (paired.has(item.case)) return "unranked";
        if (item.boundary) return "boundary-base";
        if (isSplitWith.has(item.case)) return "split-with";
        if (item.gfm) return item.carries.length ? "unranked" : isIsomorph.has(item.case) ? "twin" : "gfm";
        if (item.dialect === "commonmark" && !item.carries.length) {
            return isIsomorph.has(item.case) ? "twin" : "commonmark";
        }
        return "bound";
    };

    const atScaleOne = new Map(report.cases.filter((item) => item.scale === 1).map((item) => [item.case, item]));
    const ranked = report.cases
        .filter((item) => item.scale === 1 && item.engines["markdown-core"])
        .map((item) => {
            const core = stageIr(item.engines, "markdown-core");
            const cmarkIr = stageIr(item.engines, "cmark");
            const gfmIr = stageIr(item.engines, "cmark-gfm");
            /* Structural correspondence permits a diagnostic quotient, not an
             * equal-effort claim. Recognition and full contracts may differ. */
            const declaration = paired.get(item.case);
            const twin = declaration ? atScaleOne.get(declaration.isomorph) : null;
            /* The isomorph's OWN reference, not always cmark. A dialect
             * construct can pair with a GFM production -- a task marker with a
             * GFM task list item, a specimen with a GFM footnote definition --
             * and use the engine that implements that reference production.
             * Reading cmark there would divide by an engine
             * that parsed the paired document as ordinary prose. */
            const twinReference = twin?.gfm ? "cmark-gfm" : "cmark";
            const twinCore = twin ? stageIr(twin.engines, "markdown-core") : null;
            const twinCmark = twin ? stageIr(twin.engines, twinReference) : null;
            if (twin && twin.units !== item.units) {
                fail(
                    `${item.case} and ${declaration.isomorph} are paired but carry ${item.units} and ` +
                        `${twin.units} of the construct. A pair compares two spellings of one thing only ` +
                        `while both documents hold the same number of it`
                );
            }
            if (twin && bySubstitution.has(item.case) && twin.bytes !== item.bytes) {
                /* The substitution is character for character, so the two
                 * documents are the same length and the corpus repeats each of
                 * them the same number of times. Different totals mean the pair
                 * is no longer measuring one workload twice, and comparing the
                 * sums would divide one document's cost by another's. */
                fail(
                    `${item.case} and ${declaration.isomorph} are paired but were measured at ` +
                        `${item.bytes}/${twin.bytes} bytes over ${item.units}/${twin.units} copies`
                );
            }
            const comparison = twin
                ? pairRatios(declaration, {
                      dialect: core,
                      common: twinCore,
                      reference: twinCmark,
                      carries: twin.carries
                  })
                : null;
            return {
                ...item,
                coreIr: core,
                cmarkRatio: cmarkIr ? core / cmarkIr : null,
                gfmRatio: gfmIr ? core / gfmIr : null,
                /* Only where the other half was actually measured: a
                 * `--case`-filtered run that named one side of a pair has no
                 * comparison to report, and falls back to the bound rather than
                 * printing a pair row of dashes. */
                isomorph: twin
                    ? {
                          case: declaration.isomorph,
                          contract: declaration.contract,
                          ...comparison,
                          /* How the pair was established, because it decides
                           * which invariant held it: equal bytes under a marker
                           * substitution, or an equal count of declarations in
                           * two spellings of different length. */
                          by: declaration.substitution ? "substitution" : declaration.counts ? "count" : "domain",
                          reference: twinReference,
                          /* Its own bytes, not this case's: a logical isomorph
                           * is a different length by construction, so dividing
                           * its cost by this document's size would be reading
                           * one document's Ir over another document's bytes. */
                          bytes: twin.bytes,
                          units: twin.units,
                          coreIr: twinCore,
                          cmarkIr: twinCmark,
                          /* Unmatched reference fields suppress A/B and B/R.
                           * A/R remains arithmetic and gains no effort proof
                           * from cancellation of the shared denominator. */
                          contaminates: twin.carries,
                          /* Cross-syntax total stage quotient inside Core. */
                          grammar: comparison.grammar,
                          /* Same-input implementation quotient on B. */
                          shape: comparison.shape
                      }
                    : null,
                // Descriptive quotient only; no theoretical-optimum assertion.
                comparisonRatio: declaration
                    ? (comparison?.quotient ?? null)
                    : item.carries.length
                      ? null
                      : item.gfm && gfmIr
                        ? core / gfmIr
                        : item.dialect === "commonmark" && cmarkIr
                          ? core / cmarkIr
                          : null
            };
        })
        .sort(
            (left, right) =>
                (right.comparisonRatio ?? right.cmarkRatio ?? 0) - (left.comparisonRatio ?? left.cmarkRatio ?? 0)
        );

    return { ranked, roleOf, atScaleOne };
}

export function sameInputGroups({ ranked, roleOf }) {
    return [
        ["CommonMark", "cmark", ranked.filter((item) => roleOf(item) === "commonmark")],
        ["GFM extensions", "cmark-gfm", ranked.filter((item) => roleOf(item) === "gfm")]
    ];
}

export function median(values) {
    if (!values.length) return null;
    const sorted = [...values].sort((a, b) => a - b);
    const middle = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}
