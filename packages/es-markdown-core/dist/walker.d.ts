import type { Markup } from "./model/markup.js";
export declare const WalkEvent: {
    readonly entering: "entering";
    readonly exiting: "exiting";
};
export type WalkEvent = (typeof WalkEvent)[keyof typeof WalkEvent];
export declare class Walker {
    walk(root: Markup, callback: (event: WalkEvent, node: Markup) => void): void;
}
/** Every child of a node, in the order the C tree holds them. `Walker` walks
 * with it, so a walk and a hand-written descent cannot disagree about what a
 * node's children are. */
export declare function children(node: Markup): readonly Markup[];
