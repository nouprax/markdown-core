package com.nouprax.markdown.core

public data class Record(
    public val name: String,
    public val value: String,
)

/** Ordered immutable values, including every duplicate. */
public class Attributes private constructor(
    public val classes: kotlin.collections.List<String>,
    public val records: kotlin.collections.List<Record>,
    @Suppress("UNUSED_PARAMETER") owned: Boolean,
) {
    public constructor(
        classes: kotlin.collections.List<String>,
        records: kotlin.collections.List<Record>,
    ) : this(classes.immutableMap { it }, records.immutableMap { it }, true)

    public companion object {
        public val empty: Attributes = Attributes(emptyOwnedList(), emptyOwnedList(), true)

        /**
         * Adopts lists a decoder built and will not touch again: the lists are
         * the fields, not copied into new ones. Empty attributes are [empty].
         */
        internal fun owned(
            classes: kotlin.collections.List<String>,
            records: kotlin.collections.List<Record>,
        ): Attributes = if (classes.isEmpty() && records.isEmpty()) empty else Attributes(classes, records, true)
    }
}

/** Definition values decoded once; no C handle survives materialization. */
internal data class DefinitionResource(
    val dest: Destination,
    val title: String?,
    val anchor: String?,
    val attributes: Attributes,
)

internal fun Attributes.inheriting(inherited: Attributes): Attributes {
    if (classes.isEmpty() && records.isEmpty()) return inherited
    if (inherited.classes.isEmpty() && inherited.records.isEmpty()) return this
    return Attributes(inherited.classes + classes, inherited.records + records)
}
