import type { Document } from "../model/document.js";
import type { Markup } from "../model/markup.js";
import { MarkupDumper } from "../markup-dumper.js";
import type { Scope } from "../values.js";
import type { DocumentValue } from "../wire/node-decoder.js";
import type { ScopeResolver } from "./scope-resolver.js";

/**
 * Completes a decoded document into a snapshot: the `scope` and `dump`
 * mediators are wired to the snapshot's resolver as non-enumerable
 * properties, keeping the node a plain value for enumeration and
 * serialization.
 */
export function adopt(value: DocumentValue, resolver: ScopeResolver): Document {
    const document = {
        kind: value.kind,
        id: value.id,
        revision: value.revision,
        content: value.content
    };
    Object.defineProperty(document, "scope", {
        enumerable: false,
        value(node: Markup): Scope {
            if (node.id.lineage !== value.id.lineage) {
                throw new Error("node belongs to a different session or parse");
            }
            const entry = resolver.entry(node.id.rawValue);
            if (entry === undefined) throw new Error("node does not belong to this snapshot");
            if (entry.revision !== node.revision) {
                throw new Error("node value is from a different revision of this snapshot's session");
            }
            return entry.scope;
        }
    });
    Object.defineProperty(document, "dump", {
        enumerable: false,
        value(this: Document): string {
            return MarkupDumper.dump(this);
        }
    });
    return document as Document;
}
