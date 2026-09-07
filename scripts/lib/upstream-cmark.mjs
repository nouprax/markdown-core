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
    List: ["flavor", "tight", "start"],
    ListItem: ["checked"],
    CodeBlock: ["info", "literal"],
    Code: ["literal"],
    Text: ["literal"],
    HTML: ["literal"],
    HTMLBlock: ["literal"],
    Comment: ["literal"],
    Link: ["dest", "title"],
    Image: ["dest", "title"],
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
        // The footnote extension's two nodes (M4): a call is a one-item `Cite`
        // whose `Citation` carries empty affix groups, and a definition is a
        // `Footnote` value; upstream states no label for either, so neither
        // carries a compared field.
        if (name === "<unknown>") kind = selfClose ? "Cite" : "Footnote";
        if (kind === undefined) kind = `?${name}`;
        const node = { kind, fields: attributes, children: [] };
        if (kind === "Cite") node.children.push(citationItem({}));
        if (name === "table_header") node.fields.isHeader = "true";
        if (name === "table_row") node.fields.isHeader = "false";
        if (name === "tasklist") node.fields.checked = attributes.completed === "true" ? "true" : "false";
        if (name === "item") node.fields.checked = "null";
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
    return root;
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
export function parseCanonicalFields(body) {
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
            } else if (/\s/.test(character) && parentheses === 0 && brackets === 0) {
                break;
            }
            cursor++;
        }

        const value = body.slice(valueStart, cursor);
        fields[name] = value.startsWith('"') ? JSON.parse(value) : value;
    }
    return fields;
}

/** The `url` branch of a `Destination`: the target as one decoded string. */
export function urlDestination(value) {
    return { kind: "url", value };
}

/** The `cross` branch of a `Destination`: a workspace path, possibly empty, and an anchor or null. */
export function crossDestination(path, anchor) {
    return { kind: "cross", path, anchor };
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

/** One `Citation` item as the dump nests it: a value line with the given
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
    const anchor = destination.anchor === null ? "null" : JSON.stringify(destination.anchor);
    return `cross(path=${JSON.stringify(destination.path)},anchor=${anchor})`;
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
        const fields = parseCanonicalFields(body);
        const node = { kind: body.split(" ")[0], fields, children: [] };
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
        if (side === "upstream") {
            if (node.kind === "List" && key === "flavor") value = node.fields.type;
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
 * this repository's AST owns every footnote as a `Footnote` value of the
 * document in source order (canonical-ast.md, M4). Both sides therefore have
 * their footnotes lifted out and re-attached in one deterministic order, which
 * compares their *content* while deliberately not comparing their position.
 */
export function liftFootnotes(root, fired) {
    const definitions = [];
    const strip = (node) => {
        node.children = node.children.filter((child) => {
            if (child.kind === "Footnote") {
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
 * representations of retention.
 */
export function applyUpstreamFootnoteModel(root, fired) {
    // Both a `Footnote.id` and a `footnote` referent's id are the label under
    // the reference map's own normalization (M4), so they compare directly.
    const referenced = new Set();
    const survey = (node) => {
        const referent = /^footnote\(id=("(?:\\.|[^"\\])*")\)$/.exec(node.fields.referent ?? "");
        if (node.kind === "Citation" && referent) referenced.add(JSON.parse(referent[1]));
        for (const child of node.children) survey(child);
    };
    survey(root);

    const rewrite = (node) => {
        const before = node.children.length;
        node.children = node.children.filter(
            (child) => !(child.kind === "Footnote" && !referenced.has(child.fields.id ?? ""))
        );
        if (node.children.length !== before) fired?.add("footnote-resolution-model");
        for (const child of node.children) rewrite(child);
        return node;
    };
    return rewrite(root);
}

/* No reference projection any more (M2): both sides consume a link reference
 * definition into a map and resolve every reference into the `Link` or `Image`
 * it names, so a reference that resolved to the wrong definition, or to none,
 * shows up as a plain difference in `dest`, `title`, or kind. */

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
