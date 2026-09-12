/** Source grammar belongs to element extensions. The drivers may depend on
 * structural roots and Text's shared source map, but cannot inspect another
 * element's payload or dispatch a spelling themselves. */
export function auditParserBoundaries(sources, { elementHeaders = [], syntaxScanners = [] } = {}) {
    const failures = [];
    const headers = new Set(elementHeaders);
    const scanners = new Set(syntaxScanners);
    for (const { file, source } of sources) {
        const code = source.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, "");
        for (const match of code.matchAll(/\bMARKDOWN_CORE_NODE_([A-Z][A-Z_]*)\b/g)) {
            const kind = match[1];
            if (["NONE", "DOCUMENT", "TEXT"].includes(kind) || kind.startsWith("TYPE_")) continue;
            failures.push(`${file}: engine refers to element kind ${kind}`);
        }
        for (const [, name] of code.matchAll(/\b(_?scan_\w+)\s*\(/g)) {
            if (scanners.has(name)) failures.push(`${file}: engine invokes syntax scanner ${name}`);
        }
        if (/->as\.(?!literal\b)\w+/.test(code)) {
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
