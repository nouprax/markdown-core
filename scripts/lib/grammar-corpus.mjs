/** Grammar-first corpus. Proofs are in benchmark-grammar-corpus.md.
 * No native AST topology participates in certificate admission.
 */
import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { productionProofs } from "./pair-productions.mjs";
import { pairReviews } from "./pair-review.mjs";
import { featureGrammars, featureValues, finiteLexicons } from "./grammar-features.mjs";
import { validateFeatureCoverage } from "./grammar-coverage.mjs";

export const grammarVersion = "grammar-corpus-v2";
export const hash = (value) => createHash("sha256").update(value).digest("hex");

// Normal forms are grammars, not shapes learned from native output.
export const normalForms = Object.freeze({
    word: "Word = [a-z]+ ;",
    "attribute-key": "AttributeKey = Word except 'id' and 'class' ; Word = [a-z]+ ;",
    positive9: "Positive9 = [1-9][0-9]{0,8} ;",
    phrase: "Phrase = Word (SP Word)* ; Word = [a-z]+ ;",
    inline: "Body = Word | Word SP (Atom SP)* Word ; Atom = Word | OPEN Body CLOSE ; Word = [a-z]+ ;",
    empty: "Empty = epsilon ;",
    unicode: "UnicodeWord = [a-zé字]+ ;",
    ordinal: "Ordinal = 1 | 2 | ... | 26 ;",
    state: "State = plain | closed | open ;"
});

const direct = {
    "insertion-strong": ["inline", "++", "**"],
    "run-insertion": ["phrase", "++", "**"],
    "run-mark": ["phrase", "==", "**"],
    "run-strike": ["phrase", "~~", "**"],
    "run-super": ["word", "^", "*"],
    "run-sub": ["word", "~", "*"],
    "opaque-comment": ["phrase", "%%", "``"],
    "opaque-formula": ["phrase", "$", "`"],
    "opaque-display": ["phrase", "$$", "``"]
};
const directFrames = new Set(["task-value", "decimal-list"]);
const additionalBoundaries = {
    "metadata-types": "properties",
    "metadata-literal": "properties",
    "multiline-matrix": "tables",
    "headless-multiline": "tables",
    "image-dimensions": "links-and-images"
};
export const historicalHosts = {
    "citation-affixes": {
        legacy: "citegroup"
    },
    "embed-dimensions": {
        legacy: "embed",
        reason: "Numeric image width/height has no field in the supplied CommonMark image counterpart; target and label remain locally paired."
    },
    "specimen-reset": {
        legacy: "specimenstart"
    },
    "trailing-caption": {
        legacy: "tcaption"
    },
    "headless-matrix": {
        legacy: "headless",
        reason: "Headless-row classification differs from the reference header rule; every cell value is retained in the local product."
    },
    "sparse-grid": {
        legacy: "sparsegrid",
        reason: "Spans, sparse rows, footer and empty-caption recognition remain residual; all six independent cell fields are retained."
    },
    "leading-caption": {
        legacy: "caption"
    },
    "mixed-definitions": {
        legacy: "deflist"
    }
};
const splitIds = new Set([
    ...Object.keys(additionalBoundaries),
    ...Object.keys(historicalHosts).filter((id) => historicalHosts[id].reason),
    "grid-cell",
    "simple-matrix",
    "metadataempty",
    "metadata"
]);
const isProduct = (id) => featureGrammars.has(id) || (!direct[id] && !directFrames.has(id) && !splitIds.has(id));

const boundaryReason = (id) => {
    if (historicalHosts[id]) return historicalHosts[id].reason;
    if (id === "image-dimensions")
        return "Positive bounded width/height recognition is absent from the pinned cmark image grammar; image label and destination are paired locally without attributing numeric validation to the reference.";
    if (id === "multiline-matrix" || id === "headless-multiline")
        return "Column-interval equality, physical-line segmentation and logical-row block parsing have no counterpart in the supplied GFM pipe-table grammar; all cell payloads are paired locally.";
    if (id === "simple-matrix" || id === "grid-cell")
        return "Geometry, column discovery, padding and cell source mapping remain residual; all cell contents enter the labelled local product.";
    if (id.startsWith("metadata"))
        return "Envelope detection, typed member decoding and first-valid/unknown-member policy remain residual; every value and the following body are retained.";
    throw new Error(`missing boundary disposition: ${id}`);
};

const ids = [
    ...new Set([
        ...Object.keys(additionalBoundaries),
        ...featureGrammars.keys(),
        ...Object.keys(historicalHosts),
        "insertion-strong",
        ...[...productionProofs.keys()].map((id) => id.replace(/-v2$/u, "")),
        "anchor",
        "metadataempty",
        "metadata",
        "callout"
    ])
];
export const grammarCertificates = ids.map((id) => {
    const full = direct[id] || directFrames.has(id) || isProduct(id);
    const legacy = [...pairReviews.values()]
        .filter((review) => review.proofs.includes(`${id}-v2`) || review.id === id)
        .map((review) => review.id);
    if (id === "insertion-strong") legacy.push("runs");
    if (historicalHosts[id]) legacy.push(historicalHosts[id].legacy);
    return Object.freeze({
        id,
        certificate: `${id}-grammar-v2`,
        scope: full ? "paired-document-grammar" : "boundary-grammar",
        grammar: direct[id]?.[0] ?? (directFrames.has(id) ? id : isProduct(id) ? "labelled-product" : "field-sequence"),
        legacy,
        structuralPredecessor:
            id === "insertion-strong" ? "insertion-strong-v1" : productionProofs.has(`${id}-v2`) ? `${id}-v2` : null,
        ...(featureGrammars.has(id)
            ? {
                  feature: featureGrammars.get(id).feature,
                  facets: featureGrammars.get(id).facets,
                  identity: featureGrammars.get(id).identity === true,
                  ...(featureGrammars.get(id).outputDifference
                      ? { outputDifference: featureGrammars.get(id).outputDifference }
                      : {})
              }
            : {}),
        ...(additionalBoundaries[id] ? { feature: additionalBoundaries[id] } : {}),
        ...(direct[id]
            ? { dialectMarker: full[1], commonMarker: full[2], residual: null }
            : full
              ? { residual: null }
              : { residual: boundaryReason(id) })
    });
});
const byId = new Map(grammarCertificates.map((entry) => [entry.id, entry]));

export function validateGrammarCertificates() {
    assert.equal(
        byId.size,
        ids.length,
        "structural domains, unpaired families and historical residual hosts all need a disposition"
    );
    for (const [id, residual] of Object.entries(historicalHosts)) {
        assert.equal(byId.get(id).scope, featureGrammars.has(id) ? "paired-document-grammar" : "boundary-grammar");
        assert.deepEqual(byId.get(id).legacy, [residual.legacy]);
    }
    assert.deepEqual(new Set(grammarCertificates.flatMap((entry) => entry.legacy)), new Set(pairReviews.keys()));
    for (const lexicons of Object.values(finiteLexicons)) {
        const length = Object.values(lexicons)[0].length;
        for (const tokens of Object.values(lexicons)) {
            assert.equal(tokens.length, length);
            assert.equal(new Set(tokens).size, length, "finite substitution must be injective");
        }
    }
    for (const entry of grammarCertificates) {
        if (entry.scope === "paired-document-grammar") {
            if (isProduct(entry.id)) {
                assert.deepEqual(grammarNormalForm(entry.id, "dialect"), grammarNormalForm(entry.id, "common"));
                if (entry.identity)
                    assert.deepEqual(
                        productGrammar(entry.id).dialect,
                        productGrammar(entry.id).common,
                        "identity certificate changed a production"
                    );
                continue;
            }
            if (directFrames.has(entry.id)) continue;
            assert.ok(normalForms[entry.grammar]);
            assert.equal(entry.dialectMarker.length, entry.commonMarker.length, "whole-pair terminal widths differ");
            for (const marker of [entry.dialectMarker, entry.commonMarker]) assert.match(marker, /^([^a-z\s])\1?$/u);
        } else assert.ok(entry.residual.length > 40);
    }
}

export function renderBody(body, marker = "**") {
    assert.ok(Array.isArray(body) && body.length > 0);
    assert.equal(typeof body[0], "string");
    assert.equal(typeof body.at(-1), "string");
    return body
        .map((part) => {
            if (typeof part === "string") {
                assert.match(part, /^[a-z]+$/u);
                return part;
            }
            return marker + renderBody(part, marker) + marker;
        })
        .join(" ");
}

/** Independent source recognizer for the recursive grammar, including EOF.
 * The generator's derivation is never accepted in place of decoding bytes.
 */
export function recognizeBody(source, marker = "**") {
    let at = 0;
    function body(nested) {
        const result = [];
        while (true) {
            if (source.startsWith(marker, at)) {
                assert.ok(result.length > 0, "body must begin with Word");
                at += marker.length;
                result.push(body(true));
                assert.ok(source.startsWith(marker, at), "missing closer");
                at += marker.length;
            } else {
                const start = at;
                while (at < source.length && source.charCodeAt(at) >= 97 && source.charCodeAt(at) <= 122) at++;
                assert.ok(at > start, `expected Word at ${at}`);
                result.push(source.slice(start, at));
            }
            if ((nested && source.startsWith(marker, at)) || at === source.length) break;
            assert.equal(source[at++], " ", "expected one space");
        }
        assert.equal(typeof result.at(-1), "string", "body must end with Word");
        return result;
    }
    const value = body(false);
    assert.equal(at, source.length, "trailing source");
    return value;
}

export function recognizeValue(grammar, source) {
    if (grammar === "inline") return recognizeBody(source);
    if (grammar === "word") {
        assert.match(source, /^[a-z]+$/u);
        return source;
    }
    if (grammar === "attribute-key") {
        assert.match(source, /^[a-z]+$/u);
        assert.ok(!["id", "class"].includes(source), "reserved attribute key outside record grammar");
        return source;
    }
    if (grammar === "positive9") {
        assert.match(source, /^[1-9][0-9]{0,8}$/u);
        return source;
    }
    if (grammar === "phrase") {
        assert.match(source, /^[a-z]+(?: [a-z]+)*$/u);
        return source.split(" ");
    }
    if (grammar === "empty") {
        assert.equal(source, "");
        return "";
    }
    if (grammar === "unicode") {
        assert.match(source, /^[a-zé字]+$/u);
        return source;
    }
    throw new Error(`unknown grammar ${grammar}`);
}

function word(index, width = 1) {
    let result = "";
    do {
        result += String.fromCharCode(97 + (index % 26));
        index = Math.floor(index / 26);
    } while (index);
    return result.repeat(width);
}
function bodyAt(seed, depth) {
    const first = word(seed * 17 + 3, 1 + (seed % 4));
    const last = word(seed * 29 + 13, 1 + (seed % 3));
    return depth
        ? [first, bodyAt(seed + 7, depth - 1), word(seed + 71), bodyAt(seed + 11, depth - 1), last]
        : [first, word(seed + 37, 1 + (seed % 5)), last];
}
function parameters(index) {
    // Independent fields and varying lengths; no repeated six-digit placeholder.
    return {
        key: word(index + 91),
        target: word(index * 11 + 7, 1 + (index % 3)),
        value: word(index * 13 + 43, 2 + (index % 7)),
        anchor: word(index * 19 + 37),
        body: index % 3 === 0 ? chainBody(index + 1, 2 ** (Math.floor(index / 3) % 6)) : bodyAt(index + 1, index % 4),
        tail: bodyAt(index + 23, (index + 1) % 3),
        words: [word(index * 7 + 1, 1 + (index % 11)), word(index * 31 + 17), word(index + 99, 1 + (index % 5))],
        mode: index % 3,
        start: 1 + (index % finiteLexicons.ordinal.decimal.length),
        reset: String(index % 3 === 2 ? 999999999 : index + 1)
    };
}
function chainBody(seed, depth) {
    let result = bodyAt(seed, 0);
    for (let i = 0; i < depth; i++) result = [word(seed + i), result, word(seed + i + 51)];
    return result;
}

/** A host is a lossless sequence of literal residuals and named grammar slots.
 * No regexp extraction can silently lose a delimiter or a second field.
 */
const slot = (name, grammar, source) => ({ name, grammar, source });
function host(parts) {
    const flat = parts.flat(Infinity);
    let source = "";
    const pieces = flat.map((part) => {
        const value = typeof part === "string" ? part : part.source;
        const start = Buffer.byteLength(source);
        source += value;
        return typeof part === "string"
            ? { kind: "residual", start, end: Buffer.byteLength(source), source: value }
            : { kind: "slot", start, end: Buffer.byteLength(source), ...part };
    });
    return { source, pieces };
}

/** Finite product grammars. A field is a nonterminal, never a sample value.
 * Inlining these productions gives the same labelled product on both sides.
 */
export function productGrammar(id) {
    assert.ok(isProduct(id), "not a product grammar");
    if (featureGrammars.has(id)) return featureGrammars.get(id);
    const f = (name, grammar = "word") => ({ name, grammar });
    const b = f("body", "inline"),
        t = f("tail", "inline"),
        k = {
            ...f("key", id === "record-span" ? "attribute-key" : "word"),
            ...(id === "anchor" ? { maxBytes: 1000 } : {})
        },
        v = f("value"),
        target = f("target"),
        anchor = f("anchor"),
        literal = f("literal", "phrase");
    const pair = (dialect, common) => ({ dialect: dialect.flat(), common: common.flat() });
    const frame = (parts) => ["probe ", ...parts, " end\n\n"];
    if (id.startsWith("leaf-")) {
        if (id === "leaf-directive") return pair(["::", k, "[", b, "]\n\n"], ["[", b, "](/", k, ")\n\n"]);
        const [open, close] =
            id === "leaf-comment"
                ? ["%%\n", "\n%%\n\n"]
                : id === "leaf-fence"
                  ? ["```formula\n", "\n```\n\n"]
                  : id === "leaf-promotion"
                    ? ["$$", "$$\n\n"]
                    : ["$$\n", "\n$$\n\n"];
        return pair([open, literal, close], ["```\n", literal, "\n```\n\n"]);
    }
    if (id === "record-span")
        return pair(frame(["[", b, "]{", k, '="', v, '"}']), frame(["[", b, "](/", k, ' "', v, '")']));
    if (id === "class-span") return pair(frame(["[]{.", v, "}"]), frame(["[](/", v, ")"]));
    if (id.startsWith("cross-")) {
        const embed = id.includes("embed"),
            absent = id.endsWith("absent") || id === "cross-anchor" || id === "cross-local",
            empty = id.endsWith("empty");
        const label = f("label", empty ? "empty" : "phrase");
        const fields = [
            ...(id === "cross-local" ? [] : [target]),
            ...(id === "cross-anchor" || id === "cross-local" ? ["#", anchor] : [])
        ];
        return pair(
            frame([embed ? "![[" : "[[", ...fields, ...(absent ? [] : ["|", label]), "]]"]),
            frame([embed ? "![](/" : "[](/", ...fields, ...(absent ? [] : [' "', label, '"']), ")"])
        );
    }
    if (id.startsWith("cite-"))
        return pair(
            frame([
                id === "cite-normal" ? "[@" : id === "cite-suppress" ? "-@" : "@",
                k,
                id === "cite-normal" ? "]" : ""
            ]),
            frame(["<https://", k, ">"])
        );
    if (id === "inline-directive" || id === "empty-directive") {
        const label = id === "empty-directive" ? f("body", "empty") : b;
        return pair(frame([":", k, "[", label, "]"]), frame(["[", label, "](/", k, ")"]));
    }
    if (id.includes("container"))
        return pair(
            [...(id === "named-container" ? [":::", k, "\n"] : ["::: {}\n"]), b, "\n\n", t, "\n:::\n\n"],
            [...(id === "named-container" ? ["> ", k, "\n>\n"] : []), "> ", b, "\n>\n> ", t, "\n\n"]
        );
    if (id === "loose-definition") return pair([literal, "\n\n: ", b, "\n\n"], ["- ", literal, "\n\n  ", b, "\n\n"]);
    if (id === "anchor") return pair(["> ", b, "\n\n#", k, "#\n\n"], ["> ", b, "\n\n[", k, "]: /target\n\n"]);
    throw new Error(`no product grammar: ${id}`);
}

const escapePattern = (s) => s.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
function fieldPattern(field) {
    const tokens = finiteLexicons[field.grammar]?.[field.encoding];
    if (tokens)
        return tokens
            .toSorted((a, b) => b.length - a.length)
            .map(escapePattern)
            .join("|");
    assert.ok(!finiteLexicons[field.grammar], "finite field needs a declared encoding");
    return ["word", "attribute-key"].includes(field.grammar)
        ? "[a-z]+"
        : field.grammar === "positive9"
          ? "[1-9][0-9]{0,8}"
          : field.grammar === "unicode"
            ? "[a-zé字]+"
            : field.grammar === "phrase"
              ? "[a-z]+(?: [a-z]+)*"
              : field.grammar === "inline"
                ? "[a-z* ]+"
                : "";
}
function decodeField(field, source) {
    if (field.maxBytes !== undefined) {
        assert.ok(Number.isSafeInteger(field.maxBytes) && field.maxBytes > 0);
        assert.ok(Buffer.byteLength(source) <= field.maxBytes, "field exceeds its grammar byte bound");
    }
    const tokens = finiteLexicons[field.grammar]?.[field.encoding];
    if (!tokens) return recognizeValue(field.grammar, source);
    const index = tokens.indexOf(source);
    assert.ok(index >= 0, "outside finite lexical language");
    return index + (field.grammar === "ordinal" ? 1 : 0);
}
function encodeField(field, value) {
    const tokens = finiteLexicons[field.grammar]?.[field.encoding];
    if (tokens) {
        assert.ok(Number.isSafeInteger(value));
        const token = tokens[value - (field.grammar === "ordinal" ? 1 : 0)];
        assert.notEqual(token, undefined, "outside finite normal form");
        return token;
    }
    return field.grammar === "inline" ? renderBody(value) : field.grammar === "phrase" ? value.join(" ") : value;
}
export function recognizeProduct(id, side, source) {
    assert.ok(["dialect", "common"].includes(side));
    const grammar = productGrammar(id)[side];
    const fields = grammar.filter((part) => typeof part !== "string");
    const pattern = new RegExp(
        grammar.map((part) => (typeof part === "string" ? escapePattern(part) : `(${fieldPattern(part)})`)).join(""),
        "uy"
    );
    const result = [];
    while (pattern.lastIndex < source.length) {
        const match = pattern.exec(source);
        assert.ok(match, `${id}/${side}: outside product grammar`);
        const bindings = new Map();
        for (const [i, field] of fields.entries()) {
            const value = { grammar: field.grammar, value: decodeField(field, match[i + 1]) };
            if (bindings.has(field.name))
                assert.deepEqual(bindings.get(field.name), value, `${id}: inconsistent binding ${field.name}`);
            else bindings.set(field.name, value);
        }
        result.push([...bindings].sort(([a], [b]) => a.localeCompare(b)));
    }
    assert.ok(result.length, "empty product document");
    return result;
}

const metadataPreamble = (id) =>
    id === "metadataempty"
        ? ""
        : 'name: "note"\ntime: 9007199254740993\nstate: true\ncomment:\nauthors: [one, 2]\nkeywords: []\n';

function metadataHost(id, p, side) {
    const b = slot("body", "inline", renderBody(p.body)),
        v = slot("value", "word", p.value),
        k = slot("key", "word", p.key),
        literal = slot("literal", "phrase", p.words.join(" "));
    const members =
        id === "metadata-types"
            ? [
                  'title: [true]\ntitle: "',
                  v,
                  '"\ntitle: ignored\nname: ',
                  k,
                  '\nsubtitle: ""\ntime: -1.50e+2\ndate: null\nauthors: [one, 2,]\nkeywords:\n- one\n- 2\nabstract: false\nstate: true\ncomment:\nunknown: ignored\n...\n'
              ]
            : id === "metadata-literal"
              ? ["abstract: |\n  ", literal, "\n\n  literal\ncomment: |\n  ", v, "\n    indented\nstate: ready\n"]
              : [metadataPreamble(id), ...(id === "metadataempty" ? ["unknown-", k, ": "] : ["date: "]), v, "\n"];
    const envelope = {
        opening: side === "dialect" ? (id === "metadata-types" ? "\uFEFF---\n" : "---\n") : "```\n",
        members,
        closing: side === "dialect" ? "---\n\n" : "```\n\n",
        content: [b, "\n\n"]
    };
    return {
        ...host([envelope.opening, members, envelope.closing, envelope.content]),
        envelope
    };
}

function renderHosts(id, p) {
    if (isProduct(id)) {
        const { dialect, common } = productGrammar(id);
        const values = {
            body: id === "empty-directive" ? "" : p.body,
            tail: p.tail,
            key: p.key,
            target: p.target,
            value: p.value,
            anchor: p.anchor,
            literal: p.words,
            label: id.endsWith("empty") ? "" : p.words,
            ...featureValues(p),
            ...p.finiteValues
        };
        return [dialect, common].map((grammar) =>
            host(
                grammar.map((part) =>
                    typeof part === "string"
                        ? part
                        : slot(part.name, part.grammar, encodeField(part, values[part.name]))
                )
            )
        );
    }
    const b = slot("body", "inline", renderBody(p.body));
    const t = slot("tail", "inline", renderBody(p.tail));
    const value = slot("value", "word", p.value);
    const key = slot("key", "word", p.key);
    const target = slot("target", "word", p.target);
    const anchor = slot("anchor", "word", p.anchor);
    const pair = (a, r) => [host(a), host(r)];
    const literal = slot("literal", "phrase", p.words.join(" "));
    if (id.startsWith("metadata")) return [metadataHost(id, p, "dialect"), metadataHost(id, p, "common")];
    if (id === "image-dimensions")
        return pair(
            [
                "probe ![",
                literal,
                `|${p.mode === 2 ? 2147483647 : p.start}${p.mode === 0 ? "" : `x${p.start + 1}`}](/`,
                target,
                ") end\n\n"
            ],
            ["probe ![", literal, "](/", target, ") end\n\n"]
        );
    if (id === "multiline-matrix" || id === "headless-multiline") {
        const last = slot("last", "word", p.words[0]),
            footer = slot("footer", "word", p.words[1]);
        const fields = [key, target, value, anchor, last, footer];
        const width = Math.max(...fields.map((f) => f.source.length)) + 3;
        const full = "-".repeat(width * 2 + 2) + "\n",
            segmented = "-".repeat(width) + "  " + "-".repeat(width) + "\n";
        const line = (a, b) => [a, " ".repeat(width + 2 - a.source.length), b, "\n"];
        return pair(
            [
                ...(id === "multiline-matrix"
                    ? [full, line(key, target), segmented]
                    : [segmented, line(key, target), "\n"]),
                line(value, anchor),
                line(value, anchor),
                "\n",
                line(last, footer),
                full,
                "\n"
            ],
            fields.flatMap((field) => ["> ", field, "\n\n"])
        );
    }
    if (id === "embed-dimensions")
        return pair(
            ["See ![[", target, "|", literal, `|${p.start}x${p.start + 1}]] here.\n\n`],
            ["See ![", literal, "](/", target, ") here.\n\n"]
        );
    if (id === "headless-matrix") {
        const width = Math.max(key.source.length, target.source.length, value.source.length, anchor.source.length) + 2;
        const rule = "-".repeat(width) + "  " + "-".repeat(width) + "\n";
        return pair(
            [
                rule,
                key,
                " ".repeat(width - key.source.length + 2),
                target,
                "\n",
                value,
                " ".repeat(width - value.source.length + 2),
                anchor,
                "\n",
                rule,
                "\n"
            ],
            ["| ", key, " | ", target, " |\n| --- | --- |\n| ", value, " | ", anchor, " |\n\n"]
        );
    }
    if (id === "sparse-grid") {
        const d = slot("last", "word", p.words[0]),
            footer = slot("footer", "word", p.words[1]);
        const width = Math.max(...[key, target, value, anchor, d, footer].map((x) => x.source.length)) + 2;
        const rule = (left, right) => `+${left.repeat(width)}+${right.repeat(width)}+\n`;
        const cell = (field) => [" ", field, " ".repeat(width - field.source.length - 1)];
        const spanning = (field) => ["| ", field, " ".repeat(2 * width - field.source.length), "|\n"];
        return pair(
            [
                rule("-", "-"),
                spanning(key),
                rule("=", "="),
                "|",
                cell(target),
                "|",
                cell(value),
                "|\n",
                rule("-", " "),
                "|",
                cell(anchor),
                "|",
                cell(d),
                "|\n",
                rule("-", "-"),
                rule(" ", " "),
                rule("=", "="),
                spanning(footer),
                rule("=", "="),
                "\nTable:\n\n---\n\n"
            ],
            [key, target, value, anchor, d, footer].flatMap((field) => ["> ", field, "\n\n"])
        );
    }
    if (id === "task-value") return pair(["- [~] ", b, "\n- [~] ", t, "\n\n"], ["- [x] ", b, "\n- [x] ", t, "\n\n"]);
    if (id === "decimal-list")
        return pair(
            [`${p.start}. `, b, `\n${p.start + 1}. `, t, "\n\n"],
            [`${p.start}. `, b, `\n${p.start + 1}. `, t, "\n\n"]
        );
    if (id === "grid-cell") {
        const width = Math.max(b.source.length, t.source.length) + 2;
        const border = `+${"-".repeat(width)}+\n`;
        return pair(
            [
                border,
                "| ",
                b,
                " ".repeat(width - b.source.length - 1),
                "|\n",
                `|${" ".repeat(width)}|\n`,
                "| ",
                t,
                " ".repeat(width - t.source.length - 1),
                "|\n",
                border,
                "\n"
            ],
            ["> ", b, "\n>\n> ", t, "\n\n"]
        );
    }
    if (id === "simple-matrix") {
        const cells = [key, target, value, anchor];
        const w = Math.max(key.source.length, value.source.length, 4) + 2;
        const z = Math.max(target.source.length, anchor.source.length, 4) + 2;
        return pair(
            [
                key,
                " ".repeat(w - key.source.length + 2),
                target,
                "\n",
                "-".repeat(w),
                "  ",
                "-".repeat(z),
                "\n",
                value,
                " ".repeat(w - value.source.length + 2),
                anchor,
                "\n\n"
            ],
            ["| ", cells[0], " | ", cells[1], " |\n| :--- | :--- |\n| ", cells[2], " | ", cells[3], " |\n\n"]
        );
    }
    throw new Error(`no grammar host: ${id}`);
}

/** Stable local field order is semantic field order, independent of host layout. */
function fieldsOf(document) {
    const fields = new Map();
    for (const piece of document.pieces.filter((piece) => piece.kind === "slot")) {
        recognizeValue(piece.grammar, piece.source);
        if (fields.has(piece.name))
            assert.deepEqual(fields.get(piece.name), { grammar: piece.grammar, source: piece.source });
        else fields.set(piece.name, { grammar: piece.grammar, source: piece.source });
    }
    return [...fields].sort(([a], [b]) => a.localeCompare(b));
}
function embedFields(fields) {
    // Both sides pay for the same explicit standalone-parser entry envelope.
    return fields
        .map(([, field]) =>
            field.grammar === "inline"
                ? `${field.source}\n\n`
                : field.grammar === "empty"
                  ? "[](/empty)\n\n"
                  : `\`${field.source}\`\n\n`
        )
        .join("");
}

export function recognizeBoundary(id, source) {
    assert.equal(byId.get(id)?.scope, "boundary-grammar");
    const schema = grammarNormalForm(id, "dialect").fields;
    const paragraphs = source.split("\n\n");
    assert.equal(paragraphs.pop(), "", "unterminated boundary document");
    assert.ok(paragraphs.length > 0 && paragraphs.length % schema.length === 0, "incomplete field sequence");
    const units = [];
    for (let at = 0; at < paragraphs.length; at += schema.length) {
        units.push(
            schema.map((field, i) => {
                const encoded = paragraphs[at + i];
                let value;
                if (field.grammar === "inline") value = recognizeBody(encoded);
                else if (field.grammar === "empty") {
                    assert.equal(encoded, "[](/empty)");
                    value = "";
                } else {
                    assert.ok(encoded.startsWith("`") && encoded.endsWith("`"), "missing literal entry frame");
                    value = recognizeValue(field.grammar, encoded.slice(1, -1));
                }
                return [field.name, { grammar: field.grammar, value }];
            })
        );
    }
    return units;
}

export function encodeBoundary(id, units) {
    const text = units
        .map((fields) =>
            embedFields(
                fields.map(([name, field]) => [
                    name,
                    {
                        grammar: field.grammar,
                        source:
                            field.grammar === "inline"
                                ? renderBody(field.value)
                                : field.grammar === "phrase"
                                  ? field.value.join(" ")
                                  : field.value
                    }
                ])
            )
        )
        .join("");
    assert.deepEqual(recognizeBoundary(id, text), units, "boundary encoding changed a derivation");
    return text;
}

function composeHosts(id, rows, side) {
    if (id.startsWith("metadata")) {
        const rename = (part, i) => (typeof part === "string" ? part : { ...part, name: `${part.name}-${i}` });
        const envelope = rows[0][side].envelope;
        return host([
            envelope.opening,
            rows.flatMap((row, i) => row[side].envelope.members.map((part) => rename(part, i))),
            envelope.closing,
            rows.flatMap((row, i) => row[side].envelope.content.map((part) => rename(part, i)))
        ]);
    }

    return host(
        rows.flatMap((row) =>
            row[side].pieces.length
                ? row[side].pieces.map((piece) =>
                      piece.kind === "slot" ? slot(piece.name, piece.grammar, piece.source) : piece.source
                  )
                : [row[side].source]
        )
    );
}

export function grammarNormalForm(id, side) {
    const c = byId.get(id);
    assert.ok(c && ["dialect", "common"].includes(side));
    if (isProduct(id)) {
        const fields = productGrammar(id)[side].filter((part) => typeof part !== "string");
        const bindings = new Map();
        for (const field of fields) {
            const type = {
                name: field.name,
                grammar: field.grammar,
                ...(field.maxBytes !== undefined ? { maxBytes: field.maxBytes } : {})
            };
            if (bindings.has(field.name)) assert.deepEqual(bindings.get(field.name), type, "binding changed type");
            else bindings.set(field.name, type);
        }
        for (const field of fields) assert.ok(normalForms[field.grammar]);
        return { repetition: "+", fields: [...bindings.values()].toSorted((a, b) => a.name.localeCompare(b.name)) };
    }
    if (c.scope === "boundary-grammar") {
        return {
            repetition: "+",
            fields: grammarUnit(id).fields.map(([name, field]) => ({ name, grammar: field.grammar }))
        };
    }
    return { repetition: "+", grammar: c.grammar };
}

function grammarDescription(certificate, side) {
    if (isProduct(certificate.id)) {
        const grammar = productGrammar(certificate.id)[side];
        return (
            "Document = Unit+ ; Unit = " +
            grammar
                .map((part) =>
                    typeof part === "string"
                        ? JSON.stringify(part)
                        : `${part.name}:${part.grammar}${part.maxBytes ? `{source-bytes<=${part.maxBytes}}` : ""}${part.encoding ? `[${part.encoding}: ${finiteLexicons[part.grammar][part.encoding].map(JSON.stringify).join("|")}]` : ""}`
                )
                .join(" ") +
            " ; word = Word ; attribute-key = AttributeKey ; unicode = UnicodeWord ; ordinal = Ordinal ; state = State ; positive9 = Positive9 ; phrase = Phrase ; inline = Body ; empty = Empty ; OPEN = CLOSE = '**' ; repeated field names in one Unit bind the same value ; " +
            Object.values(normalForms).join(" ")
        );
    }
    if (certificate.scope === "boundary-grammar")
        return (
            "Document = Unit+ ; Unit = " +
            grammarNormalForm(certificate.id, side)
                .fields.map((f) => `${f.name}:${f.grammar}`)
                .join(" ") +
            " ; inline = InlineParagraph ; word = WordParagraph ; phrase = PhraseParagraph ; empty = EmptyLabelParagraph ; InlineParagraph = Body LF LF ; WordParagraph = '`' Word '`' LF LF ; PhraseParagraph = '`' Phrase '`' LF LF ; EmptyLabelParagraph = '[](/empty)' LF LF ; Phrase = Word (SP Word)* ; OPEN = CLOSE = '**' ; " +
            normalForms.inline
        );
    if (directFrames.has(certificate.id))
        return (
            `Document = Item (LF* Item)* LF+ ; Item = ${certificate.id === "task-value" ? JSON.stringify(side === "dialect" ? "- [~] " : "- [x] ") : "Decimal '. '"} Body LF ; Decimal = [1-9][0-9]{0,8} ; OPEN = CLOSE = '**' ; ` +
            normalForms.inline
        );
    const marker = side === "dialect" ? certificate.dialectMarker : certificate.commonMarker;
    const payload = certificate.grammar === "inline" ? "Body" : certificate.grammar === "word" ? "Word" : "Phrase";
    return `Document = Paragraph+ ; Paragraph = 'probe ' OPEN ${payload} CLOSE ' end' LF LF ; OPEN = ${JSON.stringify(marker)} ; CLOSE = ${JSON.stringify(marker)} ; ${normalForms[certificate.grammar]}`;
}

// The grammar determines the finite enumeration, independently of requested
// workload size. Repeated names are one binding; independent fields form a
// Cartesian product. The same source schedule feeds the grammar checker and Callgrind.
function finiteFields(id) {
    if (!isProduct(id)) return [];
    return [
        ...new Map(
            productGrammar(id)
                .dialect.filter((part) => part.encoding)
                .map((field) => [field.name, field])
        ).values()
    ];
}

export function grammarUnit(id, index = 0) {
    const p = parameters(index);
    let ordinal = index;
    p.finiteValues = {};
    for (const field of finiteFields(id)) {
        const tokens = finiteLexicons[field.grammar][field.encoding];
        p.finiteValues[field.name] = decodeField(field, tokens[ordinal % tokens.length]);
        ordinal = Math.floor(ordinal / tokens.length);
    }
    if (isProduct(id) && index % 12 === 11) {
        for (const field of grammarNormalForm(id, "dialect").fields)
            if (field.maxBytes) {
                const value = field.name + p[field.name];
                p[field.name] = value.repeat(Math.ceil(field.maxBytes / value.length)).slice(0, field.maxBytes);
            }
    }
    return instantiateGrammar(id, p);
}

export function instantiateGrammar(id, p) {
    const certificate = byId.get(id);
    assert.ok(certificate, `unknown grammar certificate: ${id}`);
    assert.ok(Number.isSafeInteger(p.start) && p.start >= 1 && p.start <= 26, "invalid host list start");
    assert.ok([0, 1, 2].includes(p.mode), "invalid callout state");
    if (direct[id]) {
        const sourceBody =
            certificate.grammar === "inline" ? p.body : certificate.grammar === "word" ? [p.value] : p.words;
        const sides = [certificate.dialectMarker, certificate.commonMarker].map((marker) => {
            const payload = certificate.grammar === "inline" ? renderBody(sourceBody, marker) : sourceBody.join(" ");
            const source = `probe ${marker}${payload}${marker} end\n\n`;
            return { source, pieces: [] };
        });
        const decoded = sides.map((side, i) => recognizePairedDocument(id, i ? "common" : "dialect", side.source));
        assert.deepEqual(decoded[0], decoded[1]);

        return {
            id,
            certificate: certificate.certificate,
            scope: certificate.scope,
            dialect: sides[0],
            common: sides[1],
            derivation: decoded[0]
        };
    }
    const [dialect, common] = renderHosts(id, p);
    for (const document of [dialect, common]) assert.equal(recomposeHost(document), document.source);
    if (directFrames.has(id) || isProduct(id)) {
        if (certificate.identity)
            assert.equal(dialect.source, common.source, "identity certificate changed source bytes");
        const a = recognizePairedDocument(id, "dialect", dialect.source),
            b = recognizePairedDocument(id, "common", common.source);
        assert.deepEqual(a, b);
        return { id, certificate: certificate.certificate, scope: certificate.scope, dialect, common, derivation: a };
    }
    const left = fieldsOf(dialect),
        right = fieldsOf(common);
    assert.deepEqual(left, right, `${id}: a boundary lost or changed a field`);
    return {
        id,
        certificate: certificate.certificate,
        scope: certificate.scope,
        dialect,
        common,
        fields: left,
        boundary: { dialect: embedFields(left), common: embedFields(right) }
    };
}

export function recomposeHost(document) {
    let at = 0;
    for (const piece of document.pieces) {
        assert.equal(piece.start, at, "missing or overlapping boundary bytes");
        at += Buffer.byteLength(piece.source);
        assert.equal(piece.end, at, "boundary extent mismatch");
    }
    const result = document.pieces.map((piece) => piece.source).join("");
    assert.equal(Buffer.byteLength(result), at);
    return result;
}

export function recognizePairedDocument(id, side, source) {
    const c = byId.get(id);
    assert.ok(c && c.scope === "paired-document-grammar", "not a paired-document certificate");
    assert.ok(["dialect", "common"].includes(side));
    if (isProduct(id)) return recognizeProduct(id, side, source);
    if (directFrames.has(id)) {
        assert.ok(source.endsWith("\n\n"));
        assert.ok(!source.startsWith("\n"), "leading blank outside list grammar");
        const items = [];
        const pattern = /([^\n]+)\n(\n*)/gy;
        while (pattern.lastIndex < source.length) {
            const match = pattern.exec(source);
            assert.ok(match, "invalid item framing");
            const line = match[1],
                blankLines = match[2].length;
            if (id === "task-value") {
                const prefix = side === "dialect" ? "- [~] " : "- [x] ";
                assert.ok(line.startsWith(prefix));
                items.push({ value: "task", body: recognizeBody(line.slice(prefix.length)), blankLines });
            } else {
                const item = /^([1-9][0-9]{0,8})\. (.*)$/u.exec(line);
                assert.ok(item, "invalid decimal item");
                items.push({ value: item[1], body: recognizeBody(item[2]), blankLines });
            }
        }
        assert.ok(items.length && items.at(-1).blankLines > 0);
        return items;
    }
    const marker = side === "dialect" ? c.dialectMarker : c.commonMarker;
    const prefix = `probe ${marker}`,
        suffix = `${marker} end\n\n`;
    const documents = source.split("\n\n");
    assert.equal(documents.pop(), "", "missing document terminator");
    assert.ok(documents.length);
    return documents.map((line) => {
        const unit = line + "\n\n";
        assert.ok(unit.startsWith(prefix) && unit.endsWith(suffix), "outside paired document grammar");
        const body = unit.slice(prefix.length, -suffix.length);
        return c.grammar === "inline" ? recognizeBody(body, marker) : recognizeValue(c.grammar, body);
    });
}

/** The inverse translation is executable and accepts decoded derivations,
 * independently of the deterministic benchmark schedule. */
export function encodePairedDocument(id, side, derivations) {
    assert.ok(["dialect", "common"].includes(side) && derivations.length > 0);
    const c = byId.get(id);
    assert.ok(c?.scope === "paired-document-grammar");
    let text;
    if (isProduct(id)) {
        const grammar = productGrammar(id)[side];
        text = derivations
            .map((fields) => {
                const values = new Map(fields);
                assert.equal(values.size, grammarNormalForm(id, side).fields.length);
                return grammar
                    .map((part) => {
                        if (typeof part === "string") return part;
                        const field = values.get(part.name);
                        assert.equal(field?.grammar, part.grammar);
                        return encodeField(part, field.value);
                    })
                    .join("");
            })
            .join("");
    } else if (directFrames.has(id)) {
        text = derivations
            .map((item) => {
                assert.ok(Number.isSafeInteger(item.blankLines) && item.blankLines >= 0);
                if (id === "task-value") assert.equal(item.value, "task");
                else assert.match(item.value, /^[1-9][0-9]{0,8}$/u);
                return (
                    (id === "task-value" ? (side === "dialect" ? "- [~] " : "- [x] ") : `${item.value}. `) +
                    renderBody(item.body) +
                    "\n".repeat(item.blankLines + 1)
                );
            })
            .join("");
    } else {
        const marker = side === "dialect" ? c.dialectMarker : c.commonMarker;
        text = derivations
            .map(
                (value) =>
                    `probe ${marker}${c.grammar === "inline" ? renderBody(value, marker) : c.grammar === "phrase" ? value.join(" ") : value}${marker} end\n\n`
            )
            .join("");
    }
    assert.deepEqual(recognizePairedDocument(id, side, text), derivations, "encoding changed a derivation");
    return text;
}

export function grammarCatalog() {
    const corpus = buildGrammarCorpus({ units: 2, scale: 1 });
    return {
        version: grammarVersion,
        normalForms,
        finiteLexicons,
        certificates: corpus.proofs.map((proof) => ({
            id: proof.id,
            certificate: proof.certificate,
            scope: proof.scope,
            legacy: proof.legacy,
            structuralPredecessor: proof.structuralPredecessor,
            feature: proof.feature ?? null,
            facets: proof.facets ?? [],
            identity: proof.identity === true,
            outputDifference: proof.outputDifference ?? null,
            theorem:
                proof.scope === "boundary-grammar"
                    ? "T5"
                    : proof.identity
                      ? "T6"
                      : isProduct(proof.id)
                        ? productGrammar(proof.id).dialect.some((part) => part.encoding) ||
                          ["dialect", "common"].some((side) => {
                              const fields = productGrammar(proof.id)[side].filter((part) => typeof part !== "string");
                              return new Set(fields.map((field) => field.name)).size !== fields.length;
                          })
                            ? "T7"
                            : "T3"
                        : directFrames.has(proof.id)
                          ? "T4"
                          : "T2",
            normalForm: proof.normalForm,
            grammars: proof.grammars,
            rewrite: proof.rewrite,
            residual: proof.residual,
            examples: proof.rows.slice(0, 2).map((row) => ({
                dialect: row.dialect.source,
                common: row.common.source,
                ...(row.derivation
                    ? { derivation: row.derivation }
                    : {
                          fields: row.fields,
                          boundary: row.boundary,
                          partitions: { dialect: row.dialect.pieces, common: row.common.pieces }
                      })
            }))
        }))
    };
}

export function buildGrammarCorpus({ units = 12, scale = 2 } = {}) {
    validateGrammarCertificates();
    assert.ok(Number.isSafeInteger(units) && units > 0 && units <= 1024);
    assert.ok(Number.isSafeInteger(scale) && scale > 0 && scale <= 16);
    const cases = [],
        proofs = [];
    for (const certificate of grammarCertificates) {
        const alternatives = finiteFields(certificate.id).reduce(
            (count, field) => count * finiteLexicons[field.grammar][field.encoding].length,
            1
        );
        for (let level = 1; level <= scale; level++) {
            const rows = Array.from({ length: Math.max(units, alternatives) * level }, (_, index) =>
                grammarUnit(certificate.id, index)
            );
            const bound = certificate.scope === "boundary-grammar";
            const names = {};
            const hosts = Object.fromEntries(
                ["dialect", "common"].map((side) => [side, composeHosts(certificate.id, rows, side)])
            );
            for (const document of Object.values(hosts)) assert.equal(recomposeHost(document), document.source);
            for (const side of ["dialect", "common"]) {
                for (const part of bound ? ["host", "boundary"] : ["paired"]) {
                    const name = `grammar-${certificate.id}-${part}-${side}`;
                    names[`${part}-${side}`] = name;
                    const text =
                        part === "boundary" ? rows.map((row) => row.boundary[side]).join("") : hosts[side].source;
                    cases.push({
                        name,
                        side,
                        part,
                        certificate: certificate.certificate,
                        id: certificate.id,
                        scale: level,
                        units: rows.length,
                        text,
                        sha256: hash(text),
                        bytes: Buffer.byteLength(text),
                        dialect: side === "dialect" && part !== "boundary" ? "extended" : "commonmark",
                        gfm:
                            featureGrammars.get(certificate.id)?.gfm === true ||
                            ([
                                "task-value",
                                "simple-matrix",
                                "specimen-graph",
                                "headless-matrix",
                                "leading-caption",
                                "trailing-caption"
                            ].includes(certificate.id) &&
                                part !== "boundary"),
                        carries: [],
                        growth: "grammar derivations"
                    });
                }
            }
            proofs.push({
                ...certificate,
                normalForm: grammarNormalForm(certificate.id, "dialect"),
                scale: level,
                units: rows.length,
                names,
                rows,
                hosts,
                grammars: {
                    dialect: grammarDescription(certificate, "dialect"),
                    common: grammarDescription(certificate, "common")
                },
                rewrite: bound
                    ? [
                          "lossless-slot-decomposition",
                          "common-field-order",
                          "shared-explicit-entry-envelope",
                          "identity-grammar"
                      ]
                    : isProduct(certificate.id)
                      ? [
                            "inline-administrative-productions",
                            "invertible-fixed-terminal-encoding",
                            "label-preserving-product-permutation",
                            "common-normal-form"
                        ]
                      : [
                            "inline-administrative-productions",
                            "rename-contextual-delimiter-terminals",
                            "common-normal-form"
                        ]
            });
        }
    }
    return { version: grammarVersion, normalForms, certificates: grammarCertificates, cases, proofs };
}

export function documentMetadata(item) {
    const result = { ...item };
    delete result.text;
    return result;
}

/** Only the certified counterpart has a reference measurement. Parsing an
 * extension host as ordinary CommonMark would manufacture an unrelated floor. */
export function grammarEngines(document) {
    assert.ok(["dialect", "common"].includes(document.side));
    assert.ok(["paired", "boundary", "host"].includes(document.part));
    return document.side === "common" && document.part !== "host"
        ? ["markdown-core", document.gfm ? "cmark-gfm" : "cmark"]
        : ["markdown-core"];
}

export function writeGrammarCorpus(directory, options) {
    const corpus = buildGrammarCorpus(options);
    const coverage = validateFeatureCoverage(fileURLToPath(new URL("../../", import.meta.url)), corpus);
    fs.mkdirSync(directory, { recursive: true });
    // This namespace belongs to the generator. A rerun with fewer scales or
    // upgraded boundary certificates must not archive obsolete source halves.
    const names = new Set(corpus.cases.map((item) => `${item.name}.x${item.scale}.md`));
    for (const name of fs.readdirSync(directory))
        if (/^grammar-[a-z0-9-]+\.x[1-9][0-9]*\.md$/u.test(name) && !names.has(name))
            fs.unlinkSync(path.join(directory, name));
    for (const item of corpus.cases)
        fs.writeFileSync(path.join(directory, `${item.name}.x${item.scale}.md`), item.text);
    const identity = hash(
        JSON.stringify({
            sourceIdentity: grammarSourceIdentity(),
            version: corpus.version,
            normalForms,
            proofs: corpus.proofs,
            documents: corpus.cases.map(documentMetadata)
        })
    );
    const index = {
        version: corpus.version,
        identity,
        normalForms,
        certificates: corpus.certificates,
        coverage,
        cases: corpus.cases.map(documentMetadata),
        proofs: corpus.proofs
    };
    fs.writeFileSync(path.join(directory, "grammar-corpus.json"), JSON.stringify(index, null, 2) + "\n");
    return { ...corpus, identity, coverage };
}

export function grammarSourceIdentity(root = fileURLToPath(new URL("../../", import.meta.url))) {
    const files = [
        "scripts/lib/grammar-corpus.mjs",
        "scripts/lib/grammar-features.mjs",
        "scripts/lib/grammar-coverage.mjs",
        "scripts/lib/grammar-sections.mjs",
        "scripts/lib/element-inventory.mjs",
        "scripts/lib/pair-productions.mjs",
        "scripts/lib/pair-review.mjs",
        "scripts/benchmark-stages.mjs",
        "scripts/init-environment.sh",
        "docs/architecture/benchmark-grammar-corpus.md",
        "docs/architecture/benchmark-grammar-coverage.md",
        "packages/markdown-core/benchmarks/grammar-corpus.json",
        "packages/markdown-core/benchmarks/grammar-coverage.json",
        "docs/specs/dialect.md",
        ...fs
            .readdirSync(path.join(root, "docs/specs/dialect"))
            .filter((file) => file.endsWith(".md"))
            .map((file) => `docs/specs/dialect/${file}`)
    ];
    return hash(
        files
            .sort()
            .map((file) => `${file}\0${hash(fs.readFileSync(path.join(root, file)))}`)
            .join("\n")
    );
}

export function grammarMarkdown(report) {
    const g = report.grammarCorpus;
    const ir = (item, engine) =>
        item?.engines[engine]
            ? Object.values(item.engines[engine].stages).reduce((sum, stage) => sum + stage.ir, 0)
            : null;
    const lines = [
        "## Corpus certified by grammar equivalence",
        "",
        `Grammar identity: \`${g.identity}\`. ${g.certificates.length} certificates cover all 30 historical scenarios: ${g.certificates.filter((c) => c.scope === "paired-document-grammar").length} paired document grammars and ${g.certificates.filter((c) => c.scope === "boundary-grammar").length} explicit boundary grammars.`,
        ...(g.coverage
            ? [
                  "",
                  `Feature coverage: ${g.coverage.features} specification features, ${g.coverage.elements} registered elements, ${g.coverage.sections} specification sections. Coverage identity: \`${g.coverage.identity}\`. Shared syntax uses identical input. Coverage is of features with explicitly bounded proof domains; boundary host ratios remain uncertified.`
              ]
            : []),
        "",
        "Each certificate has source grammars, a common normal form, a mathematical transformation proof and generated derivations. Native AST topology is not an admission rule. The proof domain is the declared grammar; timings of unrestricted native parsers are implementation measurements, not a proof of their global optimum.",
        "",
        "Boundary rows measure the same explicit standalone entry envelope on both sides. Original hosts and lossless residuals are archived separately. The local ratio excludes the unmatched host grammar and is not additive with it.",
        "",
        "A is Core on the dialect encoding, B is Core on the common encoding, and R is the pinned reference on the common encoding. All Ir totals below are source_to_buffer + buffer_to_ast. A/B includes the whole grammar/frame change; it is not a lexical-only attribution.",
        "",
        "| Certificate | Scope | Scale | Units | A/B bytes | A Ir | B Ir | R Ir | A/B | B/R | A/R | Core host Ir (residual included) |",
        "| --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    ];
    for (const proof of g.proofs) {
        const part = proof.scope === "boundary-grammar" ? "boundary" : "paired";
        const find = (name) => report.cases.find((item) => item.case === name && item.scale === proof.scale);
        const a = find(proof.names[`${part}-dialect`]),
            b = find(proof.names[`${part}-common`]);
        if (!a && !b) continue;
        assert.ok(a && b, "missing measured grammar counterpart");
        const engine = b.gfm ? "cmark-gfm" : "cmark";
        const left = ir(a, "markdown-core"),
            common = ir(b, "markdown-core"),
            right = ir(b, engine);
        assert.ok(left > 0 && common > 0 && right > 0, "missing grammar measurement");
        assert.equal(a.certificate, proof.certificate, "measurement belongs to another grammar");
        assert.equal(b.certificate, proof.certificate, "measurement belongs to another grammar");
        assert.equal(a.units, proof.units);
        assert.equal(b.units, proof.units);
        const hostCost = part === "boundary" ? ir(find(proof.names["host-dialect"]), "markdown-core") : null;
        lines.push(
            `| ${proof.certificate} | ${proof.scope} | ${proof.scale} | ${proof.units} | ${a.bytes}/${b.bytes} | ${left} | ${common} | ${right} | ${(left / common).toFixed(3)}x | ${(common / right).toFixed(3)}x | ${(left / right).toFixed(3)}x | ${hostCost ?? "—"} |`
        );
    }
    lines.push("", "### Unmatched boundary obligations", "", "| Certificate | Residual |", "| --- | --- |");
    for (const c of g.certificates.filter((c) => c.residual)) lines.push(`| ${c.certificate} | ${c.residual} |`);
    lines.push(
        "",
        "### Grammar identity with different output work",
        "",
        "The shared grammar does not require identical native output models. Core also computes automatic heading anchors and retains typed metadata on nodes. The following documented output difference affects the interpretation of measurements:"
    );
    for (const c of g.certificates.filter((c) => c.outputDifference))
        lines.push(`- ${c.certificate}: ${c.outputDifference}`);
    return lines.join("\n") + "\n";
}
