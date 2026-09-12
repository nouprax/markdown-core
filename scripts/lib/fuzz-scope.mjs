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
 * Custom task prefixes and definition markers in base-language paragraphs
 * likewise belong to their Obsidian/Pandoc gates, not to these base oracles.
 */
export function outsideSharedFuzzScope(input) {
    if (!/[[:~]/u.test(input)) return null;
    const pending = [commonmark.parse(input)];
    let heading = false;
    let unresolvedBracket = false;
    while (pending.length) {
        const node = pending.pop();
        // These extension envelopes belong to the Obsidian and Pandoc gates.
        // Inspect an independent base parse, never the product's recognized
        // kinds; opaque code/HTML cannot activate either boundary.
        if (node.type === "listItem" && node.children[0]?.type === "paragraph") {
            const first = node.children[0];
            const raw = input.slice(first.position.start.offset, first.position.end.offset);
            const marker = /^\[([^[\]\p{White_Space}])\][ \t\v\f]/u.exec(raw)?.[1];
            if (marker && ![" ", "x", "X"].includes(marker)) return "custom-task-markers";
        }
        if (node.type === "paragraph") {
            const raw = input.slice(node.position.start.offset, node.position.end.offset);
            if (/(?:^|\n)[^\r\n]+\r?\n(?:[ \t]*\r?\n)?[ \t]*[:~](?:[ \t]|\r?\n|$)/u.test(raw)) {
                return "definition-lists";
            }
        }
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
