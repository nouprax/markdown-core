/** Identical local problems, not AST projections or claims about entire parses. */
import { Buffer } from "node:buffer";
import { isDeepStrictEqual } from "node:util";
import { effortReview } from "./pair-effort.mjs";

export const boundaryModel = "canonical-byte-boundaries-v1";
export const boundaryOperations = Object.freeze(["copy", "trim", "unescape", "whitespace", "code", "closer"]);

/** Lossless SOURCE partition. Recombining transformed chunks is a distinct,
 * uncertified operation; this never claims a compositional parsing theorem. */
export function splitWhitespaceBoundary(input) {
    const segments = [];
    let start = 0;
    for (let at = 0; at <= input.length; at++) {
        if (at !== input.length && input[at] !== 11 && input[at] !== 12) continue;
        if (at > start) segments.push({ kind: "matched", start, end: at, input: input.subarray(start, at) });
        if (at < input.length)
            segments.push({ kind: "residual", start: at, end: at + 1, input: input.subarray(at, at + 1) });
        start = at + 1;
    }
    return segments;
}

export function boundarySplits() {
    const input = Buffer.from(" a ".repeat(128) + "\v" + " b\n".repeat(128) + "\f" + " c\t".repeat(128));
    return ["trim", "whitespace"].map((operation) => ({
        id: `${operation}-vt-ff`,
        operation,
        input,
        segments: splitWhitespaceBoundary(input),
        residual: "VT/FF classification and recomposition; no whole-operation ratio is admitted."
    }));
}
const space = (b) => b === 32 || b === 9 || b === 10 || b === 13;
const punctuation = (b) =>
    (b >= 33 && b <= 47) || (b >= 58 && b <= 64) || (b >= 91 && b <= 96) || (b >= 123 && b <= 126);

export function admitBoundary({ operation, input, start = 0, ticks = 1 }) {
    if (
        !boundaryOperations.includes(operation) ||
        !Buffer.isBuffer(input) ||
        input.length >= 0x3fffffff ||
        !Number.isInteger(start) ||
        start < 0 ||
        start > input.length ||
        !Number.isInteger(ticks) ||
        ticks < 1 ||
        ticks > 80
    )
        throw new Error("outside boundary domain");
    if (["trim", "whitespace"].includes(operation) && (input.includes(11) || input.includes(12)))
        throw new Error("VT/FF classification is a semantic boundary");
    if (["code", "closer"].includes(operation) && (input.includes(0) || input.includes(13)))
        throw new Error("boundary requires NUL/CR-normalized input");
    if (operation === "closer" && start > 0 && input[start - 1] === 96 && input[start] === 96)
        throw new Error("cursor splits a delimiter run");
}

/** Executable specification, independent of either production implementation. */
export function boundaryOracle(fixture) {
    admitBoundary(fixture);
    const { operation, input, start = 0, ticks = 1 } = fixture;
    let bytes = [...input];
    const output = {
        source: input.toString("hex"),
        hex: "",
        position: 0,
        result: 0,
        scanned: 0,
        cache: Array(81).fill(0)
    };
    if (operation === "trim") {
        const first = bytes.findIndex((b) => !space(b));
        const last = bytes.findLastIndex((b) => !space(b));
        bytes = first < 0 ? [] : bytes.slice(first, last + 1);
    } else if (operation === "unescape") {
        const groups = input.toString("latin1").match(/\\[!-/:-@[-`{-~]|[\s\S]/gu) ?? [];
        bytes = [...Buffer.from(groups.map((s) => (s.length === 2 ? s[1] : s)).join(""), "latin1")];
    } else if (operation === "whitespace") {
        bytes = [...Buffer.from(input.toString("latin1").replace(/[\t\n\r ]+/gu, " "), "latin1")];
    } else if (operation === "code") {
        bytes = bytes.map((b) => (b === 10 ? 32 : b));
        if (bytes.some((b) => b !== 32) && bytes[0] === 32 && bytes.at(-1) === 32) bytes = bytes.slice(1, -1);
    } else if (operation === "closer") {
        output.position = input.length;
        output.scanned = 1;
        for (const match of input.toString("latin1").slice(start).matchAll(/`+/gu)) {
            const at = start + match.index;
            const length = match[0].length;
            if (length <= 80) output.cache[length] = at;
            if (length === ticks) {
                output.position = output.result = at + length;
                output.scanned = 0;
                break;
            }
        }
    }
    output.hex = Buffer.from(bytes).toString("hex");
    return output;
}

export function verifyBoundaryReceipt(fixture, stdout, count) {
    const rows = stdout
        .trim()
        .split("\n")
        .map((line) => JSON.parse(line));
    const expected = boundaryOracle(fixture);
    if (rows.length !== count || rows.some((row) => !isDeepStrictEqual(row, expected)))
        throw new Error(`${fixture.id}: boundary receipt violates complete output/state contract`);
}

export function boundaryFixtures() {
    const result = [];
    const add = (id, operation, input, options = {}) =>
        result.push({ id, operation, input: Buffer.from(input), start: 0, ticks: 1, ...options });
    for (const split of boundarySplits())
        for (const segment of split.segments) {
            if (segment.kind === "matched")
                add(`${split.id}-${segment.start}`, split.operation, segment.input, {
                    measure: true,
                    split: split.id,
                    extent: [segment.start, segment.end]
                });
        }
    for (const operation of boundaryOperations) {
        add(`${operation}-empty`, operation, "");
        for (const size of [1024, 2048]) {
            const unit = operation === "closer" ? "a``` b`` c " : " a\\*b\t\n c ";
            const body = unit.repeat(Math.ceil(size / unit.length)).slice(0, size);
            add(`${operation}-mixed-${size}`, operation, body, { ticks: 2, measure: true });
        }
    }
    const alphabet = Buffer.from(Array.from({ length: 256 }, (_, n) => n));
    for (const operation of ["copy", "trim", "unescape", "whitespace"]) {
        add(
            `${operation}-all-bytes`,
            operation,
            ["trim", "whitespace"].includes(operation) ? alphabet.filter((b) => b !== 11 && b !== 12) : alphabet
        );
        add(`${operation}-spaces`, operation, "\t\n\r   ");
        add(`${operation}-interior-nul`, operation, Buffer.from([32, 0, 32]));
    }
    add("unescape-every-byte", "unescape", Buffer.concat([...alphabet].map((b) => Buffer.from([92, b, 124]))));
    add("unescape-final-backslash", "unescape", "\\a\\\\\\!\\");
    for (const input of [" ", "\n", " \n ", "\n\n", " a ", "  a  ", "\t", " \t ", "é\n字", "`\n`"])
        add(`code-edge-${result.length}`, "code", input);
    for (const ticks of [1, 2, 79, 80]) {
        for (const run of [1, 2, 79, 80, 81, 1000, 1001]) {
            add(`closer-${ticks}-${run}`, "closer", "prefix " + "`".repeat(run) + " tail", { start: 7, ticks });
        }
        add(`closer-miss-${ticks}`, "closer", "a".repeat(2048), { ticks, measure: true });
    }
    // Exhaustive small words exercise adjacent escapes and all normalization states.
    let words = [""];
    for (let depth = 0; depth < 3; depth++) {
        words = words.flatMap((word) => [" ", "\n", "a", "\\", "*"].map((c) => word + c));
        for (const word of words)
            for (const operation of ["code", "unescape", "whitespace"])
                add(`${operation}-word-${result.length}`, operation, word);
    }
    return result;
}

/** Every old proof gets an explicit residual, never an inherited certificate.
 * Local kernel availability is not a claim that a given parse executed it. */
export function boundaryPairAudit(pairs) {
    return pairs
        .filter((pair) => pair.contract?.proof)
        .map((pair) => ({
            case: pair.case,
            proof: pair.contract.proof,
            full: effortReview(pair),
            local: {
                status: "separate-contracted-problems",
                certificates: [...boundaryOperations],
                coverage: "No end-to-end cost fraction claimed; boundary inputs must independently pass admission.",
                residual: effortReview(pair).reason
            }
        }));
}

// Export predicates for the exhaustive independent specification checks.
export const byteClasses = { space, punctuation };
