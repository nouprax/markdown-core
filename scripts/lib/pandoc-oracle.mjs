import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseAttributesDump, parseDestination } from "./upstream-cmark.mjs";

export const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
export const source = JSON.parse(fs.readFileSync(path.join(root, "specs/oracles/pandoc/source.json"), "utf8"));
export const pin = source.executableOracle;
export const binary = path.join(
    root,
    ".tools/pandoc",
    pin.version,
    process.platform === "win32" ? "pandoc.exe" : "pandoc"
);
export const digest = (value) =>
    createHash("sha256")
        .update(
            typeof value === "string"
                ? value
                : JSON.stringify(value, (_key, item) =>
                      item && typeof item === "object" && !Array.isArray(item)
                          ? Object.fromEntries(Object.entries(item).sort(([a], [b]) => a.localeCompare(b)))
                          : item
                  )
        )
        .digest("hex");

export function withOracle(use) {
    assert.ok(
        execFileSync(binary, ["--version"], { encoding: "utf8" }).startsWith(pin.versionPrefix),
        "Pandoc version prefix changed"
    );
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "markdown-core-pandoc-"));
    try {
        const run = (input, from, environment = process.env) => {
            assert.match(from, /^markdown_strict(?:[+-][a-z_]+)*$/);
            const result = JSON.parse(
                execFileSync(binary, ["--from", from, "--to", pin.writer, "--data-dir", directory], {
                    input,
                    encoding: "utf8",
                    env: environment,
                    maxBuffer: 64 * 1024 * 1024
                })
            );
            assert.deepEqual(result["pandoc-api-version"], pin.pandocApiVersion, "Pandoc API envelope changed");
            assert.deepEqual(result.meta, {}, "unexpected oracle metadata");
            assert.ok(Array.isArray(result.blocks), "missing Pandoc blocks");
            assert.deepEqual(fs.readdirSync(directory), [], "oracle data directory is no longer empty");
            return result;
        };
        return use(run);
    } finally {
        fs.rmSync(directory, { recursive: true, force: true });
    }
}

const node = (kind, fields = {}, children = [], attr = ["", [], []]) => ({
    kind,
    anchor: attr[0] || null,
    attributes: { classes: attr[1], records: attr[2].map(([name, value]) => ({ name, value })) },
    ...fields,
    children
});
function append(children, child) {
    const previous = children.at(-1);
    if (child.kind === "Text" && previous?.kind === "Text") previous.literal += child.literal;
    else if (child.kind !== "Text" || child.literal !== "") children.push(child);
}
const text = (literal) => node("Text", { literal });
const inlineKinds = {
    Emph: "Emphasis",
    Strong: "Strong",
    Strikeout: "Strikethrough",
    Superscript: "Superscript",
    Subscript: "Subscript",
    SmallCaps: "SmallCaps"
};
const alignment = (value) =>
    ({ AlignDefault: "none", AlignLeft: "left", AlignCenter: "center", AlignRight: "right" })[value.t];
const variant = (value) =>
    ({
        Decimal: "decimal",
        LowerAlpha: "alpha(lowercased=true)",
        UpperAlpha: "alpha(lowercased=false)",
        LowerRoman: "roman(lowercased=true)",
        UpperRoman: "roman(lowercased=false)",
        Example: "example",
        DefaultStyle: "default"
    })[value.t];
const delimiter = (value) =>
    ({
        Period: "period",
        OneParen: "parenthesis(closed=false)",
        TwoParens: "parenthesis(closed=true)",
        DefaultDelim: "default"
    })[value.t];
function sequence(values) {
    const result = [];
    for (const value of values) append(result, pandocNode(value));
    return result;
}
function row(value) {
    return node(
        "TableRow",
        {},
        value[1].map((cell) => node("TableCell", { rowspan: cell[2], colspan: cell[3] }, sequence(cell[4]), cell[0])),
        value[0]
    );
}
function pandocNode({ t, c }) {
    if (inlineKinds[t]) return node(inlineKinds[t], {}, sequence(c));
    switch (t) {
        case "Str":
            return text(c);
        case "Space":
            return text(" ");
        case "SoftBreak":
            return node("SoftBreak");
        case "LineBreak":
            return node("LineBreak");
        case "Para":
        case "Plain":
            return node("Paragraph", {}, sequence(c));
        case "Header":
            return node("Heading", { level: c[0] }, sequence(c[2]), c[1]);
        case "Code":
            return node("Code", { literal: c[1] }, [], c[0]);
        case "CodeBlock":
            return node("CodeBlock", { literal: c[1] }, [], c[0]);
        case "Link":
        case "Image":
            return node(
                t === "Link" ? "Link" : "Media",
                { dest: { kind: "url", value: c[2][0] }, title: c[2][1] || null },
                sequence(c[1]),
                c[0]
            );
        case "Span":
            return node("Span", {}, sequence(c[1]), c[0]);
        case "Div":
            return node("DirectiveBlock", { name: "" }, sequence(c[1]), c[0]);
        case "BlockQuote":
            return node("Callout", { variant: null, collapsed: null }, sequence(c));
        case "HorizontalRule":
            return node("ThematicBreak");
        case "RawInline":
        case "RawBlock":
            return node(t === "RawInline" ? "HTML" : "HTMLBlock", { format: c[0], literal: c[1] });
        case "Math":
            return node(c[0].t === "DisplayMath" ? "FormulaBlock" : "Formula", { literal: c[1] });
        case "BulletList":
            return node(
                "List",
                {
                    flavor: "bullet",
                    start: null,
                    variant: null,
                    delimiter: null,
                    tight: c.every((item) => item.every((block) => block.t !== "Para"))
                },
                c.map((item) => node("ListItem", {}, sequence(item)))
            );
        case "OrderedList":
            return node(
                "List",
                {
                    flavor: "ordered",
                    start: c[0][0],
                    variant: variant(c[0][1]),
                    delimiter: delimiter(c[0][2]),
                    tight: c[1].every((item) => item.every((block) => block.t !== "Para"))
                },
                c[1].map((item) => node("ListItem", {}, sequence(item)))
            );
        case "DefinitionList":
            return node(
                "DefinitionList",
                {},
                c.map(([term, definitions]) =>
                    node("DefinitionItem", {}, [
                        node("DefinitionTerm", {}, sequence(term)),
                        ...definitions.map((blocks) => node("DefinitionBody", {}, sequence(blocks)))
                    ])
                )
            );
        case "Cite":
            return node(
                "Cite",
                {
                    citations: c[0].map((value) => ({
                        key: value.citationId,
                        mode: value.citationMode.t,
                        prefix: sequence(value.citationPrefix),
                        suffix: sequence(value.citationSuffix)
                    }))
                },
                sequence(c[1])
            );
        case "Note":
            return node("Footnote", {}, sequence(c));
        case "Table":
            return node(
                "Table",
                {
                    columns: c[2].map(([align, width]) => ({
                        alignment: alignment(align),
                        relative: width.t === "ColWidthDefault" ? null : width.c
                    }))
                },
                [
                    ...(c[1][1].length ? [node("TableCaption", {}, sequence(c[1][1]))] : []),
                    node("TableHead", {}, c[3][1].map(row)),
                    node(
                        "TableBody",
                        {},
                        c[4].flatMap((body) => [...body[2], ...body[3]].map(row))
                    ),
                    node("TableFoot", {}, c[5][1].map(row))
                ],
                c[0]
            );
        default:
            throw new Error(`unmapped Pandoc constructor: ${t}`);
    }
}
export function fromPandoc(value) {
    return node("Document", {}, sequence(value.blocks));
}

// Pandoc cannot distinguish absent/empty titles, authored code-info from the
// first code class, or a code block's final source newline. These are the
// declared representation projections; product scopes remain fixture-owned.
export function fromCanonical(value) {
    const f = value.fields;
    const optional = (name) => (value.tokens[name] === undefined ? null : JSON.parse(value.tokens[name]));
    const children = [];
    for (const child of value.children) append(children, fromCanonical(child));
    const attrs = parseAttributesDump(f.attributes);
    const result = { kind: value.kind, anchor: optional("anchor"), attributes: attrs, children };
    if (["Code", "Text", "HTML", "HTMLBlock", "Comment", "Formula", "FormulaBlock"].includes(value.kind))
        result.literal = f.literal;
    if (value.kind === "CodeBlock") {
        result.literal = f.literal.replace(/\n$/, "");
        if (optional("language") !== null) attrs.classes = [optional("language"), ...attrs.classes];
    }
    if (value.kind === "Heading") result.level = Number(f.level);
    if (["Link", "Media", "CrossLink", "CrossEmbedded"].includes(value.kind)) {
        result.dest = parseDestination(f.dest);
        if (value.kind === "Link" || value.kind === "Media") result.title = optional("title") || null;
        else result.label = f.label;
    }
    if (value.kind === "Callout") {
        result.variant = optional("variant");
        result.collapsed = optional("collapsed");
    }
    if (value.kind === "List")
        for (const key of ["flavor", "start", "variant", "delimiter", "tight"])
            result[key] =
                f[key] === "null"
                    ? null
                    : key === "tight"
                      ? f[key] === "true"
                      : key === "start"
                        ? Number(f[key])
                        : f[key];
    if (value.kind === "Table") {
        assert.match(
            f.columns,
            /^\[(?:(?:none|left|center|right):(?:null|[0-9.e+-]+)(?:,(?:none|left|center|right):(?:null|[0-9.e+-]+))*)?\]$/
        );
        result.columns =
            f.columns.slice(1, -1) === ""
                ? []
                : f.columns
                      .slice(1, -1)
                      .split(",")
                      .map((column) => {
                          const [alignment, width] = column.split(":");
                          return { alignment, relative: width === "null" ? null : Number(width) };
                      });
    }
    if (value.kind === "TableCell") {
        result.rowspan = Number(f.rowspan);
        result.colspan = Number(f.colspan);
    }
    if (value.kind === "Directive" || value.kind === "DirectiveBlock") result.name = f.name;
    return result;
}

export function assertCanaries(run) {
    const from = "markdown_strict+inline_code_attributes";
    const input = "`你好🧭`{#id .same .same k=1 k=2}\n";
    const expected = node("Document", {}, [
        node("Paragraph", {}, [
            node(
                "Code",
                { literal: "你好🧭" },
                [],
                [
                    "id",
                    ["same", "same"],
                    [
                        ["k", "1"],
                        ["k", "2"]
                    ]
                ]
            )
        ])
    ]);
    assert.deepEqual(fromPandoc(run(input, from)), expected);
    assert.notDeepEqual(
        fromPandoc(run(input, "markdown_strict-inline_code_attributes")),
        expected,
        "extension disable ignored"
    );
    assert.throws(() => run(input, "markdown"), /markdown_strict/);
    const poison = fs.mkdtempSync(path.join(os.tmpdir(), "markdown-core-pandoc-user-"));
    try {
        fs.mkdirSync(path.join(poison, "pandoc"));
        fs.writeFileSync(path.join(poison, "pandoc", "defaults.yaml"), "invalid: [\n");
        assert.deepEqual(
            run(input, from, { ...process.env, XDG_DATA_HOME: poison, PANDOC_DATA_DIR: poison }),
            run(input, from)
        );
    } finally {
        fs.rmSync(poison, { recursive: true, force: true });
    }
    const merge = fromPandoc(
        run("[x][r]{.b .c k=3}\n\n[r]: /t {#d .a .b k=1 m=2}\n", "markdown_strict+link_attributes")
    );
    const link = merge.children[0].children[0];
    assert.deepEqual(
        link.attributes,
        {
            classes: ["b", "c", "a"],
            records: [
                { name: "m", value: "2" },
                { name: "k", value: "3" }
            ]
        },
        "Pandoc combineAttr behavior changed"
    );
    const anchors = "markdown_strict+auto_identifiers+gfm_auto_identifiers";
    const headingAnchors = (input, from = anchors) =>
        fromPandoc(run(input, from)).children.map((value) => value.anchor);
    assert.deepEqual(headingAnchors("# !!!\n"), ["section"]);
    assert.deepEqual(headingAnchors("# x\n\n# Name {#x}\n", anchors + "+header_attributes"), ["x", "x"]);
    assert.deepEqual(headingAnchors("# a‿b ∑ 😀\n"), ["a‿b--grinning"]);
    assert.deepEqual(headingAnchors("# İ ſ ẞ Σ ς\n"), ["i̇-ſ-ß-σ-ς"]);
    assert.deepEqual(headingAnchors("# A\u0085B\u2028C\n"), ["abc"]);
    const adjacent = fromPandoc(run("# H\n\n[H] [H]\n", anchors + "+implicit_header_references"));
    assert.equal(adjacent.children[1].children.length, 1, "Pandoc permits whitespace before a full reference tail");
    assert.equal(adjacent.children[1].children[0].dest.value, "#h");
}

export function validatePolicy(policy, cases) {
    assert.equal(policy.schemaVersion, 1);
    const ids = new Set(cases.map((value) => value.id));
    assert.equal(ids.size, cases.length, "duplicate corpus id");
    const entries = new Map();
    for (const entry of policy.entries) {
        assert.ok(ids.has(entry.id) && !entries.has(entry.id), `unknown/duplicate delta: ${entry.id}`);
        assert.ok(["gap", "projection"].includes(entry.status));
        assert.match(entry.item, /^P(?:[2-9][a-d]?|1[012][a-d]?)$/);
        assert.ok(entry.reason.length > 20);
        for (const key of ["inputDigest", "oracleDigest", "coreDigest"]) assert.match(entry[key], /^[a-f0-9]{64}$/);
        entries.set(entry.id, entry);
    }
    return entries;
}
export function verifyComparison(testCase, expected, actual, entry) {
    if (digest(expected) === digest(actual)) {
        assert.equal(entry, undefined, `stale delta: ${testCase.id}`);
        return false;
    }
    assert.ok(entry, `unregistered Pandoc difference: ${testCase.id}`);
    assert.equal(entry.inputDigest, digest([testCase.from, testCase.input]), `changed input: ${testCase.id}`);
    assert.equal(entry.oracleDigest, digest(expected), `changed oracle: ${testCase.id}`);
    assert.equal(entry.coreDigest, digest(actual), `changed core: ${testCase.id}`);
    return true;
}
