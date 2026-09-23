import fs from "node:fs";
import path from "node:path";

/** Read the descriptor definitions and the one product attach table together.
 * An audit must inspect every attached descriptor, and a descriptor must have
 * exactly one position. Neither condition depends on a fixed feature count.
 * Unsupported declaration shapes fail closed instead of disappearing from an
 * audit's inventory.
 */
export function parseElementInventory(sources) {
    const descriptors = new Map();
    for (const { file, source } of sources) {
        const declarations = [...source.matchAll(/\bconst\s+markdown_core_element\s+(MARKDOWN_CORE_ELEMENT_\w+)\s*=/g)];
        const definitions = [
            ...source.matchAll(/\bconst\s+markdown_core_element\s+(MARKDOWN_CORE_ELEMENT_\w+)\s*=\s*\{([\s\S]*?)^\};/gm)
        ];
        if (declarations.length !== definitions.length) {
            throw new Error(`${file}: could not read every element descriptor initializer`);
        }
        for (const [, symbol, body] of definitions) {
            if (descriptors.has(symbol)) throw new Error(`${symbol}: duplicate descriptor definition`);
            descriptors.set(symbol, { symbol, file, source, body });
        }
    }

    const attachSource = sources.find(({ file }) => file === "core-elements.c")?.source ?? "";
    const tables = [...attachSource.matchAll(/\bCORE_ELEMENTS\[\]\s*=\s*\{([\s\S]*?)\};/g)];
    if (tables.length !== 1) throw new Error("expected exactly one CORE_ELEMENTS[] table");
    const entries = tables[0][1].split(",").map((entry) => entry.trim());
    if (entries.at(-1) === "") entries.pop();
    if (entries.length === 0) throw new Error("CORE_ELEMENTS[] is empty");
    const ordered = [];
    const attached = new Set();
    for (const entry of entries) {
        const match = /^&\s*(MARKDOWN_CORE_ELEMENT_\w+)$/.exec(entry);
        if (match === null) throw new Error(`unreadable CORE_ELEMENTS[] entry: ${entry}`);
        const symbol = match[1];
        if (attached.has(symbol)) throw new Error(`${symbol}: duplicate CORE_ELEMENTS[] entry`);
        if (!descriptors.has(symbol)) throw new Error(`${symbol}: attached descriptor has no definition`);
        attached.add(symbol);
        ordered.push(descriptors.get(symbol));
    }
    for (const symbol of descriptors.keys()) {
        if (!attached.has(symbol)) throw new Error(`${symbol}: descriptor has no CORE_ELEMENTS[] entry`);
    }
    // These projections are indexed by semantic rule and default source byte
    // in the parser. A collision would otherwise silently change ownership
    // with attach order. Shared dispatch bytes remain valid: a scanner can
    // select a non-default rule, as the two tilde elements do.
    const rules = new Map();
    const characters = new Map();
    for (const { symbol, body } of ordered) {
        const rule = /\.delimiter_rule\s*=\s*(MARKDOWN_CORE_DELIM_RULE_\w+)/.exec(body)?.[1];
        const character = /\.delimiter_character\s*=\s*'([^'\\])'/.exec(body)?.[1];
        if (/\.delimiter_rule\s*=/.test(body) && !rule) {
            throw new Error(`${symbol}: unreadable delimiter rule`);
        }
        if (/\.delimiter_character\s*=/.test(body) && !character) {
            throw new Error(`${symbol}: default delimiter character must be one printable literal byte`);
        }
        if (character && !rule) throw new Error(`${symbol}: delimiter character has no rule`);
        if (rule) {
            if (/_(NONE|COUNT)$/.test(rule)) {
                throw new Error(`${symbol}: ${rule} is reserved by the engine`);
            }
            if (rules.has(rule)) throw new Error(`${symbol}: duplicate delimiter rule ${rule}`);
            rules.set(rule, symbol);
            const minimum = Number(/\.minimum_width\s*=\s*(\d+)/.exec(body)?.[1]);
            const maximum = Number(/\.maximum_width\s*=\s*(\d+)/.exec(body)?.[1]);
            if (!(minimum > 0 && maximum >= minimum)) {
                throw new Error(`${symbol}: invalid parsed delimiter widths`);
            }
        }
        if (character) {
            if (characters.has(character)) throw new Error(`${symbol}: duplicate default delimiter character`);
            characters.set(character, symbol);
        }
        if (/\.scan_block_start\s*=/.test(body) && !/\.maximum_block_indent\s*=\s*(?:\d+|INT_MAX)\b/.test(body)) {
            throw new Error(`${symbol}: block scanner requires an explicit indentation bound`);
        }
    }
    return { descriptors: [...descriptors.values()], ordered };
}

export function readElementInventory(directory) {
    const sources = fs
        .readdirSync(directory)
        .filter((file) => file.endsWith(".c"))
        .sort()
        .map((file) => ({ file, source: fs.readFileSync(path.join(directory, file), "utf8") }));
    return parseElementInventory(sources);
}
