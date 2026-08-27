import { visit } from "./visitor.js";
import { Walker, WalkEvent } from "./walker.js";
/** Produces the canonical diagnostic tree for immutable Markdown markup. */
export class TreeDumper {
    constructor() { }
    /** Returns the canonical diagnostic dump for `root` and its descendants. */
    static dump(root) {
        const remainingChildren = [];
        const lines = [];
        new Walker().walk(root, (event, node) => {
            if (event === WalkEvent.entering) {
                const dump = visit(node, dumpVisitor);
                if (remainingChildren.length === 0) {
                    lines.push(dump.line);
                }
                else {
                    const parent = remainingChildren.length - 1;
                    const prefix = remainingChildren
                        .slice(0, -1)
                        .map((remaining) => (remaining > 0 ? "│   " : "    "))
                        .join("");
                    const connector = remainingChildren[parent] === 1 ? "└── " : "├── ";
                    lines.push(prefix + connector + dump.line);
                    remainingChildren[parent] = remainingChildren[parent] - 1;
                }
                remainingChildren.push(dump.children);
            }
            else {
                if (remainingChildren.pop() !== 0)
                    throw new Error("walker exited before its children");
            }
        });
        return `${lines.join("\n")}\n`;
    }
}
const dumpVisitor = {
    visitSemantic: (node) => record("Document", node, [], node.content.length),
    visitBlockQuote: (node) => record("BlockQuote", node, [], node.content.length),
    visitParagraph: (node) => record("Paragraph", node, [], node.content.length),
    visitHeading: (node) => record("Heading", node, [`level=${node.level}`], node.content.length),
    visitThematicBreak: (node) => record("ThematicBreak", node),
    visitList: (node) => record("List", node, [`flavor=${node.flavor}`, `start=${node.start ?? "null"}`, `tight=${node.tight}`], node.items.length),
    visitListItem: (node) => record("ListItem", node, [`checked=${node.checked ?? "null"}`], node.content.length),
    visitCodeBlock: (node) => record("CodeBlock", node, [
        `info=${optionalString(node.info)}`,
        `language=${optionalString(node.language)}`,
        `literal=${jsonString(node.literal)}`,
        `fenced=${node.fenced}`,
        `closed=${node.closed}`
    ]),
    visitHTMLBlock: (node) => record("HTMLBlock", node, [`literal=${jsonString(node.literal)}`]),
    visitFormulaBlock: (node) => record("FormulaBlock", node, [`literal=${jsonString(node.literal)}`]),
    visitTable: (node) => record("Table", node, [`alignments=[${node.alignments.join(",")}]`], 1 + node.rows.length),
    visitTableRow: (node) => record("TableRow", node, [`isHeader=${node.isHeader}`], node.cells.length),
    visitTableCell: (node) => record("TableCell", node, [], node.content.length),
    visitDirectiveBlock: (node) => record("DirectiveBlock", node, directiveFields(node.name, node.attributes), (node.label === null ? 0 : 1) + node.content.length),
    visitDirectiveLabel: (node) => record("DirectiveLabel", node, [], node.content.length),
    visitFootnoteDefinition: (node) => record("FootnoteDefinition", node, definitionAssociation(node), node.content.length),
    visitReferenceDefinition: (node) => record("ReferenceDefinition", node, [
        ...definitionAssociation(node),
        `destination=${jsonString(node.destination)}`,
        `title=${optionalString(node.title)}`
    ]),
    visitLinkReference: (node) => record("LinkReference", node, [`label=${jsonString(node.label)}`, `form=${node.form}`, `definition=${identity(node.definition)}`], node.content.length),
    visitImageReference: (node) => record("ImageReference", node, [`label=${jsonString(node.label)}`, `form=${node.form}`, `definition=${identity(node.definition)}`], node.content.length),
    visitText: (node) => record("Text", node, [`literal=${jsonString(node.literal)}`]),
    visitSoftBreak: (node) => record("SoftBreak", node),
    visitLineBreak: (node) => record("LineBreak", node),
    visitCode: (node) => record("Code", node, [`literal=${jsonString(node.literal)}`]),
    visitHTML: (node) => record("HTML", node, [`literal=${jsonString(node.literal)}`]),
    visitFormula: (node) => record("Formula", node, [`mode=${node.mode}`, `literal=${jsonString(node.literal)}`]),
    visitEmphasis: (node) => record("Emphasis", node, [], node.content.length),
    visitStrong: (node) => record("Strong", node, [], node.content.length),
    visitStrikethrough: (node) => record("Strikethrough", node, [], node.content.length),
    visitLink: (node) => record("Link", node, [`destination=${jsonString(node.destination)}`, `title=${optionalString(node.title)}`], node.content.length),
    visitImage: (node) => record("Image", node, [`source=${jsonString(node.source)}`, `title=${optionalString(node.title)}`], node.content.length),
    visitDirective: (node) => record("Directive", node, directiveFields(node.name, node.attributes), node.label === null ? 0 : 1),
    visitFootnoteReference: (node) => record("FootnoteReference", node, [
        `label=${jsonString(node.label)}`,
        `definition=${identity(node.definition)}`
    ])
};
/** The two fields five kinds carry identically, in contract order. */
/** A definition's two halves, in contract order: the label as written and the
 * match key it folds to. A REFERENCE does not print its key -- it prints
 * `definition=`, and the key is the winning definition's `norm`. */
function definitionAssociation(node) {
    return [`label=${jsonString(node.label)}`, `norm=${jsonString(node.norm)}`];
}
function record(kind, node, fields = [], children = 0) {
    const fieldText = fields.length === 0 ? "" : ` ${fields.join(" ")}`;
    return { line: `${kind} id=${identity(node.id)} ${scope(node.scope)}${fieldText} children=${children}`, children };
}
function identity(value) {
    return `${value.block}:${value.ordinal}`;
}
function directiveFields(name, attributes) {
    if (attributes === null)
        return [`name=${jsonString(name)}`, "attributes=null"];
    const pairs = attributes.map((pair) => `${pair.name}=${jsonString(pair.value)}`).join(" ");
    return [`name=${jsonString(name)}`, `attributes=[${pairs}]`];
}
function scope(value) {
    return `scope=${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}`;
}
function optionalString(value) {
    return value === null ? "null" : jsonString(value);
}
function jsonString(value) {
    return JSON.stringify(value);
}
