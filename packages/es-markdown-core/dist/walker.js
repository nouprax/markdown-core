export const WalkEvent = {
    entering: "entering",
    exiting: "exiting"
};
export class Walker {
    walk(root, callback) {
        const stack = [{ event: WalkEvent.entering, node: root }];
        while (stack.length > 0) {
            const frame = stack.pop();
            callback(frame.event, frame.node);
            if (frame.event === WalkEvent.exiting)
                continue;
            stack.push({ event: WalkEvent.exiting, node: frame.node });
            const descendants = children(frame.node);
            for (let index = descendants.length - 1; index >= 0; index -= 1) {
                stack.push({ event: WalkEvent.entering, node: descendants[index] });
            }
        }
    }
}
/** Every child of a node, in the order the C tree holds them. `Walker` walks
 * with it, so a walk and a hand-written descent cannot disagree about what a
 * node's children are. */
export function children(node) {
    switch (node.kind) {
        case "document":
        case "blockQuote":
        case "paragraph":
        case "heading":
        case "listItem":
        case "footnoteDefinition":
        case "emphasis":
        case "strong":
        case "strikethrough":
        case "link":
        case "image":
        case "linkReference":
        case "imageReference":
            return node.content;
        case "list":
            return node.items;
        case "table":
            return [node.header, ...node.rows];
        case "tableRow":
            return node.cells;
        case "tableCell":
            return node.content;
        case "directiveBlock":
            return node.label === null ? node.content : [node.label, ...node.content];
        case "directiveLabel":
            return node.content;
        case "directive":
            return node.label === null ? [] : [node.label];
        case "referenceDefinition":
        case "thematicBreak":
        case "codeBlock":
        case "htmlBlock":
        case "formulaBlock":
        case "text":
        case "softBreak":
        case "lineBreak":
        case "code":
        case "html":
        case "formula":
        case "footnoteReference":
            return [];
    }
    return unreachable(node);
}
function unreachable(value) {
    throw new Error(`unreachable markup ${String(value)}`);
}
