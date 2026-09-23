/** Parse-effort adjudication is separate from the structural theorem.
 * No existing pair has a full-parser optimal-effort certificate. These records
 * identify missing obligations, not proofs that such a certificate is impossible.
 */
import { renamingReview, validateRenamingProofs } from "./effort-renaming.mjs";

export const effortModel = "parser-effort-v1";
const reviews = new Map();
function review(ids, category, reason) {
    for (const id of ids) {
        if (reviews.has(id)) throw new Error(`duplicate effort review: ${id}`);
        const alphabetRenaming = renamingReview(id);
        reviews.set(
            id,
            Object.freeze({
                model: effortModel,
                status: "unproved",
                scope: "full-parser-optimum",
                category,
                reason,
                ...(alphabetRenaming ? { alphabetRenaming } : {})
            })
        );
    }
}
const production = (names, category, reason) =>
    review(
        names.map((name) => `${name}-v2`),
        category,
        reason
    );

review(
    ["insertion-strong-v1"],
    "marker-correspondence",
    "The recursive grammar needs an equivalent-problem proof covering marker realization, contextual decisions, disambiguation and failure. Neither AST-owner correspondence nor a global byte permutation is a required grammar-equivalence criterion."
);
production(
    ["run-insertion", "run-mark"],
    "marker-correspondence",
    "Equivalent grammar rewrites must preserve marker recognition, flanking/residue decisions and failure under the declared cost model. The optional alphabet-map obstruction does not decide grammar-level optimal effort."
);
production(
    ["run-strike", "run-sub"],
    "marker-correspondence",
    "Equivalent grammar rewrites must account for run widths, body restrictions, fence context and failure. The optional alphabet-map obstruction is not a grammar-equivalence admission condition."
);
production(
    ["run-super"],
    "marker-correspondence",
    "Equivalent grammar rewrites must account for empty scripts, raw-space invalidation, escapes and inline-footnote precedence. Different derivation/AST shapes alone do not decide optimal parsing effort."
);
production(
    ["opaque-comment", "opaque-formula", "opaque-display"],
    "lexical-obligations",
    "Opaque output leaves do not equate closer recognition, delimiter-run lengths, escape/flanking decisions, or code-span whitespace normalization."
);
production(
    ["leaf-comment", "leaf-formula", "leaf-fence", "leaf-promotion"],
    "phase-and-normalization",
    "Different fence/promotion decisions and physical-line extents remain; the reversible terminal-LF field encoding does not prove equal trimming, normalization or recognition cost."
);
production(
    ["task-value"],
    "marker-correspondence",
    "The singleton marker-value correspondence preserves ownership, but does not map the complete task-marker language, field representation and contextual decisions with equal cost."
);
production(
    ["record-span", "class-span"],
    "field-grammar",
    "Attribute/class members and link destinations/titles have different delimiters, decoding, termination and field-construction obligations despite the reversible field encoding."
);
production(
    [
        "cross-link",
        "cross-embed",
        "cross-link-absent",
        "cross-embed-absent",
        "cross-link-empty",
        "cross-embed-empty",
        "cross-anchor",
        "cross-local"
    ],
    "field-grammar",
    "Cross path/anchor/label fields are encoded as URL/title fields. Recognizing and normalizing those grammars, bracket context and null/empty distinctions have no cost-preserving simulation."
);
production(
    ["cite-author", "cite-suppress", "cite-normal"],
    "field-grammar",
    "Citation-key/mode recognition and an explicit URI autolink have different validation and derived-text obligations; fixed key encoding establishes values, not equal effort."
);
production(
    ["inline-directive", "empty-directive", "leaf-directive"],
    "owner-grammar",
    "DirectiveLabel ownership is preserved, but nested image/link destinations, labels and link activation do not correspond to directive recognition step for step."
);
production(
    ["anonymous-container", "named-container"],
    "container-grammar",
    "Fenced containers have open/close, name/attribute and line-boundary decisions; a quote has prefix continuation. Equal owned paragraphs do not remove those obligations."
);
production(
    ["alpha-list", "upper-list", "roman-list", "upper-roman-list", "default-list", "enclosed-default-list"],
    "marker-value-conversion",
    "Alphabetic, Roman or implicit markers encode values differently from decimal markers, with distinct continuation/disambiguation rules. Two fixed items do not prove equal conversion or recognition cost."
);
production(
    ["decimal-list"],
    "identical-input-control",
    "The two measured source strings are identical, so A/B is a control. Core and the reference still support different complete parser contracts; identity of this input does not prove equal full-parser optima."
);
production(
    ["loose-definition"],
    "owner-grammar",
    "Term/body recognition and caption precedence differ from list/quote continuation. The extra reference owner repairs the output graph, not the recognition obligations."
);
production(
    ["grid-cell"],
    "geometry-grammar",
    "Grid borders, scalar columns, rectangle/span validation and cell source mapping differ from list/quote prefixes. Equal output owners do not make these recognition problems isomorphic."
);
production(
    ["simple-matrix"],
    "geometry-grammar",
    "Positional column discovery and pipe delimiter/escape recognition differ even with equal cell values and alignment fields."
);
production(
    ["specimen-graph"],
    "binding-grammar",
    "Fresh fixed labels align definition/call edges, but no simulation covers full definition recognition, symbol lookup, hoisting, ordinals, resolution failures and source coordinates."
);

export function effortReview(pair) {
    const proof = pair.contract?.proof;
    if (!proof)
        return {
            model: effortModel,
            status: "unproved",
            scope: "full-parser-optimum",
            category: "structural-contract-missing",
            reason: "A reviewed historical workload or pending mapping has no parser-effort certificate."
        };
    const result = reviews.get(proof);
    if (!result) throw new Error(`${pair.case}: missing parse-effort review for ${proof}`);
    return result;
}

export function validateEffortReviews(proofs) {
    const expected = new Set(proofs);
    if (expected.size !== reviews.size || [...expected].some((id) => !reviews.has(id)))
        throw new Error("parse-effort reviews must cover exactly the registered structural proofs");
    validateRenamingProofs(proofs);
}
