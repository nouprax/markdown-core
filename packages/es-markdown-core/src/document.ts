import type { Concrete, Region } from "./concrete.js";
import type { DocumentRoot } from "./model/document-root.js";
import type { Markup } from "./model/markup.js";
import type { ParseOptions } from "./parse-options.js";
import { parseDocument } from "./runtime/parser.js";
import { children } from "./walker.js";

/**
 * One parse under two total views.
 *
 * `semantic` is the tree with policy applied, which may omit bytes -- a fence,
 * a bullet and a reference definition's punctuation are in no literal anywhere.
 * `concrete` omits nothing. Every byte of the source is in exactly one region
 * of the concrete view and every region has exactly one owner in the semantic
 * one, so the pair is complete.
 */
export class Document {
    readonly semantic: DocumentRoot;
    readonly concrete: Concrete;

    constructor(semantic: DocumentRoot, concrete: Concrete) {
        this.semantic = semantic;
        this.concrete = concrete;
    }

    /**
     * The node a region's `owner` path names, or `undefined` when the path
     * names no node in this tree.
     *
     * The path counts children the way the C tree holds them, and the value
     * tree splits some of those runs into named fields -- a directive's label
     * and its content, a table's header and its rows -- so descending it is not
     * `content[i]` at every step. This is the descent.
     */
    ownerOf(region: Region): Markup | undefined {
        let node: Markup = this.semantic;
        for (const step of region.owner) {
            const descendants = children(node);
            if (step < 0 || step >= descendants.length) return undefined;
            node = descendants[step]!;
        }
        return node;
    }

    static parse(source: string, options?: ParseOptions): Document {
        const parsed = parseDocument(source, options);
        return new Document(parsed.semantic, parsed.concrete);
    }
}
