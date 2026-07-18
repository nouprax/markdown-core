/**
 * Session-scoped node identity: `rawValue` is unique within the owning
 * session and never reused; `lineage` is the session's random 64-bit salt, so
 * nodes from different sessions (including separate one-shot parses) never
 * share an identity. Stable across incremental commits while the node remains
 * the same kind of thing at the same place.
 *
 * Two ids are the same identity exactly when their `lineage` and `rawValue`
 * are equal. Within one session (or one parse) the same identity is always
 * the same object, so ids are usable as `Map` keys and React-style list keys.
 */
export interface MarkupID {
    readonly lineage: bigint;
    readonly rawValue: number;
}
