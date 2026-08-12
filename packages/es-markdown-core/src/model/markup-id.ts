/**
 * Series-scoped node identity: `rawValue` is unique within the owning series
 * and never reused; `series` is that series' random 64-bit salt, so nodes
 * from different series (including separate one-shot parses) never share an
 * identity. Stable across edits while the node remains the same kind of thing
 * at the same place.
 *
 * A SERIES is one document and every document its edits produce. Raw values
 * restart at 1 for each new series, so the salt is the only thing keeping two
 * unrelated documents' identities apart.
 *
 * Two ids are the same identity exactly when their `series` and `rawValue`
 * are equal. Within one series the same identity is always the same object,
 * so ids are usable as `Map` keys and React-style list keys.
 */
export interface MarkupID {
    readonly series: bigint;
    readonly rawValue: number;
}
