import type { Markup } from "../markup/markup.js";

/**
 * Deep value equality including `id`: two nodes are equal when they have the
 * same kind, id, extent, scalar fields and pairwise equal children in every
 * relation. Nodes and values are plain objects, so this is one structural
 * comparison of every enumerable field, run over an explicit work stack: depth
 * is data, not call stack. An object shared by both sides, such as a
 * definition's destination, is compared once. The comparator React.memo takes
 * for a node; a list keys the same nodes by `id`.
 */
export function markupEquals(a: Markup, b: Markup): boolean {
    const pending: unknown[] = [a, b];
    while (pending.length > 0) {
        const right = pending.pop();
        const left = pending.pop();
        if (Object.is(left, right)) continue;
        if (typeof left !== "object" || typeof right !== "object" || left === null || right === null) return false;
        if (Array.isArray(left)) {
            if (!Array.isArray(right) || left.length !== right.length) return false;
            for (let index = left.length - 1; index >= 0; index -= 1) pending.push(left[index], right[index]);
            continue;
        }
        if (Array.isArray(right)) return false;
        const keys = Object.keys(left);
        if (keys.length !== Object.keys(right).length) return false;
        for (const key of keys) {
            if (!Object.hasOwn(right, key)) return false;
            pending.push(
                (left as { readonly [key: string]: unknown })[key],
                (right as { readonly [key: string]: unknown })[key]
            );
        }
    }
    return true;
}
