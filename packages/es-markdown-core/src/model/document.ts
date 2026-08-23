import type { Concrete, Region } from "../concrete.js";
import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";

/**
 * A parse, under two total views.
 *
 * The document IS the semantic view -- the tree with policy applied, which may
 * omit bytes: a fence, a bullet and a reference definition's punctuation are in
 * no literal anywhere. `concrete` omits nothing. Every byte of the source is in
 * exactly one region of the concrete view and every region has exactly one
 * owner in this tree, so the pair is complete.
 *
 * In C the two are siblings, because a `markdown_core_document` is a handle and
 * the root is a node it lends out. Here they are not: the handle is gone by the
 * time `Document.parse` returns, the tree is a value, and the concrete view
 * hangs off the root it names into.
 */
export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
    readonly concrete: Concrete;

    /**
     * The node a region's `owner` path names, or `undefined` when the path
     * names no node in this tree.
     *
     * The path counts children the way the C tree holds them, and the value
     * tree splits some of those runs into named fields -- a directive's label
     * and its content, a table's header and its rows -- so descending it is not
     * `content[i]` at every step. This is the descent.
     */
    ownerOf(region: Region): Markup | undefined;
}
