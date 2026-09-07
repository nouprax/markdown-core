import fs from "node:fs";
import path from "node:path";

/** Read the descriptor definitions and the one product attach table together.
 * An audit must inspect every attached descriptor, and a descriptor must have
 * exactly one position. Neither condition depends on a fixed feature count.
 * Unsupported declaration shapes fail closed instead of disappearing from an
 * audit's inventory.
 */
export function parseExtensionInventory(sources) {
    const descriptors = new Map();
    for (const { file, source } of sources) {
        const declarations = [
            ...source.matchAll(/\bconst\s+markdown_core_extension\s+(MARKDOWN_CORE_EXTENSION_\w+)\s*=/g)
        ];
        const definitions = [
            ...source.matchAll(
                /\bconst\s+markdown_core_extension\s+(MARKDOWN_CORE_EXTENSION_\w+)\s*=\s*\{([\s\S]*?)^\};/gm
            )
        ];
        if (declarations.length !== definitions.length) {
            throw new Error(`${file}: could not read every extension descriptor initializer`);
        }
        for (const [, symbol, body] of definitions) {
            if (descriptors.has(symbol)) throw new Error(`${symbol}: duplicate descriptor definition`);
            descriptors.set(symbol, { symbol, file, source, body });
        }
    }

    const attachSource = sources.find(({ file }) => file === "core-extensions.c")?.source ?? "";
    const tables = [...attachSource.matchAll(/\bCORE_EXTENSIONS\[\]\s*=\s*\{([\s\S]*?)\};/g)];
    if (tables.length !== 1) throw new Error("expected exactly one CORE_EXTENSIONS[] table");
    const entries = tables[0][1].split(",").map((entry) => entry.trim());
    if (entries.at(-1) === "") entries.pop();
    if (entries.length === 0) throw new Error("CORE_EXTENSIONS[] is empty");
    const ordered = [];
    const attached = new Set();
    for (const entry of entries) {
        const match = /^&\s*(MARKDOWN_CORE_EXTENSION_\w+)$/.exec(entry);
        if (match === null) throw new Error(`unreadable CORE_EXTENSIONS[] entry: ${entry}`);
        const symbol = match[1];
        if (attached.has(symbol)) throw new Error(`${symbol}: duplicate CORE_EXTENSIONS[] entry`);
        if (!descriptors.has(symbol)) throw new Error(`${symbol}: attached descriptor has no definition`);
        attached.add(symbol);
        ordered.push(descriptors.get(symbol));
    }
    for (const symbol of descriptors.keys()) {
        if (!attached.has(symbol)) throw new Error(`${symbol}: descriptor has no CORE_EXTENSIONS[] entry`);
    }
    return { descriptors: [...descriptors.values()], ordered };
}

export function readExtensionInventory(directory) {
    const sources = fs
        .readdirSync(directory)
        .filter((file) => file.endsWith(".c"))
        .sort()
        .map((file) => ({ file, source: fs.readFileSync(path.join(directory, file), "utf8") }));
    return parseExtensionInventory(sources);
}
