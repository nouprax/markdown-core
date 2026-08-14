/**
 * Series-scoped node identity, stable across appends while the node remains
 * the same kind of thing at the same place.
 *
 * `rawValue` is unique within the owning series and never reused; `series` is
 * that series' random 64-bit salt, so nodes from different series (including
 * separate one-shot parses) never share an identity.
 *
 * A SERIES is one document and every document its appends produce — a chain
 * grows one way, and replacing the text is a new document (new chain, new
 * series). Raw values restart at 1 for each new series, so the salt is the
 * only thing keeping two unrelated documents' identities apart.
 *
 * Two ids are the same identity exactly when their `series` and `rawValue`
 * are equal. Within one series the same identity is always the same object,
 * so ids a document handed out are usable as `Map` keys and React-style list
 * keys. An id rebuilt from JSON or `structuredClone` is a new object with the
 * same fields: `document.node` accepts it, reference equality does not.
 */
export interface MarkupID {
    /**
     * The salt's 64 bits as 16 lowercase hex digits.
     *
     * The contract calls a series' identity OPAQUE, and nothing here does
     * arithmetic on it — it is assigned, compared, and used as a key. Of the
     * two JavaScript types that carry 64 bits without losing any, `bigint` is
     * the one `JSON.stringify` throws on, so this is the other. Fixed width,
     * so two salts order the same way their numbers would.
     */
    readonly series: string;
    /** A `number` and not a `bigint`: raw values start at 1 and count up, and
     * the decoder throws rather than hand back one that has outgrown exact
     * integer precision. */
    readonly rawValue: number;
}
