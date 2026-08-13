import type { Document } from "./model/document.js";
import type { Markup } from "./model/markup.js";
import { visit, type MarkupVisitor } from "./markup-visitor.js";
import type { Scope } from "./values.js";

/**
 * Whether a walk callback fires on the way into a node or on the way out.
 *
 * Every node produces both, in that order: `entering` in preorder, and
 * `exiting` once the last of its descendants has exited. A leaf gets the two
 * back to back. The pairs therefore nest exactly, which is what lets a
 * consumer keep a stack instead of a parent map.
 */
export const WalkEvent = {
    entering: "entering",
    exiting: "exiting"
} as const;

export type WalkEvent = (typeof WalkEvent)[keyof typeof WalkEvent];

/** Returning is the callback's only reply.
 *
 * There is no way to prune a subtree mid-walk, so a consumer that wants only
 * some nodes tests for them and returns. A throw leaves `walk` at once. */
export type WalkCallback = (event: WalkEvent, node: Markup, scope: Scope) => void;

interface Frame {
    readonly event: WalkEvent;
    readonly node: Markup;
}

/**
 * A read-only depth-first traversal, iterative rather than recursive:
 * nesting depth is input-controlled, so a document that parsed also walks,
 * whatever its depth.
 *
 * An instance holds nothing — the frame stack belongs to the call — so one
 * walker serves any number of walks, including one begun inside another's
 * callback.
 */
export class MarkupWalker {
    /**
     * Walks the document depth-first, dispatching each node to `visitor` in
     * preorder.
     *
     * One dispatch per node, not the callback overload's entering/exiting
     * pair, and no scope argument: `scope` is a field on the node.
     */
    walk(document: Document, visitor: MarkupVisitor<void>): void;
    /** Walks the document depth-first, supplying each event with the node's
     * resolved absolute scope. */
    walk(document: Document, callback: WalkCallback): void;
    /** Walks the subtree rooted at `from`; scopes stay document-absolute. */
    walk(document: Document, from: Markup, callback: WalkCallback): void;
    walk(
        document: Document,
        fromOrHandler: Markup | WalkCallback | MarkupVisitor<void>,
        subtreeCallback?: WalkCallback
    ): void {
        if (isVisitor(fromOrHandler) && subtreeCallback === undefined) {
            MarkupWalker.walkVisitor(document, fromOrHandler);
            return;
        }
        const from = typeof fromOrHandler === "function" ? document : (fromOrHandler as Markup);
        const callback = typeof fromOrHandler === "function" ? fromOrHandler : subtreeCallback;
        if (typeof callback !== "function") throw new TypeError("walk requires a callback");
        const stack: Frame[] = [{ event: WalkEvent.entering, node: from }];
        while (stack.length > 0) {
            const frame = stack.pop()!;
            const scope = frame.node.scope;
            callback(frame.event, frame.node, scope);
            if (frame.event === WalkEvent.exiting) continue;

            stack.push({ event: WalkEvent.exiting, node: frame.node });
            const descendants = children(frame.node);
            for (let index = descendants.length - 1; index >= 0; index -= 1) {
                stack.push({ event: WalkEvent.entering, node: descendants[index]! });
            }
        }
    }

    private static walkVisitor(document: Document, visitor: MarkupVisitor<void>): void {
        const stack: Markup[] = [document];
        while (stack.length > 0) {
            const node = stack.pop()!;
            visit(node, visitor);
            const descendants = children(node);
            for (let index = descendants.length - 1; index >= 0; index -= 1) {
                stack.push(descendants[index]!);
            }
        }
    }
}

/** `visitDocument` is required on every complete visitor and absent from
 * every Markup value and plain callback.
 *
 * Visitors may carry arbitrary state and may themselves be callable objects,
 * so visitor shape takes precedence whenever the overload domains overlap. */
function isVisitor(value: Markup | WalkCallback | MarkupVisitor<void>): value is MarkupVisitor<void> {
    return (
        (typeof value === "object" || typeof value === "function") &&
        value !== null &&
        "visitDocument" in value &&
        typeof value.visitDocument === "function"
    );
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
            return node.label === null ? node.content : [node.label, ...node.content];
        case "directiveLabel":
            return node.content;
        case "directive":
            return node.label === null ? [] : [node.label];
        case "linkReference":
        case "imageReference":
            return [...node.content];
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
        case "referenceDefinition":
        case "crossLink":
        case "embed":
            return [];
    }
    return unreachable(node);
}

function unreachable(value: never): never {
    throw new Error(`unreachable markup ${String(value)}`);
}
