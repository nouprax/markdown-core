package com.nouprax.markdown.core

// A pending exit carries the container kind its enter dispatch found, so the
// exit visit is one table switch and a cast rather than a second walk down
// the `is` chain; a pending enter carries ENTER. A leaf's exit follows its
// enter at once, so leaves have no tag.
private const val ENTER = -1
private const val PARAGRAPH = 0
private const val EMPHASIS = 1
private const val STRONG = 2
private const val LINK = 3
private const val LIST_ITEM = 4
private const val LIST = 5
private const val HEADING = 6
private const val EMBEDDED = 7
private const val CALLOUT = 8
private const val TABLE = 9
private const val TABLE_ROW = 10
private const val TABLE_CELL = 11
private const val TABLE_CAPTION = 12
private const val STRIKETHROUGH = 13
private const val MARK = 14
private const val INSERTION = 15
private const val SPAN = 16
private const val SUPERSCRIPT = 17
private const val SUBSCRIPT = 18
private const val DIRECTIVE = 19
private const val DIRECTIVE_BLOCK = 20
private const val DIRECTIVE_LABEL = 21
private const val DEFINITION_LIST = 22
private const val DEFINITION = 23
private const val CITE = 24
private const val CITATION = 25
private const val FOOTNOTE = 26
private const val SPECIMEN = 27
private const val DOCUMENT = 28

/**
 * Walks markup depth first and reports both phases through typed callbacks.
 *
 * The pending visits are two parallel stacks -- the node, and its tag -- so a
 * visit costs no pair. A node's kind is found once, on enter, by an `is`
 * chain ordered with the common inline and block kinds first; a leaf's exit
 * follows its enter at once, and a container's exit waits on the stack under
 * its children with the kind already known.
 */
internal class MarkupWalker(
    private val visitor: MarkupVisitor,
) {
    private var nodes = arrayOfNulls<Markup>(64)
    private var tags = IntArray(64)
    private var count = 0

    fun walk(root: Markup) {
        push(root, ENTER)
        while (count > 0) {
            val index = --count
            val node = nodes[index]!!
            nodes[index] = null
            val tag = tags[index]
            if (tag == ENTER) enter(node) else exit(node, tag)
        }
    }

    private fun push(
        node: Markup,
        tag: Int,
    ) {
        val index = count
        if (index == nodes.size) {
            nodes = nodes.copyOf(index * 2)
            tags = tags.copyOf(index * 2)
        }
        nodes[index] = node
        tags[index] = tag
        count = index + 1
    }

    /** Schedules [content] so its first element is visited first. */
    private fun schedule(content: kotlin.collections.List<Markup>) {
        var index = content.size
        while (index > 0) push(content[--index], ENTER)
    }

    private fun enter(node: Markup) {
        when (node) {
            is Text -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is SoftBreak -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Paragraph -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, PARAGRAPH)
                schedule(node.content)
            }

            is Emphasis -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, EMPHASIS)
                schedule(node.content)
            }

            is Strong -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, STRONG)
                schedule(node.content)
            }

            is Code -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Link -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, LINK)
                schedule(node.content)
            }

            is ListItem -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, LIST_ITEM)
                schedule(node.content)
            }

            is List -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, LIST)
                schedule(node.items)
            }

            is Heading -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, HEADING)
                schedule(node.content)
            }

            is LineBreak -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Embedded -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, EMBEDDED)
                schedule(node.content)
            }

            is HTML -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is CodeBlock -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is ThematicBreak -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Callout -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, CALLOUT)
                schedule(node.content)
                node.title?.let(::schedule)
            }

            is Table -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, TABLE)
                schedule(node.foot)
                schedule(node.content)
                schedule(node.head)
                node.caption?.let { push(it, ENTER) }
            }

            is TableRow -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, TABLE_ROW)
                schedule(node.cells)
            }

            is TableCell -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, TABLE_CELL)
                schedule(node.content)
            }

            is TableCaption -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, TABLE_CAPTION)
                schedule(node.content)
            }

            is HTMLBlock -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Comment -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Strikethrough -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, STRIKETHROUGH)
                schedule(node.content)
            }

            is Mark -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, MARK)
                schedule(node.content)
            }

            is Insertion -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, INSERTION)
                schedule(node.content)
            }

            is Span -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, SPAN)
                schedule(node.content)
            }

            is Superscript -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, SUPERSCRIPT)
                schedule(node.content)
            }

            is Subscript -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, SUBSCRIPT)
                schedule(node.content)
            }

            is Formula -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is FormulaBlock -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is CrossLink -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is CrossEmbedded -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Directive -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DIRECTIVE)
                node.label?.let { push(it, ENTER) }
            }

            is DirectiveBlock -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DIRECTIVE_BLOCK)
                schedule(node.content)
                node.label?.let { push(it, ENTER) }
            }

            is DirectiveLabel -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DIRECTIVE_LABEL)
                schedule(node.content)
            }

            is DefinitionList -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DEFINITION_LIST)
                schedule(node.definitions)
            }

            is Definition -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DEFINITION)
                val bodies = node.content
                var index = bodies.size
                while (index > 0) schedule(bodies[--index])
                schedule(node.term)
            }

            is Cite -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, CITE)
                schedule(node.citations)
            }

            is Citation -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, CITATION)
                schedule(node.suffix)
                schedule(node.prefix)
            }

            is Footnote -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, FOOTNOTE)
                schedule(node.content)
            }

            is Specimen -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, SPECIMEN)
                schedule(node.content)
            }

            is Metadata -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                visitor.visit(node, MarkupVisitPhase.EXIT)
            }

            is Document -> {
                visitor.visit(node, MarkupVisitPhase.ENTER)
                push(node, DOCUMENT)
                schedule(node.specimens)
                schedule(node.footnotes)
                schedule(node.content)
                node.metadata?.let { push(it, ENTER) }
            }
        }
    }

    private fun exit(
        node: Markup,
        tag: Int,
    ) {
        when (tag) {
            PARAGRAPH -> visitor.visit(node as Paragraph, MarkupVisitPhase.EXIT)
            EMPHASIS -> visitor.visit(node as Emphasis, MarkupVisitPhase.EXIT)
            STRONG -> visitor.visit(node as Strong, MarkupVisitPhase.EXIT)
            LINK -> visitor.visit(node as Link, MarkupVisitPhase.EXIT)
            LIST_ITEM -> visitor.visit(node as ListItem, MarkupVisitPhase.EXIT)
            LIST -> visitor.visit(node as List, MarkupVisitPhase.EXIT)
            HEADING -> visitor.visit(node as Heading, MarkupVisitPhase.EXIT)
            EMBEDDED -> visitor.visit(node as Embedded, MarkupVisitPhase.EXIT)
            CALLOUT -> visitor.visit(node as Callout, MarkupVisitPhase.EXIT)
            TABLE -> visitor.visit(node as Table, MarkupVisitPhase.EXIT)
            TABLE_ROW -> visitor.visit(node as TableRow, MarkupVisitPhase.EXIT)
            TABLE_CELL -> visitor.visit(node as TableCell, MarkupVisitPhase.EXIT)
            TABLE_CAPTION -> visitor.visit(node as TableCaption, MarkupVisitPhase.EXIT)
            STRIKETHROUGH -> visitor.visit(node as Strikethrough, MarkupVisitPhase.EXIT)
            MARK -> visitor.visit(node as Mark, MarkupVisitPhase.EXIT)
            INSERTION -> visitor.visit(node as Insertion, MarkupVisitPhase.EXIT)
            SPAN -> visitor.visit(node as Span, MarkupVisitPhase.EXIT)
            SUPERSCRIPT -> visitor.visit(node as Superscript, MarkupVisitPhase.EXIT)
            SUBSCRIPT -> visitor.visit(node as Subscript, MarkupVisitPhase.EXIT)
            DIRECTIVE -> visitor.visit(node as Directive, MarkupVisitPhase.EXIT)
            DIRECTIVE_BLOCK -> visitor.visit(node as DirectiveBlock, MarkupVisitPhase.EXIT)
            DIRECTIVE_LABEL -> visitor.visit(node as DirectiveLabel, MarkupVisitPhase.EXIT)
            DEFINITION_LIST -> visitor.visit(node as DefinitionList, MarkupVisitPhase.EXIT)
            DEFINITION -> visitor.visit(node as Definition, MarkupVisitPhase.EXIT)
            CITE -> visitor.visit(node as Cite, MarkupVisitPhase.EXIT)
            CITATION -> visitor.visit(node as Citation, MarkupVisitPhase.EXIT)
            FOOTNOTE -> visitor.visit(node as Footnote, MarkupVisitPhase.EXIT)
            SPECIMEN -> visitor.visit(node as Specimen, MarkupVisitPhase.EXIT)
            DOCUMENT -> visitor.visit(node as Document, MarkupVisitPhase.EXIT)
            else -> error("invalid walker tag $tag")
        }
    }
}
