/**
 * mdast/remark normalization.
 *
 * cmark-gfm is the authority for the base language, but it cannot judge the
 * constructs it does not implement. For directives, math, and footnote
 * placement the authority is the unified/remark ecosystem, whose definitions
 * this repository's extensions were written against — so remark is the second
 * oracle, and this module maps its tree onto the same comparable form
 * `upstream-cmark.mjs` produces.
 *
 * One model difference is normalized rather than reported, because it is a
 * choice about AST shape rather than about what the Markdown means. The
 * reference model used to be a second: mdast kept definitions as nodes and
 * references unresolved while this repository resolved them into `Link`/`Image`
 * as cmark does, and this module projected one onto the other. Since
 * 2026-08-02 the two agree and nothing is projected.
 *
 *   - mdast splits a directive's label into children; Markdown Core carries it
 *     as a `DirectiveLabel` child. Both are compared by their content.
 */

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
    blockquote: "BlockQuote",
    thematicBreak: "ThematicBreak",
    break: "LineBreak",
    delete: "Strikethrough",
    table: "Table",
    tableRow: "TableRow",
    tableCell: "TableCell",
    footnoteReference: "FootnoteReference",
    footnoteDefinition: "FootnoteDefinition",
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
    if (node.type === "definition") into.set(node.identifier, node);
    for (const child of node.children ?? []) collectDefinitions(child, into);
    return into;
}

function convert(node, definitions) {
    if (node.type === "text") return splitSoftBreaks(node.value);

    // The two models now agree here, so nothing is projected away: a
    // definition is a node where it was written, and a reference carries its
    // label and form and no destination. This used to resolve remark's
    // references and drop its definitions to reach this repository's older,
    // cmark-inherited shape; the delta that described it is retired.
    if (node.type === "definition") {
        return [
            {
                kind: "ReferenceDefinition",
                fields: {
                    label: node.label ?? node.identifier ?? "",
                    destination: node.url ?? "",
                    title: node.title ?? ""
                },
                children: []
            }
        ];
    }
    if (node.type === "linkReference" || node.type === "imageReference") {
        // An undefined label is not a reference in either model: a bare
        // bracket is prose, which is the one axis Markdown's grammar settles
        // rather than either project.
        if (!definitions.has(node.identifier)) {
            return [{ kind: "Text", fields: { literal: node.label ?? "" }, children: [] }];
        }
        return [
            {
                kind: node.type === "linkReference" ? "LinkReference" : "ImageReference",
                fields: { label: node.label ?? "", form: node.referenceType ?? "shortcut" },
                children: (node.children ?? []).flatMap((child) => convert(child, definitions))
            }
        ];
    }

    const kind = MDAST_KIND[node.type] ?? `?${node.type}`;
    const fields = {};
    if (node.type === "heading") fields.level = String(node.depth);
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
    // and cmark-gfm is the authority for the base language.
    if (node.type === "inlineCode") fields.literal = (node.value ?? "").replace(/\r\n|\r|\n/g, " ");
    // mdast strips a code block's trailing line ending; the canonical dump keeps it.
    if (node.type === "code") fields.literal = `${node.value ?? ""}\n`;
    if (node.type === "code") fields.info = node.lang ? node.lang : "null";
    if (node.type === "html") fields.literal = node.value ?? "";
    if (node.type === "link" || node.type === "image") {
        fields.destination = node.url ?? "";
        fields.title = node.title ?? "null";
    }
    if (node.type === "tableRow") fields.isHeader = "false";
    if (node.type === "inlineMath" || node.type === "math") fields.literal = node.value ?? "";
    if (node.type === "textDirective" || node.type === "leafDirective" || node.type === "containerDirective") {
        fields.name = node.name ?? "";
        fields.attributes = renderAttributes(node.attributes);
    }

    let children = (node.children ?? []).flatMap((child) => convert(child, definitions));
    // A directive's label is a `DirectiveLabel` child here. mdast states it two
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
 * hard or soft break strips leaves an empty text node in cmark's model, which
 * this repository inherits and its goldens record. mdast has no such node.
 * Dropping empty text from both sides compares the content, which is what the
 * two models agree exists.
 *
 * Removing the node instead was tried and is wrong: upstream cmark-gfm emits
 * it, so suppressing it here would put this parser at odds with the authority
 * for the base language in order to agree with the authority for the
 * extensions.
 */
/**
 * A directive's attribute block is most of its grammar — the name/value rules,
 * the `#id` and `.class` shorthands, quoting, and what makes a block malformed.
 * Comparing only the directive's name left all of that unjudged, which is how
 * the unquoted-value rules came to differ from the authority's without anything
 * noticing.
 *
 * mdast holds attributes as an object; the canonical dump prints a JSON object
 * or `null`. Both are rendered here as one sorted `key=value` string so the
 * comparison is about the attributes rather than about key order or the two
 * spellings of "none".
 */
export function renderAttributes(attributes) {
    const entries = Object.entries(attributes ?? {}).filter(([, value]) => value !== null && value !== undefined);
    if (!entries.length) return "null";
    entries.sort(([left], [right]) => (left < right ? -1 : left > right ? 1 : 0));
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
    Heading: ["level"],
    List: ["flavor", "tight", "start"],
    ListItem: ["checked"],
    CodeBlock: ["info", "literal"],
    Code: ["literal"],
    Text: ["literal"],
    HTML: ["literal"],
    Link: ["destination", "title"],
    Image: ["destination", "title"],
    TableRow: ["isHeader"],
    ReferenceDefinition: ["label", "destination", "title"],
    LinkReference: ["label", "form"],
    ImageReference: ["label", "form"],
    Directive: ["name", "attributes"],
    DirectiveBlock: ["name", "attributes"],
    Formula: ["literal"],
    FormulaBlock: ["literal"]
};
