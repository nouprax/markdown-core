import type { Document } from "./model/document.js";
import type { Markup } from "./model/markup.js";
import type { Scope } from "./values.js";

export const WalkEvent = {
    entering: "entering",
    exiting: "exiting"
} as const;

export type WalkEvent = (typeof WalkEvent)[keyof typeof WalkEvent];

export type WalkCallback = (event: WalkEvent, node: Markup, scope: Scope) => void;

interface Frame {
    readonly event: WalkEvent;
    readonly node: Markup;
    readonly scope?: Scope;
}

export class MarkupWalker {
    /** Walks the document depth-first, supplying each event with the node's
     * resolved absolute scope. */
    walk(document: Document, callback: WalkCallback): void;
    /** Walks the subtree rooted at `from`; scopes stay document-absolute. */
    walk(document: Document, from: Markup, callback: WalkCallback): void;
    walk(document: Document, fromOrCallback: Markup | WalkCallback, subtreeCallback?: WalkCallback): void {
        const from = typeof fromOrCallback === "function" ? document : fromOrCallback;
        const callback = typeof fromOrCallback === "function" ? fromOrCallback : subtreeCallback;
        if (typeof callback !== "function") throw new TypeError("walk requires a callback");
        const stack: Frame[] = [{ event: WalkEvent.entering, node: from }];
        while (stack.length > 0) {
            const frame = stack.pop()!;
            const scope = frame.scope ?? document.scope(frame.node);
            callback(frame.event, frame.node, scope);
            if (frame.event === WalkEvent.exiting) continue;

            stack.push({ event: WalkEvent.exiting, node: frame.node, scope });
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
