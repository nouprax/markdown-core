import type { Markup } from "./model/markup.js";

export const WalkEvent = {
    entering: "entering",
    exiting: "exiting"
} as const;

export type WalkEvent = (typeof WalkEvent)[keyof typeof WalkEvent];

interface Frame {
    readonly event: WalkEvent;
    readonly node: Markup;
}

export class Walker {
    walk(root: Markup, callback: (event: WalkEvent, node: Markup) => void): void {
        const stack: Frame[] = [{ event: WalkEvent.entering, node: root }];
        while (stack.length > 0) {
            const frame = stack.pop()!;
            callback(frame.event, frame.node);
            if (frame.event === WalkEvent.exiting) continue;

            stack.push({ event: WalkEvent.exiting, node: frame.node });
            const descendants = children(frame.node);
            for (let index = descendants.length - 1; index >= 0; index -= 1) {
                stack.push({ event: WalkEvent.entering, node: descendants[index]! });
            }
        }
    }
}

function children(node: Markup): readonly Markup[] {
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
            return [...(node.label ?? []), ...node.content];
        case "directive":
            return node.label ?? [];
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

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}
