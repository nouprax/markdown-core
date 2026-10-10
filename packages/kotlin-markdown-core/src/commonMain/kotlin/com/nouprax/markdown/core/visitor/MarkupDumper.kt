package com.nouprax.markdown.core

/** Produces the canonical debug tree for immutable Markdown markup. */
public object MarkupDumper {
    /**
     * The canonical debug dump of [document], with scopes computed from [source] in UTF-8 columns.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source] ends before [document] does.
     */
    public fun dump(
        document: Document,
        source: String,
    ): String = dump(document, document, source)

    /**
     * The canonical debug dump of [node], a node of [document], and its owned
     * markup, with scopes computed from [source], the text the document was
     * parsed from, always in UTF-8 columns whatever the document's unit.
     *
     * @throws MarkdownCoreException [ErrorCode.OUT_OF_BOUNDS] when [source] ends before a node of the tree does.
     */
    public fun dump(
        document: Document,
        node: Markup,
        source: String,
    ): String = Tree(SourceLines(source)).dump(document, node)
}

/**
 * Draws one line per item of the canonical walk: a node's line, or a group
 * line naming a node-valued list. The walk's stack is the tree's depth, never
 * the call stack's. A node's source leads from where the source of the node
 * before it ends, so the walk starts at the document even for a node below
 * it.
 */
private class Tree(
    private val lines: SourceLines,
) {
    private val output = StringBuilder()
    private val prefix = StringBuilder()

    /** For each depth, whether its last line has a later sibling, and where its part of [prefix] ends. */
    private val more = ArrayList<Boolean>()
    private val ends = ArrayList<Int>()

    /** Draws the tree under [root], a node of [document], each line at its level below [root]. */
    fun dump(
        document: Document,
        root: Markup,
    ): String {
        val traversal = MarkupTraversal(document)
        // The level of [root] once the walk is at it, and nothing before.
        var base = -1
        while (traversal.next()) {
            if (base < 0) {
                if (traversal.step != MarkupTraversal.Step.ENTER || traversal.node !== root) continue
                base = traversal.level
            } else if (traversal.level <= base) {
                break
            }
            if (traversal.step == MarkupTraversal.Step.EXIT) continue
            connect(traversal.level - base, traversal.more)
            val node = traversal.node
            if (node == null) {
                output
                    .append(traversal.group)
                    .append(" children=")
                    .append(traversal.count)
                    .append('\n')
            } else {
                node(node, traversal.places())
            }
        }
        return output.toString()
    }

    /** The file-tree connectors that lead a line at [level]; the root's line has none. */
    private fun connect(
        level: Int,
        more: Boolean,
    ) {
        if (level == 0) return
        val depth = level - 1
        while (this.more.size <= depth) {
            this.more += false
            ends += 0
        }
        // Lines arrive depth first, so the prefix through the depth above is
        // this line's ancestors' own.
        this.more[depth] = more
        prefix.setLength(if (depth == 0) 0 else ends[depth - 1])
        output.append(prefix).append(if (more) "├── " else "└── ")
        prefix.append(if (more) "│   " else "    ")
        ends[depth] = prefix.length
    }

    /** A node's line, with a scope for each of its source ranges, which [lines] must cover. */
    private fun node(
        node: Markup,
        places: SourcePlaces,
    ) {
        if (places.end(places.count - 1) > lines.bytes.size) throw MarkdownCoreException(ErrorCode.OUT_OF_BOUNDS)
        val line = describe(node)
        output.append(line.kind).append(" scope=")
        for (index in 0 until places.count) {
            if (index > 0) output.append(',')
            output.append(scope(lines.scope(places.start(index).toInt(), places.end(index).toInt(), TextUnit.UTF8)))
        }
        output
            .append(" anchor=")
            .append(optional(node.anchor))
            .append(" attributes=")
            .append(attributes(node.attributes))
        for (field in line.fields) output.append(' ').append(field)
        output.append(" children=").append(line.children).append('\n')
    }
}

/** A node line's kind, its kind-specific fields in contract order, and its structural child count. */
private class Line(
    val kind: String,
    val children: Int,
    val fields: kotlin.collections.List<String> = emptyList(),
)

private fun describe(node: Markup): Line =
    when (node) {
        is Document -> {
            Line("Document", node.content.size)
        }

        is Metadata -> {
            Line(
                "Metadata",
                0,
                listOf(
                    "name=${node.name?.let(::metadataValue) ?: "null"}",
                    "title=${node.title?.let(::metadataValue) ?: "null"}",
                    "subtitle=${node.subtitle?.let(::metadataValue) ?: "null"}",
                    "time=${node.time?.let(::metadataValue) ?: "null"}",
                    "date=${node.date?.let(::metadataValue) ?: "null"}",
                    "authors=${node.authors?.let(::metadataValue) ?: "null"}",
                    "keywords=${node.keywords?.let(::metadataValue) ?: "null"}",
                    "abstract=${node.`abstract`?.let(::metadataValue) ?: "null"}",
                    "state=${node.state?.let(::metadataValue) ?: "null"}",
                    "comment=${node.comment?.let(::metadataValue) ?: "null"}",
                ),
            )
        }

        is Footnote -> {
            Line("Footnote", node.content.size, listOf("label=${optional(node.label)}"))
        }

        is Specimen -> {
            Line(
                "Specimen",
                node.content.size,
                listOf("label=${optional(node.label)}", "start=${node.start ?: "null"}"),
            )
        }

        is Callout -> {
            Line(
                "Callout",
                node.content.size,
                listOf("variant=${optional(node.variant)}", "collapsed=${node.collapsed ?: "null"}"),
            )
        }

        is Paragraph -> {
            Line("Paragraph", node.content.size)
        }

        is Heading -> {
            Line("Heading", node.content.size, listOf("level=${node.level}"))
        }

        is ThematicBreak -> {
            Line("ThematicBreak", 0)
        }

        is List -> {
            Line(
                "List",
                node.items.size,
                listOf(
                    "flavor=${node.flavor.token()}",
                    "start=${node.start ?: "null"}",
                    "variant=${node.variant?.token() ?: "null"}",
                    "delimiter=${node.delimiter?.token() ?: "null"}",
                    "tight=${node.tight}",
                ),
            )
        }

        is ListItem -> {
            Line("ListItem", node.content.size, listOf("marker=${optional(node.marker)}"))
        }

        is CodeBlock -> {
            Line(
                "CodeBlock",
                0,
                listOf(
                    "info=${optional(node.info)}",
                    "language=${optional(node.language)}",
                    "literal=${escaped(node.literal)}",
                    "fenced=${node.fenced}",
                    "closed=${node.closed}",
                ),
            )
        }

        is HTMLBlock -> {
            Line("HTMLBlock", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is FormulaBlock -> {
            Line("FormulaBlock", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is Table -> {
            val columns =
                node.columns.joinToString(",") { "${it.flow.token()}:${it.relative?.let(::decimal) ?: "null"}" }
            Line("Table", node.head.size + node.content.size + node.foot.size, listOf("columns=[$columns]"))
        }

        is TableCaption -> {
            Line("TableCaption", node.content.size)
        }

        is TableRow -> {
            Line("TableRow", node.cells.size)
        }

        is TableCell -> {
            Line("TableCell", node.content.size, listOf("rowspan=${node.rowspan}", "colspan=${node.colspan}"))
        }

        is DirectiveBlock -> {
            Line("DirectiveBlock", node.content.size, listOf("name=${optional(node.name)}"))
        }

        is DirectiveLabel -> {
            Line("DirectiveLabel", node.content.size)
        }

        is Text -> {
            Line("Text", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is SoftBreak -> {
            Line("SoftBreak", 0)
        }

        is LineBreak -> {
            Line("LineBreak", 0)
        }

        is Code -> {
            Line("Code", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is HTML -> {
            Line("HTML", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is CrossLink -> {
            Line("CrossLink", 0, listOf("dest=${destination(node.dest)}", "label=${optional(node.label)}"))
        }

        is CrossEmbedded -> {
            Line(
                "CrossEmbedded",
                0,
                listOf(
                    "dest=${destination(node.dest)}",
                    "label=${optional(node.label)}",
                    "dimensions=${dimensions(node.dimensions)}",
                ),
            )
        }

        is Comment -> {
            Line("Comment", 0, listOf("literal=${escaped(node.literal)}"))
        }

        is Formula -> {
            Line("Formula", 0, listOf("mode=${node.mode.token()}", "literal=${escaped(node.literal)}"))
        }

        is Emphasis -> {
            Line("Emphasis", node.content.size)
        }

        is Strong -> {
            Line("Strong", node.content.size)
        }

        is Strikethrough -> {
            Line("Strikethrough", node.content.size)
        }

        is Mark -> {
            Line("Mark", node.content.size)
        }

        is Insertion -> {
            Line("Insertion", node.content.size)
        }

        is Span -> {
            Line("Span", node.content.size)
        }

        is Superscript -> {
            Line("Superscript", node.content.size)
        }

        is Subscript -> {
            Line("Subscript", node.content.size)
        }

        is DefinitionList -> {
            Line("DefinitionList", node.definitions.size)
        }

        is Definition -> {
            Line("Definition", node.content.size, listOf("compact=${node.compact}"))
        }

        is Link -> {
            Line(
                "Link",
                node.content.size,
                listOf("dest=${destination(node.dest)}", "title=${optional(node.title)}"),
            )
        }

        is Embedded -> {
            Line(
                "Embedded",
                node.content.size,
                listOf(
                    "dest=${destination(node.dest)}",
                    "title=${optional(node.title)}",
                    "dimensions=${dimensions(node.dimensions)}",
                ),
            )
        }

        is Reference -> {
            Line(
                "Reference",
                0,
                listOf(
                    "label=${escaped(node.label)}",
                    "dest=${destination(node.dest)}",
                    "title=${optional(node.title)}",
                ),
            )
        }

        is Directive -> {
            Line("Directive", 0, listOf("name=${escaped(node.name)}"))
        }

        is Cite -> {
            Line("Cite", node.citations.size)
        }

        is Citation -> {
            Line("Citation", 0, listOf("referent=${referent(node.referent)}"))
        }
    }

private fun scope(value: Scope): String =
    "${value.start.line}:${value.start.column}..${value.end.line}:${value.end.column}"

private fun optional(value: String?): String = value?.let(::escaped) ?: "null"

/** A tagged value prints as its branch name applied to its fields, in declaration order. */
private fun destination(value: Destination): String =
    when (value) {
        is Destination.Url -> "url(${escaped(value.value)})"
        is Destination.Cross -> "cross(path=${escaped(value.path)},anchor=${optional(value.anchor)})"
        is Destination.Reference -> "reference(${escaped(value.label)})"
    }

private fun referent(value: CitationReferent): String =
    when (value) {
        is CitationReferent.Bib -> {
            "bib(key=${escaped(value.key)},mode=${value.mode.token()})"
        }

        is CitationReferent.Footnote -> {
            when (val target = value.target) {
                is FootnoteTarget.Label -> "footnote(label=${escaped(target.value)})"
                is FootnoteTarget.Note -> "footnote(note)"
            }
        }

        is CitationReferent.Specimen -> {
            "specimen(label=${escaped(value.label)})"
        }
    }

private fun BibMode.token(): String =
    when (this) {
        BibMode.NORMAL -> "normal"
        BibMode.AUTHOR_IN_TEXT -> "authorInText"
        BibMode.SUPPRESS_AUTHOR -> "suppressAuthor"
    }

private fun Placement.token(): String = name.lowercase()

private fun ListFlavor.token(): String = name.lowercase()

private fun OrderedListVariant.token(): String =
    when (this) {
        OrderedListVariant.Decimal -> "decimal"
        is OrderedListVariant.Alpha -> "alpha(lowercased=$lowercased)"
        is OrderedListVariant.Roman -> "roman(lowercased=$lowercased)"
        OrderedListVariant.Default -> "default"
    }

private fun OrderedListDelimiter.token(): String =
    when (this) {
        OrderedListDelimiter.Period -> "period"
        is OrderedListDelimiter.Parenthesis -> "parenthesis(closed=$closed)"
        OrderedListDelimiter.Default -> "default"
    }

private fun Flow.token(): String = name.lowercase()

private fun escaped(value: String): String =
    buildString {
        append('"')
        value.forEach { character ->
            when (character) {
                '"' -> {
                    append("\\\"")
                }

                '\\' -> {
                    append("\\\\")
                }

                '\b' -> {
                    append("\\b")
                }

                '\u000c' -> {
                    append("\\f")
                }

                '\n' -> {
                    append("\\n")
                }

                '\r' -> {
                    append("\\r")
                }

                '\t' -> {
                    append("\\t")
                }

                else -> {
                    if (character.code < 0x20) {
                        append("\\u")
                        append(character.code.toString(16).padStart(4, '0'))
                    } else {
                        append(character)
                    }
                }
            }
        }
        append('"')
    }

/** Emit shortest round-trip digits with the same notation thresholds on every runtime. */
private fun decimal(value: Double): String {
    val parts = value.toString().lowercase().split('e')
    val mantissa = parts[0].split('.')
    var digits = mantissa.joinToString("")
    var point = mantissa[0].length + if (parts.size == 2) parts[1].toInt() else 0
    while (digits.startsWith('0')) {
        digits = digits.drop(1)
        point--
    }
    digits = digits.trimEnd('0')
    // Some runtimes print two significant digits for subnormal values where
    // one already round-trips. Test rounded prefixes, preserving the value.
    for (length in 1 until digits.length) {
        val rounded = digits.take(length).toLong() + if (digits[length] >= '5') 1 else 0
        val candidate = rounded.toString()
        if ((candidate + "e" + (point - length)).toDouble() == value) {
            point += candidate.length - length
            digits = candidate.trimEnd('0')
            break
        }
    }
    if (point <= -6 || point > 21) {
        return digits.take(1) + (if (digits.length > 1) "." + digits.drop(1) else "") +
            "e" + (if (point > 0) "+" else "") + (point - 1)
    }
    if (point <= 0) return "0." + "0".repeat(-point) + digits
    if (point >= digits.length) return digits + "0".repeat(point - digits.length)
    return digits.take(point) + "." + digits.drop(point)
}

private fun attributes(value: Attributes): String =
    (
        value.classes.map {
            "." + identifier(it)
        } + value.records.map { "${it.name}=${escaped(it.value)}" }
    ).joinToString(" ", "{", "}")

private fun metadataValue(value: MetadataValue): String =
    when (value) {
        is MetadataValue.Scalar -> {
            "scalar(" +
                when (val scalar = value.value) {
                    MetadataScalar.Null -> "null"
                    is MetadataScalar.Bool -> "bool(${scalar.value})"
                    is MetadataScalar.Number -> "number(${escaped(scalar.value)})"
                    is MetadataScalar.Text -> "text(${escaped(scalar.value)})"
                } + ")"
        }

        is MetadataValue.List -> {
            "list([" +
                value.items.joinToString(",") { item ->
                    when (item) {
                        is MetadataListItem.Number -> "number(${escaped(item.value)})"
                        is MetadataListItem.Text -> "text(${escaped(item.value)})"
                    }
                } + "])"
        }
    }

private fun identifier(value: String): String {
    val plain =
        value.isNotEmpty() &&
            value.all { it.code in 33..126 && it.code !in listOf(34, 92, 123, 125, 91, 93, 40, 41, 61) }
    return if (plain) value else escaped(value)
}

private fun dimensions(value: Dimensions?): String =
    value?.let { "(width=${it.width},height=${it.height ?: "null"})" } ?: "null"
