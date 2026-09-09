import { unified } from "unified";
import remarkParse from "remark-parse";

const commonmark = unified().use(remarkParse);

/**
 * cmark, cmark-gfm and remark do not implement implicit heading references.
 * Bound their recombination domain using an independent CommonMark parse,
 * never the product's output. A heading beside unresolved bracket text can
 * activate P4; this composition belongs to the anchors fixtures and Pandoc
 * oracle. This is conservative: it does not try to implement P4 label matching.
 * Resolved explicit references and brackets in opaque literals stay in scope.
 */
export function outsideSharedFuzzScope(input) {
    if (!input.includes("[")) return null;
    const pending = [commonmark.parse(input)];
    let heading = false;
    let unresolvedBracket = false;
    while (pending.length) {
        const node = pending.pop();
        heading ||= node.type === "heading";
        const literal = node.type === "text" ? node.value : node.type === "image" ? node.alt : null;
        if (literal?.includes("[")) {
            const raw = input.slice(node.position.start.offset, node.position.end.offset);
            unresolvedBracket ||= raw.includes("[");
        }
        if (heading && unresolvedBracket) return "implicit-heading-references";
        for (const child of node.children ?? []) pending.push(child);
    }
    return null;
}
