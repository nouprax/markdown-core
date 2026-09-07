/**
 * mdast/remark normalization.
 *
 * cmark owns CommonMark and cmark-gfm owns its GFM extension layer. remark is
 * the corrective and supplementary oracle for directives, math, footnote
 * representation, tables, and references. This module maps mdast onto the same
 * comparison-only form `upstream-cmark.mjs` produces.
 *
 * Two model differences are normalized rather than reported, because they are
 * choices about AST shape rather than about what the Markdown means.
 *
 *   - mdast splits a directive's label into children; Markdown Core carries it
 *     as a `DirectiveLabel` field. The comparison tree nests node-valued
 *     fields, so both are compared by their content without changing AST
 *     ownership semantics.
 *   - mdast keeps a link reference definition as a node and its references
 *     unresolved; Markdown Core consumes the definition into the parser's map
 *     and resolves every successful reference into the `Link` or `Image` it
 *     names, as cmark does (M2, registered delta `reference-resolution-model`).
 *     This module resolves mdast's references against mdast's own definitions
 *     -- the first definition of an identifier wins in both grammars -- so a
 *     reference that resolved to the wrong definition, or to none, still shows
 *     up as a difference.
 */

import { citationItem, urlDestination } from "./upstream-cmark.mjs";

const MDAST_KIND = {
    root: "Document",
    paragraph: "Paragraph",
    heading: "Heading",
    text: "Text",
    emphasis: "Emphasis",
    strong: "Strong",
    inlineCode: "Code",
    code: "CodeBlock",
    html: "HTML",
    link: "Link",
    image: "Image",
    list: "List",
    listItem: "ListItem",
    blockquote: "Callout",
    thematicBreak: "ThematicBreak",
    break: "LineBreak",
    delete: "Strikethrough",
    table: "Table",
    tableRow: "TableRow",
    tableCell: "TableCell",
    footnoteReference: "Cite",
    footnoteDefinition: "Footnote",
    textDirective: "Directive",
    leafDirective: "DirectiveBlock",
    containerDirective: "DirectiveBlock",
    inlineMath: "Formula",
    math: "FormulaBlock"
};

/**
 * mdast has no softbreak node: a soft line break is a `\n` inside a text
 * value. Markdown Core makes it a node, so text values are split on newlines
 * to put the two models in the same shape.
 */
function splitSoftBreaks(value) {
    const parts = value.split("\n");
    const out = [];
    for (const [index, part] of parts.entries()) {
        if (index > 0) out.push({ kind: "SoftBreak", fields: {}, children: [] });
        if (part.length) out.push({ kind: "Text", fields: { literal: part }, children: [] });
    }
    return out;
}

function collectDefinitions(node, into = new Map()) {
    // The first definition of an identifier wins, in mdast as in CommonMark.
    if (node.type === "definition" && !into.has(node.identifier)) into.set(node.identifier, node);
    for (const child of node.children ?? []) collectDefinitions(child, into);
    return into;
}

/** mdast parents whose `html` children are flow (block) nodes. */
const FLOW_PARENTS = new Set(["root", "blockquote", "listItem", "footnoteDefinition", "containerDirective"]);

function inlineCommentBody(literal) {
    if (!literal.startsWith("<!--")) return null;
    return literal.length > 6 ? literal.slice(4, -3) : "";
}

function blockCommentBody(literal) {
    const open = literal.search(/[^ \t]/);
    if (open < 0 || !literal.startsWith("<!--", open)) return null;
    const close = literal.indexOf("-->", open + 2);
    if (close < 0) return null;
    if (!/^[ \t]*(?:\r?\n)?$/.test(literal.slice(close + 3))) return null;
    return close > open + 4 ? literal.slice(open + 4, close) : "";
}

function convert(node, definitions, parentType = "root") {
    if (node.type === "text") return splitSoftBreaks(node.value);

    // Registered shape delta `reference-resolution-model`: a definition is
    // consumed and produces no node, and a reference that resolves is the
    // `Link` or `Image` it names, carrying the definition's destination and
    // title -- the shape this repository's parser, like cmark, produces (M2).
    if (node.type === "definition") return [];
    if (node.type === "linkReference" || node.type === "imageReference") {
        // An undefined label is not a reference in either model: a bare
        // bracket is prose, which is the one axis Markdown's grammar settles
        // rather than either project.
        const definition = definitions.get(node.identifier);
        if (!definition) {
            return [{ kind: "Text", fields: { literal: node.label ?? "" }, children: [] }];
        }
        return [
            {
                kind: node.type === "linkReference" ? "Link" : "Image",
                fields: {
                    dest: urlDestination(definition.url ?? ""),
                    title: definition.title ?? "null"
                },
                children: (node.children ?? []).flatMap((child) => convert(child, definitions, node.type))
            }
        ];
    }

    const kind = MDAST_KIND[node.type] ?? `?${node.type}`;
    const fields = {};
    if (node.type === "heading") fields.level = String(node.depth);
    // Every `>` container is a `Callout` (M3). mdast's `blockquote` carries no
    // callout metadata, so the projection states the absence the canonical
    // AST prints for a metadata-free container.
    if (node.type === "blockquote") {
        fields.variant = "null";
        fields.collapsed = "null";
    }
    if (node.type === "list") {
        fields.flavor = node.ordered ? "ordered" : "bullet";
        fields.tight = String(Boolean(!node.spread));
        fields.start = node.ordered ? String(node.start ?? 1) : "null";
    }
    if (node.type === "listItem")
        fields.checked = node.checked === null || node.checked === undefined ? "null" : String(node.checked);
    // Registered shape delta `code-span-line-ending`: CommonMark says a code
    // span's line endings are spaces, and cmark applies that when it builds the
    // node, which this repository inherits. mdast keeps the line ending in the
    // node and leaves the conversion to mdast-util-to-hast at render time. The
    // two agree on what the span means; they disagree on which layer states it,
    // and cmark is the authority for the CommonMark layer.
    if (node.type === "inlineCode") fields.literal = (node.value ?? "").replace(/\r\n|\r|\n/g, " ");
    // mdast strips a code block's trailing line ending; the canonical dump keeps it.
    if (node.type === "code") fields.literal = `${node.value ?? ""}\n`;
    if (node.type === "code") fields.info = node.lang ? node.lang : "null";
    if (node.type === "html") fields.literal = node.value ?? "";
    // Registered shape delta `html-comment-node`: mdast keeps an HTML comment
    // as an `html` node holding the token; Markdown Core makes it a `Comment`
    // whose literal is the bytes between the delimiters (M0). The same rule
    // the cmark projection states: an inline token whose literal opens with
    // `<!--`, and a flow node that opens with `<!--` after its indentation and
    // whose end line holds only whitespace after the first `-->`. mdast's
    // flow value carries no trailing line ending, so the end-line test is on
    // the value's tail.
    if (node.type === "html") {
        const comment = FLOW_PARENTS.has(parentType)
            ? blockCommentBody(fields.literal)
            : inlineCommentBody(fields.literal);
        if (comment !== null) return [{ kind: "Comment", fields: { literal: comment }, children: [] }];
    }
    if (node.type === "link" || node.type === "image") {
        // mdast's `url` is the decoded target; the canonical AST states it as
        // the `url` branch of `Destination` (M1).
        fields.dest = urlDestination(node.url ?? "");
        fields.title = node.title ?? "null";
    }
    // The citation model (M4): a call is a one-item `Cite` whose `Citation`
    // names the footnote by id and carries empty affix groups; a definition
    // is a `Footnote` value. mdast's `identifier` is its normalized label,
    // upper-cased by micromark's normalizer where this side's is case-folded,
    // so the two meet in lower case.
    if (node.type === "footnoteReference") {
        const id = JSON.stringify((node.identifier ?? node.label ?? "").toLowerCase());
        return [{ kind: "Cite", fields: {}, children: [citationItem({ referent: `footnote(id=${id})` })] }];
    }
    if (node.type === "footnoteDefinition") fields.id = (node.identifier ?? node.label ?? "").toLowerCase();
    if (node.type === "tableRow") fields.isHeader = "false";
    if (node.type === "inlineMath" || node.type === "math") fields.literal = node.value ?? "";
    if (node.type === "textDirective" || node.type === "leafDirective" || node.type === "containerDirective") {
        fields.name = node.name ?? "";
        fields.attributes = renderAttributes(node.attributes);
    }

    let children = (node.children ?? []).flatMap((child) => convert(child, definitions, node.type));
    // A directive's label becomes a nested `DirectiveLabel` in this comparison
    // tree. In the canonical AST it is a field, not directive content. mdast states it two
    // ways: for text and leaf directives it is the directive's own children,
    // and for container directives it is a first-child paragraph flagged
    // `data.directiveLabel`. Both become the same node so the label's content
    // is what gets compared.
    if ((node.type === "textDirective" || node.type === "leafDirective") && children.length) {
        children = [{ kind: "DirectiveLabel", fields: {}, children }];
    }
    if (node.type === "containerDirective") {
        const [first] = node.children ?? [];
        if (first?.type === "paragraph" && first.data?.directiveLabel) {
            children = [{ kind: "DirectiveLabel", fields: {}, children: children[0].children }, ...children.slice(1)];
        }
    }
    return [{ kind, fields, children }];
}

/**
 * Registered shape delta `empty-text-node`: a run that is only the spaces a
 * hard or soft break strips can leave an empty text node in the cmark family.
 * Markdown Core and mdast omit a node that owns no source or content. Dropping
 * it from both comparison trees isolates the semantic content; the cmark and
 * cmark-gfm policies independently verify that this projection still acts.
 */
/**
 * A directive's attribute block is most of its grammar — the name/value rules,
 * the `#id` and `.class` shorthands, quoting, and what makes a block malformed.
 * Comparing only the directive's name left all of that unjudged, which is how
 * the unquoted-value rules came to differ from the authority's without anything
 * noticing.
 *
 * mdast holds attributes as an insertion-ordered object. The canonical dump
 * prints the same first-occurrence order, so this comparison deliberately does
 * not normalize away an ordering defect.
 */
export function renderAttributes(attributes) {
    const entries = Object.entries(attributes ?? {}).filter(([, value]) => value !== null && value !== undefined);
    if (!entries.length) return "null";
    return entries.map(([key, value]) => `${key}=${JSON.stringify(String(value))}`).join(" ");
}

export function dropEmptyText(node) {
    node.children = node.children.filter((child) => {
        if (child.kind === "Text" && child.fields.literal === "") return false;
        dropEmptyText(child);
        return true;
    });
    return node;
}

export function fromMdast(tree) {
    const definitions = collectDefinitions(tree);
    const [root] = convert(tree, definitions);
    // remark marks the first table row as a header by position, not by a flag.
    // The walk covers the whole tree: a table nested in a block quote or a
    // container directive is still a table, and only looking at the root's own
    // children left those first rows marked as body rows.
    const markHeaders = (node) => {
        if (node.kind === "Table" && node.children.length) node.children[0].fields.isHeader = "true";
        for (const child of node.children) markHeaders(child);
    };
    markHeaders(root);
    return root;
}

/**
 * Fields compared per kind for this oracle. Narrower than the cmark one on
 * purpose: remark's model carries values cmark does not and vice versa, and a
 * field is compared only where both models agree it means the same thing.
 */
export const MDAST_COMPARED = {
    Callout: ["variant", "collapsed"],
    Heading: ["level"],
    List: ["flavor", "tight", "start"],
    ListItem: ["checked"],
    CodeBlock: ["info", "literal"],
    Code: ["literal"],
    Text: ["literal"],
    HTML: ["literal"],
    Comment: ["literal"],
    Link: ["dest", "title"],
    Image: ["dest", "title"],
    TableRow: ["isHeader"],
    // §5.6: footnote label bytes used to be compared by NOBODY, on either
    // side. mdast's `label` is the authored spelling and so is this side's, so
    // there is something to compare as of Step 9b.2. `identifier` is NOT
    // compared for these two: this side keeps the leading `^` deliberately and
    // mdast does not (§5.2), which is a difference of one byte that would have
    // to be registered rather than checked.
    Citation: ["referent"],
    Footnote: ["id"],
    Directive: ["name", "attributes"],
    DirectiveBlock: ["name", "attributes"],
    Formula: ["literal"],
    FormulaBlock: ["literal"]
};
