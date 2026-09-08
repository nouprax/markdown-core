package com.nouprax.markdown.core

/** Ten optional fields and their complete source envelope; never Markup. */
public data class Metadata(
    public val name: MetadataValue? = null,
    public val title: MetadataValue? = null,
    public val subtitle: MetadataValue? = null,
    public val time: MetadataValue? = null,
    public val date: MetadataValue? = null,
    public val authors: MetadataValue? = null,
    public val keywords: MetadataValue? = null,
    public val `abstract`: MetadataValue? = null,
    public val state: MetadataValue? = null,
    public val comment: MetadataValue? = null,
    public val scope: Scope,
)

public sealed interface MetadataValue {
    public data class Scalar(
        public val value: MetadataScalar,
    ) : MetadataValue

    public class List(
        items: kotlin.collections.List<MetadataListItem>,
    ) : MetadataValue {
        public val items: kotlin.collections.List<MetadataListItem> = items.immutableMap { it }
    }
}

public sealed interface MetadataScalar {
    public data object Null : MetadataScalar

    public data class Bool(
        public val value: Boolean,
    ) : MetadataScalar

    public data class Number(
        public val value: String,
    ) : MetadataScalar

    public data class Text(
        public val value: String,
    ) : MetadataScalar
}

public sealed interface MetadataListItem {
    public data class Number(
        public val value: String,
    ) : MetadataListItem

    public data class Text(
        public val value: String,
    ) : MetadataListItem
}
