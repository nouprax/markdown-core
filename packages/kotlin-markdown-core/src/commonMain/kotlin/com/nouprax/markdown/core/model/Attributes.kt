package com.nouprax.markdown.core

public data class Record(
    public val name: String,
    public val value: String,
)

/** Ordered immutable values, including every duplicate. */
public class Attributes(
    classes: kotlin.collections.List<String>,
    records: kotlin.collections.List<Record>,
) {
    public val classes: kotlin.collections.List<String> = classes.immutableMap { it }
    public val records: kotlin.collections.List<Record> = records.immutableMap { it }

    public companion object {
        public val empty: Attributes = Attributes(emptyList(), emptyList())
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
    return Attributes(inherited.classes + classes, inherited.records + records)
}
