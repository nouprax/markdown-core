export function visit(node, visitor) {
    switch (node.kind) {
        case "document":
            return visitor.visitSemantic(node);
        case "blockQuote":
            return visitor.visitBlockQuote(node);
        case "paragraph":
            return visitor.visitParagraph(node);
        case "heading":
            return visitor.visitHeading(node);
        case "thematicBreak":
            return visitor.visitThematicBreak(node);
        case "list":
            return visitor.visitList(node);
        case "listItem":
            return visitor.visitListItem(node);
        case "codeBlock":
            return visitor.visitCodeBlock(node);
        case "htmlBlock":
            return visitor.visitHTMLBlock(node);
        case "formulaBlock":
            return visitor.visitFormulaBlock(node);
        case "table":
            return visitor.visitTable(node);
        case "tableRow":
            return visitor.visitTableRow(node);
        case "tableCell":
            return visitor.visitTableCell(node);
        case "directiveBlock":
            return visitor.visitDirectiveBlock(node);
        case "directiveLabel":
            return visitor.visitDirectiveLabel(node);
        case "footnoteDefinition":
            return visitor.visitFootnoteDefinition(node);
        case "referenceDefinition":
            return visitor.visitReferenceDefinition(node);
        case "linkReference":
            return visitor.visitLinkReference(node);
        case "imageReference":
            return visitor.visitImageReference(node);
        case "text":
            return visitor.visitText(node);
        case "softBreak":
            return visitor.visitSoftBreak(node);
        case "lineBreak":
            return visitor.visitLineBreak(node);
        case "code":
            return visitor.visitCode(node);
        case "html":
            return visitor.visitHTML(node);
        case "formula":
            return visitor.visitFormula(node);
        case "emphasis":
            return visitor.visitEmphasis(node);
        case "strong":
            return visitor.visitStrong(node);
        case "strikethrough":
            return visitor.visitStrikethrough(node);
        case "link":
            return visitor.visitLink(node);
        case "image":
            return visitor.visitImage(node);
        case "directive":
            return visitor.visitDirective(node);
        case "footnoteReference":
            return visitor.visitFootnoteReference(node);
    }
    return unreachable(node);
}
function unreachable(value) {
    throw new Error(`unreachable markup ${String(value)}`);
}
