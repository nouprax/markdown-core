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
