package com.nouprax.markdown.core

/** A logical column. */
@ConsistentCopyVisibility
public data class TableColumn internal constructor(
    public val flow: Flow,
)

/** Inline content as parsed. */
public class TableCell internal constructor(
    public val content: kotlin.collections.List<Markup>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()

public class TableRow internal constructor(
    public val cells: kotlin.collections.List<TableCell>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()

public class Table internal constructor(
    public val columns: kotlin.collections.List<TableColumn>,
    public val head: kotlin.collections.List<TableRow>,
    public val content: kotlin.collections.List<TableRow>,
    override val id: MarkupID,
    override val extent: Extent,
    override val runs: kotlin.collections.List<Run>,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup()
