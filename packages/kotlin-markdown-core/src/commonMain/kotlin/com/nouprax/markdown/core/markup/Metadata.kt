package com.nouprax.markdown.core

/** A leaf Markup node holding ten optional fields and their complete source envelope. */
public class Metadata internal constructor(
    public val name: MetadataValue?,
    public val title: MetadataValue?,
    public val subtitle: MetadataValue?,
    public val time: MetadataValue?,
    public val date: MetadataValue?,
    public val authors: MetadataValue?,
    public val keywords: MetadataValue?,
    public val `abstract`: MetadataValue?,
    public val state: MetadataValue?,
    public val comment: MetadataValue?,
    override val id: MarkupID,
    override val extent: Extent,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()

public sealed interface MetadataValue {
    public data class Scalar(
        public val value: MetadataScalar,
    ) : MetadataValue

    public class List(
        items: kotlin.collections.List<MetadataListItem>,
    ) : MetadataValue {
        public val items: kotlin.collections.List<MetadataListItem> = items.immutableMap { it }

        override fun equals(other: Any?): Boolean = this === other || (other is List && items == other.items)

        override fun hashCode(): Int = items.hashCode()
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
