/**
 * Pairing contracts, independent of instruction counts and parser internals.
 * A sample substitution or a node census is evidence about a workload, not a
 * proof about a language. Only a registered, executable domain proof enables
 * an equivalent-work comparison. See the benchmark isomorphism contract.
 */
import { createHash } from "node:crypto";

import { productionProofs, productionTree, productionWorkload } from "./pair-productions.mjs";
import { pairReview } from "./pair-review.mjs";

const SPAN_PROOF = "insertion-strong-v1";

/** Interpretation is part of measurement identity even when corpus bytes stay unchanged. */
export function pairingIdentity(pairs, proofText, checkerSource) {
    return createHash("sha256").update(JSON.stringify({ pairs, proofText, checkerSource })).digest("hex");
}

export function equalProofTrees(left, right) {
    const pending = [[left, right]];
    while (pending.length) {
        const [a, b] = pending.pop();
        if (a.kind !== b.kind || a.literal !== b.literal || a.children.length !== b.children.length) return false;
        a.children.forEach((child, index) => pending.push([child, b.children[index]]));
    }
    return true;
}

export function provenPair(pair) {
    const contract = pair.contract;
    if (!contract || Object.keys(contract).length !== 1) throw new Error(`${pair.case}: one pairing contract required`);
    if (typeof contract.pending === "string" && contract.pending.trim()) return false;
    if (contract.review) {
        pairReview(pair);
        return false;
    }
    if (contract.proof === SPAN_PROOF || productionProofs.has(contract.proof)) return true;
    throw new Error(`${pair.case}: unknown or incomplete pairing proof`);
}

export function validatePairs(manifest) {
    if ("isomorphs" in manifest || "logicalIsomorphs" in manifest) throw new Error("use the single pairs registry");
    if (!Array.isArray(manifest.pairs)) throw new Error("pairs must be an array");
    const cases = new Map(manifest.cases.map((entry) => [entry.name, entry]));
    const seen = new Set();
    for (const pair of manifest.pairs) {
        for (const name of [pair.case, pair.isomorph]) {
            if (!cases.has(name) || seen.has(name)) throw new Error(`${name}: missing or repeated pair half`);
            seen.add(name);
        }
        if (pair.contract?.review) {
            const review = pairReview(pair);
            for (const proof of review.proofs) {
                if (!manifest.pairs.some((other) => other.contract?.proof === proof))
                    throw new Error(`${pair.case}: missing reconstructed proof ${proof}`);
            }
            if (review.baseline) {
                const base = cases.get(review.baseline);
                if (
                    base?.boundary?.match !== pair.case ||
                    base.boundary.cut !== review.id ||
                    base.dialect !== "extended"
                )
                    throw new Error(`${pair.case}: missing or invalid boundary baseline`);
            }
        }
        const dialect = cases.get(pair.case);
        const reference = cases.get(pair.isomorph);
        if (dialect.dialect !== "extended" || (reference.dialect !== "commonmark" && !reference.gfm)) {
            throw new Error(`${pair.case}: pair must name a dialect input and a reference-language input`);
        }
        if (
            provenPair(pair) &&
            (reference.carries?.length ||
                dialect.carries?.length ||
                Boolean(reference.gfm) !== Boolean(productionProofs.get(pair.contract.proof)?.gfm))
        ) {
            throw new Error(`${pair.case}: proof requires its declared reference and no unmatched fields`);
        }
    }
    return manifest.pairs;
}

/** Algebraic quotients remain available for candidates, without a same-job claim. */
export function pairRatios(pair, { dialect, common, reference, carries = [] }) {
    const proven = provenPair(pair);
    return {
        proven,
        grammar: common && !carries.length ? dialect / common : null,
        shape: common && reference && !carries.length ? common / reference : null,
        quotient: reference ? dialect / reference : null,
        sameJob: proven && reference ? dialect / reference : null
    };
}

const node = (kind, children = [], literal) =>
    literal === undefined ? { kind, children } : { kind, literal, children };

/**
 * Independent recognizer for the proof's language, NOT for Markdown generally:
 * P = "probe " M (" " M)* "\n"; D = P ("\n" P)* "\n"?
 * M = marker B marker; B = W (" " (W | M))*; W = [a-z]+
 * B must end in W. This keeps every delimiter run isolated and unambiguous.
 * The explicit stack accepts arbitrary nesting without a host recursion limit.
 */
export function spanLanguage(source, marker) {
    if (marker !== "++" && marker !== "**") throw new Error("unknown span spelling");
    const root = node("Document");
    let at = 0;
    const fail = () => {
        throw new Error(`outside ${SPAN_PROOF} at byte ${at}`);
    };
    const word = () => {
        const start = at;
        while (at < source.length && source[at] >= "a" && source[at] <= "z") at++;
        if (start === at) fail();
        return source.slice(start, at);
    };
    while (at < source.length) {
        if (!source.startsWith("probe ", at)) fail();
        at += 6;
        const paragraph = node("Paragraph", [node("Text", [], "probe ")]);
        root.children.push(paragraph);
        for (;;) {
            if (!source.startsWith(marker, at)) fail();
            at += 2;
            const span = node("Span", [node("Text", [], word())]);
            paragraph.children.push(span);
            const stack = [span];
            while (stack.length) {
                const current = stack.at(-1);
                if (source.startsWith(marker, at)) {
                    if (current.children.at(-1).kind !== "Text" || current.children.at(-1).literal.endsWith(" "))
                        fail();
                    at += 2;
                    stack.pop();
                    continue;
                }
                if (source[at++] !== " ") fail();
                const last = current.children.at(-1);
                if (last.kind === "Text") last.literal += " ";
                else current.children.push(node("Text", [], " "));
                if (source.startsWith(marker, at)) {
                    at += 2;
                    const inner = node("Span", [node("Text", [], word())]);
                    current.children.push(inner);
                    stack.push(inner);
                } else {
                    current.children.at(-1).literal += word();
                }
            }
            if (source[at] === "\n") {
                at++;
                if (source[at] === "\n") at++;
                else if (at !== source.length) fail();
                break;
            }
            if (source[at++] !== " ") fail();
            paragraph.children.push(node("Text", [], " "));
        }
    }
    if (!root.children.length) fail();
    return root;
}

/** Both recognition and inverse mapping are checked on the actual measured bytes. */
export function proofWorkload(pair, dialect, common) {
    if (!provenPair(pair)) throw new Error(`${pair.case}: pending proof`);
    if (productionProofs.has(pair.contract.proof)) return productionWorkload(pair.contract.proof, dialect, common);
    const left = spanLanguage(dialect, "++");
    const right = spanLanguage(common, "**");
    if (
        dialect.replaceAll("++", "**") !== common ||
        common.replaceAll("**", "++") !== dialect ||
        !equalProofTrees(left, right)
    )
        throw new Error(`${pair.case}: proof mapping does not commute`);
    return left;
}

/**
 * Check semantic output against the independent source derivation. Production
 * contracts use syntax-directed actions; the recursive span path below renames
 * only Insertion <-> Strong. Unrelated kinds and semantic fields
 * fail closed; order, nesting and literal content survive the projection.
 * Scope coordinates and child-count printer metadata are outside this proof.
 */
export function proofTree(pair, side, tree, expected, referenceHtml) {
    if (!provenPair(pair)) throw new Error(`${pair.case}: pending proof`);
    if (!["dialect", "common", "reference"].includes(side)) throw new Error("unknown proof side");
    if (productionProofs.has(pair.contract.proof))
        return productionTree(pair.contract.proof, side, tree, expected, referenceHtml);
    const span = side === "dialect" ? "Insertion" : "Strong";
    const out = node("Document");
    const stack = [[tree, out]];
    while (stack.length) {
        const [source, target] = stack.pop();
        if (!["Document", "Paragraph", "Text", span].includes(source.kind)) {
            throw new Error(`${pair.case}: unmapped ${side} kind ${source.kind}`);
        }
        target.kind = source.kind === span ? "Span" : source.kind;
        for (const [key, value] of Object.entries(source.fields)) {
            if (["scope", "sourcepos", "children"].includes(key)) continue;
            if (key === "anchor" && value === "null") continue;
            if (key === "attributes" && value === "{}") continue;
            if (key === "literal" && source.kind === "Text" && typeof value === "string") {
                target.literal = value;
                continue;
            }
            throw new Error(`${pair.case}: unmapped ${side} field ${key}`);
        }
        if (source.kind === "Text" && (target.literal === undefined || source.children.length)) {
            throw new Error(`${pair.case}: malformed text leaf`);
        }
        target.children = source.children.map(() => node(""));
        source.children.forEach((child, i) => stack.push([child, target.children[i]]));
    }
    return out;
}
