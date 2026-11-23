/** Source grammar belongs to elements. The drivers may depend on
 * structural roots and Text's shared source map, but cannot inspect another
 * element's payload, dispatch a spelling, spell a source byte or a word, or
 * name an element themselves.
 *
 * A BYTE THE ENGINE MAY SPELL: white space and the line endings, NUL and the
 * other escapes, the digits (the numbers it writes and reads, a name's
 * suffix) and the bounds of the ASCII letters (the case it folds labels
 * in). Any other byte is a grammar's, which its element declares. `kinds`,
 * when given, are the kind names (node_type.h) the kind rule knows; without
 * it every MARKDOWN_CORE_NODE_ name but the engine's own is one. */
const SPELLED = /^'( |\\.|[0-9]|[AZaz])'$/;
export function auditParserBoundaries(
    sources,
    { elementHeaders = [], syntaxScanners = [], descriptors = [], kinds = null } = {}
) {
    const failures = [];
    const headers = new Set(elementHeaders);
    const scanners = new Set(syntaxScanners);
    const named = new Set(descriptors);
    for (const { file, source } of sources) {
        const code = source.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "");
        for (const match of code.matchAll(/\bMARKDOWN_CORE_NODE_([A-Z][A-Z_]*)\b/g)) {
            const kind = match[1];
            // The structural roots, the type and value masks, and the kind
            // count are the engine's own vocabulary, not an element's kind.
            if (["NONE", "DOCUMENT", "TEXT", "VALUE_MASK", "KIND_COUNT"].includes(kind) || kind.startsWith("TYPE_")) {
                continue;
            }
            if (kinds && !kinds.has(kind)) continue;
            failures.push(`${file}: engine refers to element kind ${kind}`);
        }
        for (const [literal] of code.matchAll(/'(?:\\.|[^\\'\n])'/g)) {
            if (!SPELLED.test(literal)) failures.push(`${file}: engine spells the source byte ${literal}`);
        }
        for (const match of code.matchAll(/"(?:[^"\\\n]|\\.)*"/g)) {
            const line = code.slice(code.lastIndexOf("\n", match.index) + 1, match.index);
            if (!/^\s*#\s*include\b/.test(line)) failures.push(`${file}: engine spells the word ${match[0]}`);
        }
        for (const [name] of code.matchAll(/\bMARKDOWN_CORE_ELEMENT_[A-Z][A-Z_]*\b/g)) {
            if (named.has(name)) failures.push(`${file}: engine names the element ${name}`);
        }
        for (const [, name] of code.matchAll(/\b(_?scan_\w+)\s*\(/g)) {
            if (scanners.has(name)) failures.push(`${file}: engine invokes syntax scanner ${name}`);
        }
        if (/->as\.(?!literal\b|data\b)\w+/.test(code)) {
            failures.push(`${file}: engine inspects an element payload`);
        }
        if (/\bcase\s*'(?! |\\[rnt])/.test(code)) failures.push(`${file}: engine dispatches a source spelling`);
        for (const [, include] of code.matchAll(/#include\s*["<]([^">]+)[">]/g)) {
            if (headers.has(include.split("/").at(-1))) {
                failures.push(`${file}: engine includes element implementation interface ${include}`);
            }
        }
    }
    return failures;
}
