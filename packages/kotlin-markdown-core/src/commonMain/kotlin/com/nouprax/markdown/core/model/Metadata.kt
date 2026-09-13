package com.nouprax.markdown.core

/** A leaf Markup node holding ten optional fields and their complete source envelope. */
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
    override val scope: Scope,
    override val anchor: String? = null,
    override val attributes: Attributes = Attributes.empty,
) : Markup {
    override fun <Result> accept(
        visitor: Visitor<Result>,
        phase: MarkupWalkPhase,
    ): Result = visitor.visit(this, phase)
}

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
