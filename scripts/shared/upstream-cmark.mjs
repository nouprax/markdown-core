/**
 * Upstream-parity normalization.
 *
 * Markdown Core descends from cmark-gfm, and its Markdown semantics are meant
 * to stay identical to upstream's apart from a small, enumerated set of
 * deliberate differences. Nothing in the repository checked that claim until
 * this module: the spec fixtures' expected blocks are canonical AST dumps this
 * parser generated itself, so they pin "do not change" rather than "agrees
 * with upstream".
 *
 * Both parsers' output is normalized into one comparable tree, so a divergence
 * is either a registered difference (`specs/oracles/cmark-gfm/deltas.json`) or a
 * defect. The two ASTs are not the same shape, so the mapping below is
 * load-bearing and each rule is a claim about equivalence, not a convenience.
 */

import { Buffer } from "node:buffer";
import fs from "node:fs";

/** cmark-gfm XML element -> canonical node kind. */
const XML_KIND = {
    document: "Document",
    heading: "Heading",
    paragraph: "Paragraph",
    text: "Text",
    emph: "Emphasis",
    strong: "Strong",
    code: "Code",
    code_block: "CodeBlock",
    html_block: "HTMLBlock",
    html_inline: "HTML",
    link: "Link",
    image: "Embedded",
    list: "List",
    item: "ListItem",
    tasklist: "ListItem",
    block_quote: "Callout",
    thematic_break: "ThematicBreak",
    softbreak: "SoftBreak",
    linebreak: "LineBreak",
    strikethrough: "Strikethrough",
    table: "Table",
    table_header: "TableRow",
    table_row: "TableRow",
    table_cell: "TableCell"
};

/**
 * Fields compared per kind. A field absent here is not compared, and every
 * omission is deliberate: upstream's XML writer does not emit table column
 * alignments, a fenced-vs-indented code flag, a fence-closed flag, an inline
 * code placement mode, or a footnote label, so there is nothing on that side
 * to compare against. Those fields are pinned by this repository's own golden
 * dumps instead, and the gap is recorded in specs/oracles/cmark-gfm/README.md.
 */
const COMPARED = {
    Callout: ["variant", "collapsed"],
    Heading: ["level"],
    List: ["flavor", "start", "variant", "delimiter", "tight"],
    ListItem: ["completed"],
    CodeBlock: ["info", "literal"],
    Code: ["literal"],
    Text: ["literal"],
    HTML: ["literal"],
    HTMLBlock: ["literal"],
    Comment: ["literal"],
    Link: ["dest", "title"],
    Embedded: ["dest", "title"],
    TableCell: ["rowspan", "colspan"]
};

const LITERAL_BEARING = new Set(["text", "code", "code_block", "html_block", "html_inline"]);

function unescapeXml(text) {
    return text
        .replace(/&lt;/g, "<")
        .replace(/&gt;/g, ">")
        .replace(/&quot;/g, '"')
        .replace(/&#(\d+);/g, (_, digits) => String.fromCodePoint(Number(digits)))
        .replace(/&amp;/g, "&");
}

/**
 * Parses `cmark-gfm --to xml`.
 *
 * Upstream's XML writer has no element name for the footnote extension's
 * nodes and emits `<unknown>` for both of them. A footnote reference is a leaf
 * and a definition is not, which is what tells them apart here. That is a
 * property of upstream's writer rather than of Markdown, so if a future
 * upstream grows more unknown node kinds this heuristic stops being sound —
 * `assertNoUnknownKinds` below is what catches that rather than letting it
 * silently mis-label.
 */
export function parseUpstreamXml(xml, fired) {
    const body = xml.replace(/^[\s\S]*?<document[^>]*>/, "").replace(/<\/document>\s*$/, "");
    const root = { kind: "Document", fields: {}, children: [] };
    const stack = [root];
    const token = /<(\/?)([a-z_]+|<unknown>)((?:\s+[a-z:_]+="[^"]*")*)\s*(\/?)>|([^<]+)/g;
    for (let match = token.exec(body); match; match = token.exec(body)) {
        if (match[5] !== undefined) {
            const top = stack[stack.length - 1];
            if (top.pendingText !== undefined) top.pendingText += match[5];
            continue;
        }
        const [, closing, name, attrText, selfClose] = match;
        if (closing) {
            const node = stack.pop();
            if (node.pendingText !== undefined) node.fields.literal = unescapeXml(node.pendingText);
            continue;
        }
        const attributes = {};
        for (const attr of attrText.matchAll(/([a-z:_]+)="([^"]*)"/g)) {
            if (attr[1] !== "xml:space") attributes[attr[1]] = unescapeXml(attr[2]);
        }
        let kind = XML_KIND[name];
        // The footnote extension's two nodes (M4): a call is a one-item `Cite`
        // whose `Citation` carries empty affix groups, and a definition is a
        // `Footnote` node; upstream states no label for either, so neither
        // carries a compared field.
        if (name === "<unknown>") kind = selfClose ? "Cite" : "Footnote";
        if (kind === undefined) kind = `?${name}`;
        const node = { kind, fields: attributes, children: [] };
        if (kind === "Cite") node.children.push(citationItem({}));
        if (name === "table_cell") {
            node.fields.rowspan = "1";
            node.fields.colspan = "1";
        }
        if (name === "item") {
            node.fields.completed = "null";
        }
        // Every `>` container is a `Callout` (M3), and an inherited quote is
        // metadata-free: cmark has no callout metadata to state, so the
        // projection states the absence the canonical AST prints.
        if (name === "block_quote") {
            node.fields.variant = "null";
            node.fields.collapsed = "null";
        }
        // cmark states a link's or image's target as one string; the canonical
        // AST states it as the `url` branch of `Destination` (M1).
        if (name === "link" || name === "image") node.fields.dest = urlDestination(attributes.destination ?? "");
        if (LITERAL_BEARING.has(name)) node.pendingText = "";
        stack[stack.length - 1].children.push(node);
        if (!selfClose) stack.push(node);
        else if (node.pendingText !== undefined) node.fields.literal = "";
    }
    const pending = [root];
    while (pending.length) {
        const node = pending.pop();
        pending.push(...node.children);
        if (node.kind === "Table") node.children = tableGroups(node.children, fired);
    }
    return root;
}

/** The inherited pipe-table shape, used only to project external oracles. */
export function tableGroups(rows, fired) {
    fired?.add("table-row-groups");
    return [
        { kind: "TableHead", fields: {}, children: rows.slice(0, 1) },
        { kind: "TableBody", fields: {}, children: rows.slice(1) },
        { kind: "TableFoot", fields: {}, children: [] }
    ];
}

/**
 * Parses fields from one canonical-dump line in a single forward pass.
 *
 * Field values may be JSON strings, bracketed sequences, or tagged values
 * such as `cross(path="Note", anchor="Heading one")`. Whitespace terminates a
 * value only outside quotes and balanced delimiters. Treating every unquoted
 * value as one non-whitespace token truncates structured values and makes an
 * oracle compare printer spacing instead of semantics.
 */
function parseCanonicalFieldTokens(body) {
    const fields = {};
    let cursor = body.indexOf(" ");
    if (cursor < 0) return fields;

    while (cursor < body.length) {
        while (body[cursor] === " ") cursor++;
        const nameStart = cursor;
        while (/[a-zA-Z]/.test(body[cursor] ?? "")) cursor++;
        if (cursor === nameStart || body[cursor] !== "=") break;
        const name = body.slice(nameStart, cursor++);
        const valueStart = cursor;
        let parentheses = 0;
        let brackets = 0;
        let braces = 0;
        let quoted = false;
        let escaped = false;

        while (cursor < body.length) {
            const character = body[cursor];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character === "\\") escaped = true;
                else if (character === '"') quoted = false;
            } else if (character === '"') {
                quoted = true;
            } else if (character === "(") {
                parentheses++;
            } else if (character === ")") {
                parentheses--;
            } else if (character === "[") {
                brackets++;
            } else if (character === "]") {
                brackets--;
            } else if (character === "{") {
                braces++;
            } else if (character === "}") {
                braces--;
            } else if (/\s/.test(character) && parentheses === 0 && brackets === 0 && braces === 0) {
                break;
            }
            cursor++;
        }

        const value = body.slice(valueStart, cursor);
        fields[name] = value;
    }
    return fields;
}

// Lexical tokens retain distinctions the legacy text projections cannot
// represent, notably the absent marker null versus the authored string "null".
function decodeCanonicalFields(tokens) {
    return Object.fromEntries(
        Object.entries(tokens).map(([name, value]) => [name, value.startsWith('"') ? JSON.parse(value) : value])
    );
}
export function parseCanonicalFields(body) {
    return decodeCanonicalFields(parseCanonicalFieldTokens(body));
}

/** The `url` branch of a `Destination`: the target as one decoded string. */
export function urlDestination(value) {
    return { kind: "url", value };
}

/** The `cross` branch of a `Destination`: a workspace path, possibly empty, and an anchor or null. */
export function crossDestination(path, anchor) {
    return { kind: "cross", path, anchor };
}

/** The `reference` branch of a `Destination`: the normalized label a reference names. */
export function referenceDestination(label) {
    return { kind: "reference", label };
}

function scanJsonString(text, start) {
    if (text[start] !== '"') return null;
    for (let cursor = start + 1; cursor < text.length; cursor++) {
        if (text[cursor] === "\\") {
            cursor++;
        } else if (text[cursor] === '"') {
            try {
                return { value: JSON.parse(text.slice(start, cursor + 1)), end: cursor + 1 };
            } catch {
                return null;
            }
        }
    }
    return null;
}

/**
 * Parses the dump's spelling of a `Destination` — `url("...")` or
 * `cross(path="...",anchor="..."|null)`, whitespace allowed between tokens —
 * into the object the oracle mappings build. Anything else is null, so a
 * caller compares a real value or fails, never a spelling.
 */
export function parseDestination(raw) {
    const text = String(raw);
    let cursor = 0;
    const skipSpace = () => {
        while (/\s/.test(text[cursor] ?? "")) cursor++;
    };
    const consume = (token) => {
        skipSpace();
        if (!text.startsWith(token, cursor)) return false;
        cursor += token.length;
        return true;
    };
    const string = () => {
        skipSpace();
        const parsed = scanJsonString(text, cursor);
        if (parsed) cursor = parsed.end;
        return parsed?.value;
    };
    const complete = () => {
        skipSpace();
        return cursor === text.length;
    };

    if (consume("url(")) {
        const value = string();
        if (value === undefined || !consume(")") || !complete()) return null;
        return urlDestination(value);
    }

    cursor = 0;
    if (consume("reference(")) {
        const label = string();
        if (label === undefined || !consume(")") || !complete()) return null;
        return referenceDestination(label);
    }

    cursor = 0;
    if (!consume("cross(") || !consume("path=")) return null;
    const path = string();
    if (path === undefined || !consume(",") || !consume("anchor=")) return null;
    skipSpace();
    let anchor;
    if (text.startsWith("null", cursor)) {
        cursor += 4;
        anchor = null;
    } else {
        anchor = string();
        if (anchor === undefined) return null;
    }
    if (!consume(")") || !complete()) return null;
    return crossDestination(path, anchor);
}

/** One `Citation` item as the dump nests it: a node line with the given
 * fields, then its `CitationPrefix` and `CitationSuffix` groups. */
export function citationItem(fields, prefix = [], suffix = []) {
    return {
        kind: "Citation",
        fields,
        children: [
            { kind: "CitationPrefix", fields: {}, children: prefix },
            { kind: "CitationSuffix", fields: {}, children: suffix }
        ]
    };
}

/** The canonical dump spelling of a `Destination`: the one form both sides compare. */
export function renderDestination(destination) {
    if (destination.kind === "url") return `url(${JSON.stringify(destination.value)})`;
    if (destination.kind === "reference") return `reference(${JSON.stringify(destination.label)})`;
    const anchor = destination.anchor === null ? "null" : JSON.stringify(destination.anchor);
    return `cross(path=${JSON.stringify(destination.path)},anchor=${anchor})`;
}

/** Parses this repository's canonical AST dump. */
export function parseCanonicalDump(dump) {
    const lines = dump.split("\n").filter((line) => line.trim().length);
    const tokens = parseCanonicalFieldTokens(lines[0] ?? "Document");
    const root = { kind: "Document", fields: decodeCanonicalFields(tokens), tokens, children: [] };
    const byDepth = [root];
    for (const line of lines.slice(1)) {
        const marker = line.search(/[├└]/);
        const depth = marker < 0 ? 1 : marker / 4 + 1;
        const body = marker < 0 ? line.trim() : line.slice(marker + 4).trim();
        const tokens = parseCanonicalFieldTokens(body);
        const node = { kind: body.split(" ")[0], fields: decodeCanonicalFields(tokens), tokens, children: [] };
        byDepth[depth - 1].children.push(node);
        byDepth[depth] = node;
    }
    return root;
}

export function normalize(node, side, fired) {
    const children = [];
    for (const child of node.children) {
        const normalized = normalize(child, side, fired);
        const previous = children[children.length - 1];
        // The two parsers split runs of text at different points around
        // entities, escapes, and extension boundaries. The character content
        // is what the comparison is about, so adjacent text is joined on both
        // sides before comparing.
        if (previous && previous.kind === "Text" && normalized.kind === "Text") {
            previous.fields.literal += normalized.fields.literal;
        } else {
            children.push(normalized);
        }
    }
    // `empty-text-node`, Q38. A `Text` that owns no bytes is dropped from BOTH
    // sides. Upstream emits one wherever its autolink split or its hard-break
    // stripping leaves a fragment with nothing in it; this repository stopped
    // emitting them at 0a.14, because a child with no literal and no source is
    // not a node. Projecting it away rather than registering eleven inputs is
    // the right shape: it is a MODEL difference -- it appears wherever the
    // construct does -- and a list of inputs would go stale the moment the
    // corpus grew. Dropped after the run-joining above, so a merged run that
    // came out empty goes too.
    const kept = children.filter((child) => !(child.kind === "Text" && child.fields.literal === ""));
    if (kept.length !== children.length) fired?.add("empty-text-node");
    children.length = 0;
    children.push(...kept);
    const fields = {};
    for (const key of COMPARED[node.kind] ?? []) {
        let value = node.fields[key];
        if (node.kind === "ListItem" && key === "completed") {
            value = taskCompletion(node.fields);
            if (value !== "null") fired?.add("task-marker-completion");
        }
        if (side === "upstream") {
            if (node.kind === "List" && key === "flavor") value = node.fields.type;
            if (node.kind === "List" && key === "variant") value = node.fields.type === "ordered" ? "decimal" : "null";
            if (node.kind === "List" && key === "delimiter") {
                value =
                    node.fields.type === "ordered"
                        ? node.fields.delim === "paren"
                            ? "parenthesis(closed=false)"
                            : "period"
                        : "null";
            }
            if (node.kind === "List" && key === "tight") value = node.fields.tight ?? "false";
            if (node.kind === "CodeBlock" && key === "info") value = node.fields.info ?? "null";
        }
        // `dest` is a tagged value on both sides: the object the oracle mapping
        // built, or the dump's `url("...")` text. One spelling is compared, so
        // the comparison is of the value and not of a printer.
        if (key === "dest") {
            const destination = typeof value === "string" ? parseDestination(value) : value;
            if (!destination) throw new Error(`invalid destination on ${node.kind}: ${String(value)}`);
            fields.dest = renderDestination(destination);
            continue;
        }
        if (value === undefined || value === "") value = key === "literal" ? "" : "null";
        if (key === "title" && value === "") value = "null";
        fields[key] = String(value);
    }
    // Upstream omits an ordered list's start when it is 1; this repository
    // always prints it.
    if (node.kind === "List" && fields.start === "null" && fields.flavor === "ordered") fields.start = "1";
    return { kind: node.kind, fields, children };
}

/**
 * Registered delta `html-comment-node`: an HTML comment is a `Comment` node of
 * the dialect, and cmark keeps it as an html node holding the token as
 * written. Upstream's tree has those nodes mapped here by the rule the engine
 * applies on its own side (M0, `docs/specs/dialect/comments.md`):
 *
 *   - an `html_inline` whose literal opens with `<!--` is the comment token,
 *     and its literal is the bytes between `<!--` and `-->`; `<!-->` and
 *     `<!--->` are the two tokens the grammar names as comments with nothing
 *     inside, so their literal is empty;
 *   - an `html_block` whose literal opens with `<!--`, after the block's own
 *     indentation, and whose end line holds only whitespace after the first
 *     `-->` is a block comment with the bytes between the delimiters, line
 *     endings included; the `-->` is searched from two bytes into the opener,
 *     which is what makes `<!-->` and `<!--->` empty comments as blocks too.
 *     Every other block stays `HTMLBlock` as written.
 *
 * A MODEL difference, so a projection rather than a list of inputs: it appears
 * wherever a comment does, in the fuzzed inputs too.
 */
export function projectHtmlComments(root, fired) {
    const rewrite = (node) => {
        for (const child of node.children) {
            const literal = child.fields.literal ?? "";
            if (child.kind === "HTML" && literal.startsWith("<!--")) {
                child.kind = "Comment";
                child.fields.literal = literal.length > 6 ? literal.slice(4, -3) : "";
                fired?.add("html-comment-node");
            } else if (child.kind === "HTMLBlock") {
                const body = blockCommentBody(literal);
                if (body !== null) {
                    child.kind = "Comment";
                    child.fields.literal = body;
                    fired?.add("html-comment-node");
                }
            }
            rewrite(child);
        }
        return node;
    };
    return rewrite(root);
}

/** The block comment's body, or null when the block is not a comment. */
export function blockCommentBody(literal) {
    const open = literal.search(/[^ \t]/);
    if (open < 0 || !literal.startsWith("<!--", open)) return null;
    const close = literal.indexOf("-->", open + 2);
    if (close < 0) return null;
    if (!/^[ \t]*(?:\r?\n)?$/.test(literal.slice(close + 3))) return null;
    return close > open + 4 ? literal.slice(open + 4, close) : "";
}

/**
 * Registered delta `footnote-definition-placement`: upstream moves every
 * footnote definition to the document tail in first-reference order, while
 * this repository keeps a definition as a `Footnote` block in the content
 * where it was written (canonical-ast.md, M4). Both sides therefore have
 * their definitions lifted out and re-attached in one deterministic order,
 * which compares their *content* while deliberately not comparing their
 * position.
 *
 * An inline note is not a definition: it is the `Footnote` its `Citation`
 * owns, so it stays under that citation and is compared where it was written.
 */
export function liftFootnotes(root, fired) {
    const definitions = [];
    const strip = (node) => {
        node.children = node.children.filter((child) => {
            if (child.kind === "Footnote" && node.kind !== "Citation") {
                definitions.push(child);
                return false;
            }
            strip(child);
            return true;
        });
        return node;
    };
    strip(root);
    for (const definition of definitions) strip(definition);
    definitions.sort((left, right) => (render(left) < render(right) ? -1 : 1));
    root.children.push(...definitions);
    if (definitions.length > 0) fired?.add("footnote-definition-placement");
    return root;
}

/**
 * Registered delta `footnote-resolution-model`: upstream unlinks a footnote
 * definition nothing refers to. This repository keeps it — an unreferenced
 * definition is text the author wrote, and remark keeps it too.
 *
 * This once also covered unresolved *references*: this side kept them as nodes
 * where upstream rewrote them to literal text. That half is gone as of
 * 2026-08-02 — an unresolved footnote reference degrades to text here as well,
 * so both authorities and this repository now agree and there is nothing left
 * to project. What remains is the retention half, applied to a tree from this
 * side so the gate compares the resolved language rather than two
 * representations of retention. An inline note is owned by the citation that
 * refers to it, so only definitions are candidates.
 */
export function applyUpstreamFootnoteModel(root, fired) {
    // Both a definition's `Footnote.label` and a `footnote(label=...)`
    // referent's label are the label under the reference map's own
    // normalization (M4), so they compare directly.
    const referenced = new Set();
    const survey = (node) => {
        const referent = /^footnote\(label=("(?:\\.|[^"\\])*")\)$/.exec(node.fields.referent ?? "");
        if (node.kind === "Citation" && referent) referenced.add(JSON.parse(referent[1]));
        for (const child of node.children) survey(child);
    };
    survey(root);

    const rewrite = (node) => {
        const before = node.children.length;
        if (node.kind !== "Citation") {
            node.children = node.children.filter(
                (child) => !(child.kind === "Footnote" && !referenced.has(child.fields.label))
            );
        }
        if (node.children.length !== before) fired?.add("footnote-resolution-model");
        for (const child of node.children) rewrite(child);
        return node;
    };
    return rewrite(root);
}

/**
 * The engine's full case fold, read from its own fold data
 * (`core/case_fold.inc`): code point -> its fold's image.
 */
let caseFolds;
function caseFold() {
    if (caseFolds) return caseFolds;
    const data = fs.readFileSync(new URL("../../packages/markdown-core/core/case_fold.inc", import.meta.url), "utf8");
    const numbers = (name) =>
        new RegExp(`${name}\\[\\d+\\] = \\{([^}]*)\\}`, "u")
            .exec(data)[1]
            .split(",")
            .map((value) => value.trim())
            .filter(Boolean)
            .map(Number);
    const replacements = Buffer.from(numbers("cf_repl"));
    caseFolds = new Map(
        numbers("cf_table").map((entry) => {
            const at = ((entry >>> 17) & 0xfff) * 2;
            return [entry & 0x1ffff, replacements.subarray(at, at + (entry >>> 29)).toString("utf8")];
        })
    );
    return caseFolds;
}

/**
 * A label under the reference-label normalization: runs of spaces, tabs and
 * line ends become one space, the ends are trimmed, and each character takes
 * its full case fold.
 */
function normalizeLabel(label) {
    const folds = caseFold();
    return Array.from(label.replace(/[ \t\r\n]+/gu, " ").replace(/^ | $/gu, ""), (character) => {
        const code = character.codePointAt(0);
        return code < 0x80 ? character.toLowerCase() : (folds.get(code) ?? character);
    }).join("");
}

/**
 * Registered delta `reference-definition-node`: upstream consumes a link
 * reference definition and copies it into every `Link` or `Embedded` that
 * resolves to it, while this repository keeps the definition as a
 * `Reference` block where it was written and has each occurrence name its
 * label (`dest=reference("...")`), which the document resolves. A tree from
 * this side is projected onto upstream's shape: each reference destination
 * that resolves takes its target's destination, and every `Reference` leaves
 * the tree. A reference that resolved to the wrong definition, or to none,
 * still shows up as a plain difference in `dest`, `title`, or kind.
 *
 * The label resolves as the document resolves it: to the first `Reference`
 * in source order with that label, whose url and title the occurrence takes,
 * with the definition's classes and records ahead of the occurrence's and its
 * anchor where the occurrence has none; or, when no `Reference` states it, to
 * the first `Heading` whose text declares it, whose `#anchor` the occurrence
 * takes with no title. A heading's text is the `source` of its own ranges
 * that its content spans, under the reference-label normalization.
 */
export function resolveReferences(root, source, fired) {
    // Lines end at LF, CR, or CRLF, as the parser's do.
    const lines = Buffer.from(source, "utf8")
        .toString("latin1")
        .split(/\r\n|\r|\n/u)
        .map((line) => Buffer.from(line, "latin1"));
    const point = (text) => text.split(":").map(Number);
    const before = ([a, b], [c, d]) => a < c || (a === c && b < d);
    // The bytes from `start` to `end`, both points, inclusive.
    const spanned = ([startLine, startColumn], [endLine, endColumn]) => {
        const parts = [];
        for (let line = startLine; line <= endLine; line++) {
            const bytes = lines[line - 1] ?? Buffer.alloc(0);
            const from = line === startLine ? startColumn - 1 : 0;
            const to = line === endLine ? endColumn : bytes.length;
            parts.push(bytes.subarray(from, to).toString("utf8"));
        }
        return parts.join("\n");
    };
    // A heading's text: the bytes of its own scope's ranges, which leave out
    // container prefixes, from where its first child starts to where its
    // last child ends.
    const headingText = (heading) => {
        const start = point(heading.children[0].tokens.scope.split("..")[0]);
        const end = point(heading.children.at(-1).tokens.scope.split(",").at(-1).split("..")[1]);
        return heading.tokens.scope
            .split(",")
            .map((range) => range.split("..").map(point))
            .map(([from, to]) => [before(from, start) ? start : from, before(end, to) ? end : to])
            .filter(([from, to]) => !before(to, from))
            .map(([from, to]) => spanned(from, to))
            .join("\n");
    };
    const fold = (label) => normalizeLabel(label);
    const definitions = new Map();
    const headings = new Map();
    const survey = (node) => {
        if (node.kind === "Reference" && !definitions.has(node.fields.label)) definitions.set(node.fields.label, node);
        if (node.kind === "Heading" && node.children.length > 0) {
            const key = fold(headingText(node));
            if (key && !headings.has(key)) headings.set(key, node);
        }
        for (const child of node.children) survey(child);
    };
    survey(root);
    const rewrite = (node) => {
        const destination =
            node.kind === "Link" || node.kind === "Embedded" ? parseDestination(node.fields.dest) : null;
        if (destination?.kind === "reference") {
            const definition = definitions.get(destination.label);
            const heading = definition ? undefined : headings.get(fold(destination.label));
            if (definition) {
                const inner = (attributes) => attributes.slice(1, -1);
                node.fields.dest = definition.fields.dest;
                node.fields.title = definition.fields.title;
                node.fields.attributes = `{${[inner(definition.fields.attributes), inner(node.fields.attributes)]
                    .filter(Boolean)
                    .join(" ")}}`;
                if (node.fields.anchor === "null") node.fields.anchor = definition.fields.anchor;
            } else if (heading) {
                node.fields.dest = renderDestination(urlDestination(`#${heading.fields.anchor}`));
            }
            fired?.add("reference-definition-node");
        }
        const before = node.children.length;
        node.children = node.children.filter((child) => child.kind !== "Reference");
        if (node.children.length !== before) fired?.add("reference-definition-node");
        for (const child of node.children) rewrite(child);
        return node;
    };
    return rewrite(root);
}

export function render(node, indent = "") {
    const fields = Object.entries(node.fields)
        .map(([key, value]) => `${key}=${JSON.stringify(value)}`)
        .join(" ");
    return [
        `${indent}${node.kind}${fields ? ` ${fields}` : ""}`,
        ...node.children.map((c) => render(c, `${indent}  `))
    ].join("\n");
}

/**
 * A node kind neither side's mapping recognized. Left unchecked this reads as
 * agreement — two unmapped kinds both render as `?name` and compare equal — so
 * an unmapped kind is an error, not a difference.
 */
export function unknownKinds(node, found = new Set()) {
    if (node.kind.startsWith("?")) found.add(node.kind.slice(1));
    for (const child of node.children) unknownKinds(child, found);
    return found;
}

/** The completion fact shared by boolean-only oracles and authored markers.
 * Absence stays distinct from incomplete and complete; spelling is tested by
 * the canonical fixtures rather than fabricated from an oracle boolean. */
export function taskCompletion(fields) {
    if (fields.completed !== undefined) return fields.completed;
    const marker = fields.marker;
    return marker === undefined || marker === "null" ? "null" : String(marker !== " ");
}

/** The dump's universal attribute value, independent of source tokenization. */
export function parseAttributesDump(text = "{}") {
    if (!text.startsWith("{") || !text.endsWith("}")) throw new Error("invalid attribute dump");
    let cursor = 1;
    const classes = [],
        records = [];
    function quoted() {
        const match = /^"(?:\\.|[^"\\])*"/.exec(text.slice(cursor));
        if (!match) throw new Error("invalid attribute string");
        cursor += match[0].length;
        return JSON.parse(match[0]);
    }
    while (cursor < text.length - 1) {
        if (text[cursor] === " ") {
            cursor++;
            continue;
        }
        if (text[cursor] === ".") {
            cursor++;
            if (text[cursor] === '"') classes.push(quoted());
            else {
                const start = cursor;
                while (cursor < text.length - 1 && text[cursor] !== " ") cursor++;
                classes.push(text.slice(start, cursor));
            }
        } else {
            const start = cursor;
            while (cursor < text.length - 1 && text[cursor] !== "=") cursor++;
            if (text[cursor++] !== "=") throw new Error("invalid attribute record");
            const name = text.slice(start, cursor - 1);
            records.push({ name, value: quoted() });
        }
    }
    return { classes, records };
}
