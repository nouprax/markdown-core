import type { Document } from "../model/document.js";
import type { MarkupID } from "../model/markup-id.js";
import type { ParseOptions } from "../parse-options.js";
import { ScopeResolver } from "../session/scope-resolver.js";
import { adopt } from "../session/snapshot.js";
import { CSession } from "./c-session.js";

/**
 * One-shot parse as session sugar: open, one edit, one delta-free commit,
 * decode through the session pipeline, materialize scopes eagerly, free. No
 * session survives the call; nodes carry ids under the internal session's
 * lineage, so separate parses never share identity.
 */
export function parse(source: string, parseOptions: ParseOptions = {}): Document {
    if (typeof source !== "string") throw new TypeError("source must be a string");
    if (parseOptions === null || typeof parseOptions !== "object") {
        throw new TypeError("options must be an object");
    }
    const session = new CSession(parseOptions);
    try {
        if (source.length > 0) session.edit(0, 0, source);
        session.commit(false);
        const lineage = session.lineage();
        const identities = new Map<number, MarkupID>();
        const resolver = ScopeResolver.live(session);
        const document = session.decoder.decodeDocument(session.rootPointer(), {
            ids: (rawValue) => {
                let id = identities.get(rawValue);
                if (!id) {
                    id = { lineage, rawValue };
                    identities.set(rawValue, id);
                }
                return id;
            },
            adopt: (value) => adopt(value, resolver),
            mirror: null,
            touched: null
        });
        resolver.materialize();
        return document;
    } finally {
        session.free();
    }
}
