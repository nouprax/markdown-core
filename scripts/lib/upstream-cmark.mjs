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
 * is either a registered difference (`specs/upstream-parity/deltas.json`) or a
 * defect. The two ASTs are not the same shape, so the mapping below is
 * load-bearing and each rule is a claim about equivalence, not a convenience.
 */

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
    image: "Image",
    list: "List",
    item: "ListItem",
    tasklist: "ListItem",
    block_quote: "BlockQuote",
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
 * dumps instead, and the gap is recorded in specs/upstream-parity/README.md.
 */
const COMPARED = {
    Heading: ["level"],
    List: ["flavor", "tight", "start"],
    ListItem: ["checked"],
    CodeBlock: ["info", "literal"],
    Code: ["literal"],
    Text: ["literal"],
    HTML: ["literal"],
    HTMLBlock: ["literal"],
    Link: ["destination", "title"],
    Image: ["destination", "title"],
    TableRow: ["isHeader"]
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
export function parseUpstreamXml(xml) {
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
        if (name === "<unknown>") kind = selfClose ? "FootnoteReference" : "FootnoteDefinition";
        if (kind === undefined) kind = `?${name}`;
        const node = { kind, fields: attributes, children: [] };
        if (name === "table_header") node.fields.isHeader = "true";
        if (name === "table_row") node.fields.isHeader = "false";
        if (name === "tasklist") node.fields.checked = attributes.completed === "true" ? "true" : "false";
        if (name === "item") node.fields.checked = "null";
        if (LITERAL_BEARING.has(name)) node.pendingText = "";
        stack[stack.length - 1].children.push(node);
        if (!selfClose) stack.push(node);
        else if (node.pendingText !== undefined) node.fields.literal = "";
    }
    return root;
}

/** Parses this repository's canonical AST dump. */
export function parseCanonicalDump(dump) {
    const lines = dump.split("\n").filter((line) => line.trim().length);
    const root = { kind: "Document", fields: {}, children: [] };
    const byDepth = [root];
    for (const line of lines.slice(1)) {
        const marker = line.search(/[├└]/);
        const depth = marker < 0 ? 1 : marker / 4 + 1;
        const body = marker < 0 ? line.trim() : line.slice(marker + 4).trim();
        const fields = {};
        // A bracketed group is ONE field even when it contains spaces: a
        // directive's attributes print as `[k="v" k2="v w"]`.
        for (const field of body.matchAll(/([a-zA-Z]+)=("(?:[^"\\]|\\.)*"|\[(?:"(?:[^"\\]|\\.)*"|[^\]])*\]|[^\s]+)/g)) {
            fields[field[1]] = field[2].startsWith('"') ? JSON.parse(field[2]) : field[2];
        }
        const node = { kind: body.split(" ")[0], fields, children: [] };
        byDepth[depth - 1].children.push(node);
        byDepth[depth] = node;
    }
    return root;
}

export function normalize(node, side) {
    const children = [];
    for (const child of node.children) {
        const normalized = normalize(child, side);
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
    const fields = {};
    for (const key of COMPARED[node.kind] ?? []) {
        let value = node.fields[key];
        if (side === "upstream") {
            if (node.kind === "List" && key === "flavor") value = node.fields.type;
            if (node.kind === "List" && key === "tight") value = node.fields.tight ?? "false";
            if (node.kind === "CodeBlock" && key === "info") value = node.fields.info ?? "null";
        } else if (node.kind === "Image" && key === "destination") {
            value = node.fields.source;
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
 * Registered delta `footnote-definition-placement`: upstream moves every
 * footnote definition to the document tail in first-reference order, while
 * this repository's AST is source-faithful and leaves each one where it was
 * written (canonical-ast.md). Both sides therefore have their definitions
 * lifted out and re-attached in one deterministic order, which compares their
 * *content* while deliberately not comparing their position.
 */
export function liftFootnoteDefinitions(root) {
    const definitions = [];
    const strip = (node) => {
        node.children = node.children.filter((child) => {
            if (child.kind === "FootnoteDefinition") {
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
 * representations of retention.
 */
export function applyUpstreamFootnoteModel(root) {
    // cmark matches footnote labels through the reference map's normalization:
    // case-folded with whitespace runs collapsed.
    const fold = (label) => label.trim().replace(/\s+/g, " ").toLowerCase();
    const referenced = new Set();
    const survey = (node) => {
        if (node.kind === "FootnoteReference") referenced.add(fold(node.fields.id ?? ""));
        for (const child of node.children) survey(child);
    };
    survey(root);

    const rewrite = (node) => {
        node.children = node.children.filter(
            (child) => !(child.kind === "FootnoteDefinition" && !referenced.has(fold(child.fields.id ?? "")))
        );
        for (const child of node.children) rewrite(child);
        return node;
    };
    return rewrite(root);
}

/**
 * Registered delta `reference-definition-node`: upstream consumes a link
 * reference definition into its map and leaves no node, and resolves each
 * reference into a `Link`/`Image` carrying a copy of the destination. This
 * repository keeps the definition where it was written and lets the reference
 * carry only its label.
 *
 * Applying upstream's model to this side is what makes the two comparable, and
 * it compares the thing that matters: the label is resolved against the
 * document's own definitions, so a reference that resolved to the wrong
 * destination, or to none, still shows up as a difference.
 */
export function applyUpstreamReferenceModel(root) {
    const fold = (label) => label.trim().replace(/\s+/g, " ").toLowerCase();
    const definitions = new Map();
    const survey = (node) => {
        if (node.kind === "ReferenceDefinition") {
            const key = fold(node.fields.label ?? "");
            // The earliest definition of a label wins, in both models.
            if (!definitions.has(key)) definitions.set(key, node.fields);
        }
        for (const child of node.children) survey(child);
    };
    survey(root);

    const rewrite = (node) => {
        node.children = node.children
            .filter((child) => child.kind !== "ReferenceDefinition")
            .map((child) => {
                if (child.kind === "LinkReference" || child.kind === "ImageReference") {
                    const found = definitions.get(fold(child.fields.label ?? ""));
                    rewrite(child);
                    return {
                        kind: child.kind === "LinkReference" ? "Link" : "Image",
                        fields: {
                            destination: found?.destination ?? "",
                            source: found?.destination ?? "",
                            title: found?.title ?? ""
                        },
                        children: child.children
                    };
                }
                rewrite(child);
                return child;
            });
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
