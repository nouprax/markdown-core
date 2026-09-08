package com.nouprax.markdown.core

/** Scoped Properties values; they are never Markup or visitor events. */
public class Metadata(
    content: kotlin.collections.List<MetadataContent>,
    public val scope: Scope,
) {
    public val content: kotlin.collections.List<MetadataContent> = content.immutableMap { it }
}

/** Metadata content is a value; neither branch is Markup or receives visitor callbacks. */
public sealed interface MetadataContent {
    public data class Comment(
        public val value: String,
    ) : MetadataContent

    public data class Data(
        public val record: MetadataRecord,
    ) : MetadataContent
}

public data class MetadataRecord(
    public val name: String,
    public val value: MetadataValue,
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
