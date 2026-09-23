/** Identical local problems, not AST projections or claims about entire parses. */
import { Buffer } from "node:buffer";
import { isDeepStrictEqual } from "node:util";
import { effortReview } from "./pair-effort.mjs";
import { productionProofs, productionWorkload } from "./pair-productions.mjs";
import { spanLanguage } from "./corpus-pairs.mjs";

export const boundaryModel = "canonical-parser-boundaries-v1";
export const boundaryOperations = Object.freeze(["copy", "trim", "unescape", "whitespace", "code", "closer", "owners"]);

export function encodeParents(parents) {
    const bytes = Buffer.alloc(parents.length * 4);
    parents.forEach((parent, i) => bytes.writeUInt32LE(parent, i * 4));
    return bytes;
}

export function ownershipTopology(tree) {
    const parents = [];
    const stack = [[tree, 0xffffffff]];
    while (stack.length) {
        const [node, parent] = stack.pop();
        const index = parents.length;
        parents.push(parent);
        for (let i = node.children.length - 1; i >= 0; i--) stack.push([node.children[i], index]);
    }
    return encodeParents(parents);
}

export function ownershipFixtures() {
    const fixtures = [];
    for (const scale of [1, 2]) {
        const values = Array.from({ length: 16 * scale }, (_, i) => String(i + 1).padStart(6, "0"));
        const trees = [...productionProofs].map(([proof, p]) => [
            proof,
            productionWorkload(
                proof,
                values.map((n) => p.dialect.replaceAll("{n:6}", n)).join(""),
                values.map((n) => p.common.replaceAll("{n:6}", n)).join("")
            )
        ]);
        trees.push(["insertion-strong-v1", spanLanguage("probe ++a ++b++ c++\n\n".repeat(16 * scale), "++")]);
        for (const [proof, tree] of trees)
            fixtures.push({
                id: `owners-${proof}-x${scale}`,
                operation: "owners",
                proof,
                scale,
                input: ownershipTopology(tree),
                start: 0,
                ticks: 1,
                measure: true
            });
    }
    return fixtures;
}

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
    if (operation === "owners") {
        if (
            input.length < 4 ||
            input.length % 4 ||
            input.length >= Math.floor(0x7fffffff / 10) ||
            input.readUInt32LE() !== 0xffffffff
        )
            throw new Error("invalid ownership stream");
        for (let i = 1; i < input.length / 4; i++)
            if (input.readUInt32LE(i * 4) >= i) throw new Error("ownership parent is not an earlier owner");
    }
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
    if (operation === "owners") {
        const parents = Array.from({ length: input.length / 4 }, (_, i) => input.readUInt32LE(i * 4));
        const children = parents.map(() => []);
        parents.forEach((parent, i) => {
            if (i) children[parent].push(i);
        });
        const links = parents.map((parent, i) => [
            parent,
            0xffffffff,
            0xffffffff,
            children[i][0] ?? 0xffffffff,
            children[i].at(-1) ?? 0xffffffff
        ]);
        for (const siblings of children)
            siblings.forEach((id, at) => {
                links[id][1] = siblings[at - 1] ?? 0xffffffff;
                links[id][2] = siblings[at + 1] ?? 0xffffffff;
            });
        bytes = [...encodeParents(links.flat())];
    } else if (operation === "trim") {
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
    const result = ownershipFixtures();
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
        if (operation === "owners") continue;
        add(`${operation}-empty`, operation, "");
        for (const size of [1024, 2048]) {
            const unit = operation === "closer" ? "a``` b`` c " : " a\\*b\t\n c ";
            const body = unit.repeat(Math.ceil(size / unit.length)).slice(0, size);
            add(`${operation}-mixed-${size}`, operation, body, { ticks: 2, measure: true });
        }
    }
    add("owners-singleton", "owners", encodeParents([0xffffffff]));
    for (const count of [256, 512])
        for (const shape of ["deep", "wide"]) {
            add(
                `owners-${shape}-${count}`,
                "owners",
                encodeParents(
                    Array.from({ length: count }, (_, i) => (i === 0 ? 0xffffffff : shape === "deep" ? i - 1 : 0))
                ),
                { measure: true }
            );
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
                ownershipCases: [1, 2].map((scale) => `owners-${pair.contract.proof}-x${scale}`),
                coverage: "No end-to-end cost fraction claimed; boundary inputs must independently pass admission.",
                residual: effortReview(pair).reason
            }
        }));
}
