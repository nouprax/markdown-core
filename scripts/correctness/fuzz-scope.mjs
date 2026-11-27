import { unified } from "unified";
import remarkParse from "remark-parse";
import remarkDirective from "remark-directive";

const commonmark = unified().use(remarkParse);
const directives = unified().use(remarkParse).use(remarkDirective);

/**
 * The dialect has no part-less text directive: `:name` is a directive only
 * when a bracketed label or an attribute container follows the name, and a
 * part that fails to scan leaves the colon as text. remark-directive accepts
 * the bare name, so wherever recombination or truncation strips or breaks a
 * part -- `:a[b]` cut to `:a`, `:a{x}` cut to `:a{x`, a label whose closer
 * lands past the cut -- the two disagree by design. The registry documents
 * the rule on the inputs people wrote; a generated input reaches the same
 * rule from a shape no entry can name, so the boundary is drawn on remark's
 * own parse: a text directive whose whole span is the colon and its name has
 * no part, and what it names is remark's, not the shared language.
 */
function partlessTextDirective(input) {
    const pending = [directives.parse(input)];
    while (pending.length) {
        const node = pending.pop();
        if (node.type === "textDirective") {
            const span = node.position.end.offset - node.position.start.offset;
            if (span === 1 + node.name.length) return true;
        }
        for (const child of node.children ?? []) pending.push(child);
    }
    return false;
}

/**
 * A line of two or more dash runs beside text is where the Pandoc table forms
 * begin: the runs are a simple table's column starts, or a multiline table's
 * segmented boundary. CommonMark reads the same line as a thematic break, a
 * list item or paragraph text, so the two disagree by design, and the Pandoc
 * gate owns the shape. Whether a table actually forms depends on lines the
 * line-local view cannot see (a body row, a matching footer, a closing
 * boundary), so the boundary is conservative: any such line next to a
 * non-blank line, after block quote markers, list markers and indentation
 * alike.
 */
const DASH_RUNS = /^(?:[ \t>]|[*+](?=[ \t])|\d{1,9}[.)](?=[ \t]))*-+(?:[ \t]+-+)+[ \t]*$/u;
const BLANK = /^[ \t>]*$/u;

function pandocTableSegments(input) {
    const lines = input.split(/\r?\n/u);
    return lines.some(
        (line, index) =>
            DASH_RUNS.test(line) &&
            ((index > 0 && !BLANK.test(lines[index - 1])) ||
                (index + 1 < lines.length && !BLANK.test(lines[index + 1])))
    );
}

/**
 * Custom task prefixes, definition markers in base-language paragraphs and
 * fenced-code attribute containers belong to their Obsidian/Pandoc gates, not
 * to these base oracles. Bound the
 * recombination domain using an independent CommonMark parse, never the
 * product's output.
 */
export function outsideSharedFuzzScope(input) {
    if (input.includes("-") && pandocTableSegments(input)) return "pandoc-tables";
    if (!/[[:{]/u.test(input)) return null;
    const pending = [commonmark.parse(input)];
    while (pending.length) {
        const node = pending.pop();
        // These extension envelopes belong to the Obsidian and Pandoc gates.
        // Inspect an independent base parse, never the product's recognized
        // kinds; opaque code/HTML cannot activate either boundary.
        if (node.type === "listItem" && node.children[0]?.type === "paragraph") {
            const first = node.children[0];
            const raw = input.slice(first.position.start.offset, first.position.end.offset);
            // The dialect's statuses beyond GFM's open and done.
            if (/^\[[?-]\][ \t]/u.test(raw)) return "custom-task-markers";
        }
        // A fence's attribute container is a dialect attachment site that
        // mdast and cmark have no counterpart for: the info's braces.
        if (node.type === "code" && /\{[^}]*\}[ \t]*$/u.test(`${node.lang ?? ""} ${node.meta ?? ""}`)) {
            return "fenced-code-attributes";
        }
        if (node.type === "paragraph") {
            // A later line of the slice keeps its container prefix: the `>`
            // of a quote the paragraph is in, and its indentation.
            const raw = input.slice(node.position.start.offset, node.position.end.offset);
            if (/(?:^|\n)[^\r\n]+\r?\n(?:[ \t>]*\r?\n)?[ \t>]*:(?:[ \t]|\r?\n|$)/u.test(raw)) {
                return "definition-lists";
            }
        }
        for (const child of node.children ?? []) pending.push(child);
    }
    if (input.includes(":") && partlessTextDirective(input)) return "part-less-text-directives";
    return null;
}
