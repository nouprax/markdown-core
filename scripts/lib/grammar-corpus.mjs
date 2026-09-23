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

export const grammarVersion = "grammar-corpus-v1";
export const hash = (value) => createHash("sha256").update(value).digest("hex");

// The four normal forms are grammars, not shapes learned from native output.
export const normalForms = Object.freeze({
    word: "Word = [a-z]+ ;",
    phrase: "Phrase = Word (SP Word)* ; Word = [a-z]+ ;",
    inline: "Body = Word | Word SP (Atom SP)* Word ; Atom = Word | OPEN Body CLOSE ; Word = [a-z]+ ;",
    empty: "Empty = epsilon ;"
});

const direct = {
    "insertion-strong": ["inline", "++", "**", "Insertion"],
    "run-insertion": ["phrase", "++", "**", "Insertion"],
    "run-mark": ["phrase", "==", "**", "Mark"],
    "run-strike": ["phrase", "~~", "**", "Strikethrough"],
    "run-super": ["word", "^", "*", "Superscript"],
    "run-sub": ["word", "~", "*", "Subscript"],
    "opaque-comment": ["phrase", "%%", "``", "Comment"],
    "opaque-formula": ["phrase", "$", "`", "Formula"],
    "opaque-display": ["phrase", "$$", "``", "Formula"]
};
const directFrames = new Set(["task-value", "decimal-list"]);
const splitIds = new Set([
    "alpha-list",
    "upper-list",
    "roman-list",
    "upper-roman-list",
    "default-list",
    "enclosed-default-list",
    "grid-cell",
    "simple-matrix",
    "specimen-graph",
    "metadataempty",
    "metadata",
    "callout"
]);
const isProduct = (id) => !direct[id] && !directFrames.has(id) && !splitIds.has(id);

const boundaryReason = (id) => {
    if (id.startsWith("leaf-"))
        return "Fence/promotion recognition, info fields and terminal-LF normalization remain outside the common payload grammar.";
    if (id.includes("list") || id === "loose-definition")
        return "Marker value conversion, continuation, term/body discovery and block ownership remain outside the common inline-field grammar.";
    if (id.includes("container"))
        return "Fence/name/attribute decisions and quote-prefix continuation do not share the body grammar; keep those frame bytes as residual.";
    if (id.includes("directive"))
        return "Directive name/envelope recognition and link destination/activation remain residual; the label grammar is compared separately.";
    if (id.startsWith("cross-"))
        return "Cross-field separation and URL/title validation/normalization remain residual; every present independent field is retained in the local value grammar.";
    if (id.startsWith("cite-"))
        return "Citation mode, URI recognition, derived text and unresolved binding behavior remain residual; the key value grammar is matched.";
    if (id === "specimen-graph")
        return "Definition/call recognition, symbol resolution, hoisting and ordinals remain residual; definition bodies are isolated with their original keys retained in the host.";
    if (id === "simple-matrix" || id === "grid-cell")
        return "Geometry, column discovery, padding and cell source mapping remain residual; all cell contents, in source order, enter the local inline grammar.";
    if (id === "record-span" || id === "class-span")
        return "Attribute member grammar and Markdown URL/title decoding are not identified; independent label and value fields are split explicitly.";
    if (id === "anchor")
        return "Identifier declaration/attachment remains residual; the complete host inline body is retained.";
    if (id.startsWith("metadata"))
        return "Envelope detection, typed member decoding and overwrite/unknown-member policy remain residual; the following body is retained.";
    if (id === "callout")
        return "Callout kind, collapse state and header recognition remain residual; title inline content is retained.";
    throw new Error(`missing boundary disposition: ${id}`);
};

const ids = [
    "insertion-strong",
    ...[...productionProofs.keys()].map((id) => id.replace(/-v2$/u, "")),
    "anchor",
    "metadataempty",
    "metadata",
    "callout"
];
export const grammarCertificates = ids.map((id) => {
    const full = direct[id] || directFrames.has(id) || isProduct(id);
    const legacy = [...pairReviews.values()]
        .filter((review) => review.proofs.includes(`${id}-v2`) || review.id === id)
        .map((review) => review.id);
    if (id === "insertion-strong") legacy.push("runs");
    return Object.freeze({
        id,
        certificate: `${id}-grammar-v1`,
        scope: full ? "paired-document-grammar" : "boundary-grammar",
        grammar: direct[id]?.[0] ?? (directFrames.has(id) ? id : isProduct(id) ? "labelled-product" : "field-sequence"),
        legacy,
        structuralPredecessor:
            id === "insertion-strong" ? "insertion-strong-v1" : productionProofs.has(`${id}-v2`) ? `${id}-v2` : null,
        ...(direct[id]
            ? { dialectMarker: full[1], commonMarker: full[2], expectedKind: full[3], residual: null }
            : full
              ? { residual: null }
              : { residual: boundaryReason(id) })
    });
});
const byId = new Map(grammarCertificates.map((entry) => [entry.id, entry]));

export function validateGrammarCertificates() {
    assert.equal(byId.size, 47, "every old structural domain and four unpaired families need a grammar disposition");
    assert.deepEqual(new Set(grammarCertificates.flatMap((entry) => entry.legacy)), new Set(pairReviews.keys()));
    for (const entry of grammarCertificates) {
        if (entry.scope === "paired-document-grammar") {
            if (isProduct(entry.id)) {
                assert.deepEqual(grammarNormalForm(entry.id, "dialect"), grammarNormalForm(entry.id, "common"));
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
    if (grammar === "phrase") {
        assert.match(source, /^[a-z]+(?: [a-z]+)*$/u);
        return source.split(" ");
    }
    if (grammar === "empty") {
        assert.equal(source, "");
        return "";
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
        start: 1 + (index % 18)
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
function host(parts, kinds = []) {
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
    return { source, pieces, kinds };
}

/** Finite product grammars. A field is a nonterminal, never a sample value.
 * Inlining these productions gives the same labelled product on both sides.
 */
export function productGrammar(id) {
    assert.ok(isProduct(id), "not a product grammar");
    const f = (name, grammar = "word") => ({ name, grammar });
    const b = f("body", "inline"),
        t = f("tail", "inline"),
        k = f("key"),
        v = f("value"),
        target = f("target"),
        anchor = f("anchor"),
        literal = f("literal", "phrase");
    const pair = (dialect, common, a, r) => ({ dialect: dialect.flat(), common: common.flat(), kinds: [a, r] });
    const frame = (parts) => ["probe ", ...parts, " end\n\n"];
    if (id.startsWith("leaf-")) {
        if (id === "leaf-directive")
            return pair(["::", k, "[", b, "]\n\n"], ["[", b, "](/", k, ")\n\n"], ["DirectiveBlock"], ["Link"]);
        const [open, close] =
            id === "leaf-comment"
                ? ["%%\n", "\n%%\n\n"]
                : id === "leaf-fence"
                  ? ["```formula\n", "\n```\n\n"]
                  : id === "leaf-promotion"
                    ? ["$$", "$$\n\n"]
                    : ["$$\n", "\n$$\n\n"];
        return pair(
            [open, literal, close],
            ["```\n", literal, "\n```\n\n"],
            [id === "leaf-comment" ? "Comment" : "FormulaBlock"],
            ["CodeBlock"]
        );
    }
    if (id === "record-span")
        return pair(
            frame(["[", b, "]{", k, '="', v, '"}']),
            frame(["[", b, "](/", k, ' "', v, '")']),
            ["Span"],
            ["Link"]
        );
    if (id === "class-span") return pair(frame(["[]{.", v, "}"]), frame(["[](/", v, ")"]), ["Span"], ["Link"]);
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
            frame([embed ? "![](/" : "[](/", ...fields, ...(absent ? [] : [' "', label, '"']), ")"]),
            [embed ? "CrossEmbedded" : "CrossLink"],
            [embed ? "Embedded" : "Link"]
        );
    }
    if (id.startsWith("cite-"))
        return pair(
            frame([
                id === "cite-normal" ? "[@" : id === "cite-suppress" ? "-@" : "@",
                k,
                id === "cite-normal" ? "]" : ""
            ]),
            frame(["<https://", k, ">"]),
            ["Citation"],
            ["Link"]
        );
    if (id === "inline-directive" || id === "empty-directive") {
        const label = id === "empty-directive" ? f("body", "empty") : b;
        return pair(frame([":", k, "[", label, "]"]), frame(["[", label, "](/", k, ")"]), ["Directive"], ["Link"]);
    }
    if (id.includes("container"))
        return pair(
            [...(id === "named-container" ? ["::: ", k, "\n"] : ["::: {}\n"]), b, "\n\n", t, "\n:::\n\n"],
            [...(id === "named-container" ? ["> ", k, "\n>\n"] : []), "> ", b, "\n>\n> ", t, "\n\n"],
            ["DirectiveBlock"],
            ["Callout"]
        );
    if (id === "loose-definition")
        return pair([literal, "\n\n: ", b, "\n\n"], ["- ", literal, "\n\n  ", b, "\n\n"], ["DefinitionList"], ["List"]);
    if (id === "anchor")
        return pair(["> ", b, "\n\n#", k, "#\n\n"], ["> ", b, "\n\n[", k, "]: /target\n\n"], ["Callout"], ["Callout"]);
    throw new Error(`no product grammar: ${id}`);
}

const escapePattern = (s) => s.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
export function recognizeProduct(id, side, source) {
    assert.ok(["dialect", "common"].includes(side));
    const grammar = productGrammar(id)[side];
    const fields = grammar.filter((part) => typeof part !== "string");
    const pattern = new RegExp(
        grammar
            .map((part) =>
                typeof part === "string"
                    ? escapePattern(part)
                    : `(${part.grammar === "word" ? "[a-z]+" : part.grammar === "phrase" ? "[a-z]+(?: [a-z]+)*" : part.grammar === "inline" ? "[a-z* ]+" : ""})`
            )
            .join(""),
        "y"
    );
    const result = [];
    while (pattern.lastIndex < source.length) {
        const match = pattern.exec(source);
        assert.ok(match, `${id}/${side}: outside product grammar`);
        result.push(
            fields
                .map((field, i) => [
                    field.name,
                    { grammar: field.grammar, value: recognizeValue(field.grammar, match[i + 1]) }
                ])
                .sort(([a], [b]) => a.localeCompare(b))
        );
    }
    assert.ok(result.length, "empty product document");
    return result;
}

function roman(n) {
    let result = "";
    for (const [value, digit] of [
        [1000, "m"],
        [900, "cm"],
        [500, "d"],
        [400, "cd"],
        [100, "c"],
        [90, "xc"],
        [50, "l"],
        [40, "xl"],
        [10, "x"],
        [9, "ix"],
        [5, "v"],
        [4, "iv"],
        [1, "i"]
    ])
        while (n >= value) {
            result += digit;
            n -= value;
        }
    return result;
}

function renderHosts(id, p) {
    const b = slot("body", "inline", renderBody(p.body));
    const t = slot("tail", "inline", renderBody(p.tail));
    const value = slot("value", "word", p.value);
    const key = slot("key", "word", p.key);
    const target = slot("target", "word", p.target);
    const anchor = slot("anchor", "word", p.anchor);
    const pair = (a, r, kinds, referenceKinds) => [host(a, kinds), host(r, referenceKinds)];
    if (isProduct(id)) {
        const { dialect, common, kinds } = productGrammar(id);
        const values = {
            body: renderBody(p.body),
            tail: renderBody(p.tail),
            key: p.key,
            target: p.target,
            value: p.value,
            anchor: p.anchor,
            literal: p.words.join(" "),
            label: id.endsWith("empty") ? "" : p.words.join(" ")
        };
        if (id === "empty-directive") values.body = "";
        return [dialect, common].map((grammar, i) =>
            host(
                grammar.map((part) =>
                    typeof part === "string" ? part : slot(part.name, part.grammar, values[part.name])
                ),
                kinds[i]
            )
        );
    }
    if (id === "task-value")
        return pair(
            ["- [~] ", b, "\n- [~] ", t, "\n\n"],
            ["- [x] ", b, "\n- [x] ", t, "\n\n"],
            ["ListItem"],
            ["ListItem"]
        );
    if (id.includes("list")) {
        const mark = (n) =>
            id === "alpha-list"
                ? `${String.fromCharCode(96 + n)})`
                : id === "upper-list"
                  ? `(${String.fromCharCode(64 + n)})`
                  : id === "roman-list"
                    ? `${roman(n)}.`
                    : id === "upper-roman-list"
                      ? `${roman(n).toUpperCase()})`
                      : id === "default-list"
                        ? "#."
                        : id === "enclosed-default-list"
                          ? "(#)"
                          : `${n}.`;
        return pair(
            [`${mark(p.start)} `, b, `\n${mark(p.start + 1)} `, t, "\n\n"],
            [`${p.start}. `, b, `\n${p.start + 1}. `, t, "\n\n"],
            ["List"],
            ["List"]
        );
    }
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
            ["> ", b, "\n>\n> ", t, "\n\n"],
            ["Table", "TableCell"],
            ["Callout"]
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
            ["| ", cells[0], " | ", cells[1], " |\n| :--- | :--- |\n| ", cells[2], " | ", cells[3], " |\n\n"],
            ["Table"],
            ["Table"]
        );
    }
    if (id === "specimen-graph")
        return pair(
            ["As (@", key, ") shows.\n\n(@", key, ") ", b, "\n\n"],
            ["As [^", key, "] shows.\n\n[^", key, "]: ", b, "\n\n"],
            ["Specimen", "Citation"],
            ["Footnote", "Cite"]
        );
    if (id.startsWith("metadata"))
        return pair(
            ["---\n", id === "metadataempty" ? "unknown: " : "name: ", value, "\n---\n\n", b, "\n\n"],
            ["```\n", id === "metadataempty" ? "unknown: " : "name: ", value, "\n```\n\n", b, "\n\n"],
            ["Metadata"],
            ["CodeBlock"]
        );
    if (id === "callout")
        return pair([`> [!note]${["", "-", "+"][p.mode]} `, b, "\n\n"], ["> ", b, "\n\n"], ["Callout"], ["Callout"]);
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
        const values = rows.map((row, i) =>
            slot(`value-${i}`, "word", row.fields.find(([name]) => name === "value")[1].source)
        );
        const bodies = rows.map((row, i) =>
            slot(`body-${i}`, "inline", row.fields.find(([name]) => name === "body")[1].source)
        );
        return host(
            [
                side === "dialect" ? "---\n" : "```\n",
                values.flatMap((value) => [id === "metadataempty" ? "unknown: " : "name: ", value, "\n"]),
                side === "dialect" ? "---\n\n" : "```\n\n",
                bodies.flatMap((body) => [body, "\n\n"])
            ],
            side === "dialect" ? ["Metadata"] : ["CodeBlock"]
        );
    }
    return host(
        rows.flatMap((row) =>
            row[side].pieces.length
                ? row[side].pieces.map((piece) =>
                      piece.kind === "slot" ? slot(piece.name, piece.grammar, piece.source) : piece.source
                  )
                : [row[side].source]
        ),
        rows[0][side].kinds
    );
}

export function grammarNormalForm(id, side) {
    const c = byId.get(id);
    assert.ok(c && ["dialect", "common"].includes(side));
    if (isProduct(id)) {
        const fields = productGrammar(id)[side].filter((part) => typeof part !== "string");
        assert.equal(new Set(fields.map((f) => f.name)).size, fields.length, "product must have independent fields");
        for (const field of fields) assert.ok(normalForms[field.grammar]);
        return { repetition: "+", fields: fields.toSorted((a, b) => a.name.localeCompare(b.name)) };
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
                .map((part) => (typeof part === "string" ? JSON.stringify(part) : `${part.name}:${part.grammar}`))
                .join(" ") +
            " ; word = Word ; phrase = Phrase ; inline = Body ; empty = Empty ; OPEN = CLOSE = '**' ; " +
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

export function grammarUnit(id, index = 0) {
    return instantiateGrammar(id, parameters(index));
}

export function instantiateGrammar(id, p) {
    const certificate = byId.get(id);
    assert.ok(certificate, `unknown grammar certificate: ${id}`);
    assert.ok(Number.isSafeInteger(p.start) && p.start >= 1 && p.start <= 25, "invalid host list start");
    assert.ok([0, 1, 2].includes(p.mode), "invalid callout state");
    if (direct[id]) {
        const sourceBody =
            certificate.grammar === "inline" ? p.body : certificate.grammar === "word" ? [p.value] : p.words;
        const sides = [certificate.dialectMarker, certificate.commonMarker].map((marker) => {
            const payload = certificate.grammar === "inline" ? renderBody(sourceBody, marker) : sourceBody.join(" ");
            const source = `probe ${marker}${payload}${marker} end\n\n`;
            return { source, kinds: [certificate.expectedKind], pieces: [] };
        });
        const decoded = sides.map((side, i) => recognizePairedDocument(id, i ? "common" : "dialect", side.source));
        assert.deepEqual(decoded[0], decoded[1]);
        sides[1].kinds = [
            id.startsWith("opaque-") ? "Code" : certificate.commonMarker.length === 1 ? "Emphasis" : "Strong"
        ];
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
    const left = fieldsOf(dialect),
        right = fieldsOf(common);
    assert.deepEqual(left, right, `${id}: a boundary lost or changed a field`);
    for (const document of [dialect, common]) assert.equal(recomposeHost(document), document.source);
    if (directFrames.has(id) || isProduct(id)) {
        const a = recognizePairedDocument(id, "dialect", dialect.source),
            b = recognizePairedDocument(id, "common", common.source);
        assert.deepEqual(a, b);
        return { id, certificate: certificate.certificate, scope: certificate.scope, dialect, common, derivation: a };
    }
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
                        return field.grammar === "inline"
                            ? renderBody(field.value)
                            : field.grammar === "phrase"
                              ? field.value.join(" ")
                              : field.value;
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
        certificates: corpus.proofs.map((proof) => ({
            id: proof.id,
            certificate: proof.certificate,
            scope: proof.scope,
            legacy: proof.legacy,
            structuralPredecessor: proof.structuralPredecessor,
            theorem:
                proof.scope === "boundary-grammar"
                    ? "T5"
                    : isProduct(proof.id)
                      ? "T3"
                      : directFrames.has(proof.id)
                        ? "T4"
                        : "T2",
            normalForm: proof.normalForm,
            grammars: proof.grammars,
            rewrite: proof.rewrite,
            residual: proof.residual,
            examples: proof.rows.map((row) => ({
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
        for (let level = 1; level <= scale; level++) {
            const rows = Array.from({ length: units * level }, (_, index) => grammarUnit(certificate.id, index));
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
                            ["task-value", "simple-matrix", "specimen-graph"].includes(certificate.id) &&
                            part !== "boundary",
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

export function writeGrammarCorpus(directory, options) {
    const corpus = buildGrammarCorpus(options);
    fs.mkdirSync(directory, { recursive: true });
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
        certificates: grammarCertificates,
        cases: corpus.cases.map(documentMetadata),
        proofs: corpus.proofs
    };
    fs.writeFileSync(path.join(directory, "grammar-corpus.json"), JSON.stringify(index, null, 2) + "\n");
    return { ...corpus, identity };
}

export function grammarSourceIdentity(root = fileURLToPath(new URL("../../", import.meta.url))) {
    const library = fs
        .readdirSync(path.join(root, "scripts/lib"), { recursive: true })
        .filter((file) => file.endsWith(".mjs"))
        .map((file) => `scripts/lib/${file.split(path.sep).join("/")}`);
    const files = [
        ...library,
        "scripts/benchmark-stages.mjs",
        "scripts/audit-corpus-pairs.mjs",
        "scripts/init-environment.sh",
        "docs/architecture/benchmark-grammar-corpus.md",
        "packages/markdown-core/benchmarks/grammar-corpus.json"
    ];
    return hash(
        files
            .sort()
            .map((file) => `${file}\0${hash(fs.readFileSync(path.join(root, file)))}`)
            .join("\n")
    );
}

const allNodes = (root) => {
    const result = [],
        pending = [root];
    while (pending.length) {
        const node = pending.pop();
        result.push(node);
        pending.push(...node.children.toReversed());
    }
    return result;
};
const bodyMeaning = (body) => {
    const result = [];
    for (const [i, part] of body.entries()) {
        if (i) {
            if (result.at(-1)?.text !== undefined) result.at(-1).text += " ";
            else result.push({ text: " " });
        }
        if (typeof part !== "string") result.push({ span: bodyMeaning(part) });
        else if (result.at(-1)?.text !== undefined) result.at(-1).text += part;
        else result.push({ text: part });
    }
    return result;
};
function inlineMeaning(nodes, kind) {
    return nodes.map((node) => {
        if (node.kind === "Text") return { text: node.fields.literal };
        assert.equal(node.kind, kind, "unexpected production in native inline result");
        return { span: inlineMeaning(node.children, kind) };
    });
}

/** Native execution checks the grammar-derived expectations. It is not the
 * mathematical proof, and none of its output supplies a grammar definition.
 */
export function auditGrammarCorpus(corpus, parse) {
    let parses = 0;
    for (const proof of corpus.proofs) {
        for (const side of ["dialect", "common"]) {
            const entry = corpus.cases.find(
                (item) =>
                    item.name === proof.names[`${proof.scope === "boundary-grammar" ? "host" : "paired"}-${side}`] &&
                    item.scale === proof.scale
            );
            const tree = parse(side === "dialect" ? "core" : entry.gfm ? "gfm" : "cmark", entry.text);
            parses++;
            const nodes = allNodes(tree);
            for (const kind of proof.hosts[side].kinds)
                assert.ok(
                    nodes.some((node) => node.kind === kind),
                    `${proof.id}/${side}: native host did not recognize ${kind}`
                );
            if (proof.scope === "paired-document-grammar") {
                const derivations = recognizePairedDocument(proof.id, side, entry.text);
                assert.equal(encodePairedDocument(proof.id, side, derivations), entry.text);
                if (isProduct(proof.id)) {
                    assert.deepEqual(
                        derivations,
                        proof.rows.flatMap((row) => row.derivation)
                    );
                } else if (directFrames.has(proof.id)) {
                    const items = nodes.filter((node) => node.kind === "ListItem");
                    assert.equal(items.length, derivations.length, `${proof.id}: lost list item`);
                    for (const [i, item] of items.entries()) {
                        assert.equal(item.children.length, 1);
                        assert.equal(item.children[0].kind, "Paragraph");
                        assert.deepEqual(
                            inlineMeaning(item.children[0].children, "Strong"),
                            bodyMeaning(derivations[i].body)
                        );
                        if (proof.id === "task-value")
                            assert.equal(
                                side === "dialect" ? item.fields.marker : item.fields.completed,
                                side === "dialect" ? "~" : "true"
                            );
                    }
                } else {
                    assert.equal(tree.children.length, derivations.length);
                    const kind =
                        side === "dialect"
                            ? proof.expectedKind
                            : proof.id.startsWith("opaque-")
                              ? "Code"
                              : proof.commonMarker.length === 1
                                ? "Emphasis"
                                : "Strong";
                    for (const [i, paragraph] of tree.children.entries()) {
                        assert.equal(paragraph.kind, "Paragraph");
                        assert.equal(paragraph.children.length, 3);
                        const [before, node, after] = paragraph.children;
                        assert.equal(before.kind, "Text");
                        assert.equal(before.fields.literal, "probe ");
                        assert.equal(after.kind, "Text");
                        assert.equal(after.fields.literal, " end");
                        assert.equal(node.kind, kind);
                        const value = derivations[i];
                        if (proof.id.startsWith("opaque-")) {
                            assert.equal(node.fields.literal, value.join(" "));
                            if (side === "dialect" && proof.expectedKind === "Formula")
                                assert.equal(
                                    node.fields.mode,
                                    proof.id === "opaque-display" ? "standalone" : "embedded"
                                );
                        } else
                            assert.deepEqual(
                                inlineMeaning(node.children, kind),
                                bodyMeaning(typeof value === "string" ? [value] : value)
                            );
                    }
                }
            } else {
                const fields = proof.rows.flatMap((row) => row.fields.map(([, value]) => value));
                const local = corpus.cases.find(
                    (item) => item.name === proof.names[`boundary-${side}`] && item.scale === proof.scale
                );
                const decoded = recognizeBoundary(proof.id, local.text);
                assert.equal(encodeBoundary(proof.id, decoded), local.text);
                const result = parse(side === "dialect" ? "core" : "cmark", local.text);
                parses++;
                assert.equal(result.children.length, fields.length, `${proof.id}: boundary field count changed`);
                for (const [i, paragraph] of result.children.entries()) {
                    const field = fields[i];
                    assert.equal(paragraph.kind, "Paragraph");
                    if (field.grammar === "inline")
                        assert.deepEqual(
                            inlineMeaning(paragraph.children, "Strong"),
                            bodyMeaning(recognizeBody(field.source))
                        );
                    else {
                        assert.equal(paragraph.children.length, 1);
                        const node = paragraph.children[0];
                        assert.equal(node.kind, field.grammar === "empty" ? "Link" : "Code");
                        if (field.grammar !== "empty") assert.equal(node.fields.literal, field.source);
                        else assert.equal(node.children.length, 0);
                    }
                }
            }
        }
    }
    return {
        certificates: corpus.certificates.length,
        pairedDocument: corpus.certificates.filter((c) => c.scope === "paired-document-grammar").length,
        boundary: corpus.certificates.filter((c) => c.scope === "boundary-grammar").length,
        legacy: new Set(corpus.certificates.flatMap((c) => c.legacy)).size,
        parses
    };
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
    return lines.join("\n") + "\n";
}
