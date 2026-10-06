package com.nouprax.markdown.core

/**
 * One owned node-valued field of a node: its nodes in stored order, and the
 * name of the group line the dump draws for it, or null when its nodes nest
 * directly under the owner.
 */
internal class Relation(
    val group: String?,
    val nodes: kotlin.collections.List<Markup>,
)

/**
 * THE CANONICAL WALK: depth first, each node's relations in canonical field
 * order, as the C facade's walk. Each [next] moves to one item: a node's
 * entry, a group line, or a node's exit. The walk's stack is a list of
 * frames, never the call stack, so its depth is the tree's.
 *
 * It also places every node it enters: a relation's first node starts
 * `lead` bytes after its owner's start and every later one `lead` bytes after
 * the end of the node before it. The root starts `lead` bytes after 0. A node
 * whose runs read content is an inline root: its first relation is its
 * content, which starts at 0, and every node anywhere in it is placed in that
 * content; the walk holds the root's runs to read the content's source from.
 * The root's later relations are back in source offsets. Roots never nest.
 */
internal class MarkupTraversal(
    root: Markup,
) {
    enum class Step { ENTER, GROUP, EXIT }

    private class Frame(
        val node: Markup,
        val level: Int,
        val start: Long,
        val end: Long,
        /** Whether the node is in an inline root's content. */
        val outer: Boolean,
    ) {
        val relations = node.relations()

        /** Whether the node is an inline root, whose first relation is its content. */
        val root = SourceRuns.readContent(node.runs)

        /** Whether the relation in hand is in an inline root's content. */
        var content = outer

        /** The last relation that draws a line at the owner's level: a group, or nodes. */
        val lastDrawn = relations.indexOfLast { it.group != null || it.nodes.isNotEmpty() }
        var relation: Relation? = null
        var next = 0
        var index = 0
        var anchor = start
    }

    private val frames = ArrayList<Frame>()
    private var first: Markup? = root

    /** The runs of the inline root whose content the walk is in, or was last. */
    private val runs = SourceRuns()

    /** The runs of the last node outside inline content asked for its places. */
    private val own = SourceRuns()
    private val places = SourcePlaces()

    lateinit var step: Step
        private set

    /** The node entered or exited, or null for a group line. */
    var node: Markup? = null
        private set

    /** A group line's name. */
    var group: String? = null
        private set

    /** A group line's node count. */
    var count: Int = 0
        private set

    /** The item's nesting level: 0 for the root, and one more under a group line. */
    var level: Int = 0
        private set

    /** Whether another line follows this one at its own level. */
    var more: Boolean = false
        private set

    /**
     * The byte range of the node entered or exited: absolute in the source,
     * or in its inline root's content when [content] says so.
     */
    var start: Long = 0
        private set
    var end: Long = 0
        private set

    /** Whether the node entered or exited is in an inline root's content. */
    var content: Boolean = false
        private set

    fun next(): Boolean {
        first?.let {
            first = null
            enter(it, 0, 0, false, false)
            return true
        }
        while (frames.isNotEmpty()) {
            val frame = frames[frames.lastIndex]
            val relation = frame.relation
            if (relation == null) {
                if (frame.next == frame.relations.size) {
                    frames.removeAt(frames.lastIndex)
                    item(Step.EXIT, frame.node, frame.level, false)
                    start = frame.start
                    end = frame.end
                    content = frame.outer
                    return true
                }
                // A root's first relation is its content, which starts at 0.
                val entered = frame.root && frame.next == 0
                val opened = frame.relations[frame.next++]
                frame.relation = opened
                frame.index = 0
                frame.anchor = if (entered) 0 else frame.start
                frame.content = frame.outer || entered
                // The group's own nodes follow it one level down, so what
                // follows it at its own level is the owner's next relation.
                if (opened.group != null) {
                    item(Step.GROUP, null, frame.level + 1, frame.next <= frame.lastDrawn)
                    group = opened.group
                    count = opened.nodes.size
                    return true
                }
                continue
            }
            if (frame.index < relation.nodes.size) {
                val child = relation.nodes[frame.index++]
                val direct = relation.group == null
                val more = frame.index < relation.nodes.size || (direct && frame.next <= frame.lastDrawn)
                enter(child, frame.anchor, frame.level + if (direct) 1 else 2, more, frame.content)
                frame.anchor = end
                return true
            }
            frame.relation = null
        }
        return false
    }

    private fun enter(
        child: Markup,
        anchor: Long,
        level: Int,
        more: Boolean,
        content: Boolean,
    ) {
        item(Step.ENTER, child, level, more)
        start = anchor + child.extent.lead
        end = start + child.extent.span.toLong()
        this.content = content
        // A root is never in content, so its start is a source offset.
        if (SourceRuns.readContent(child.runs)) runs.read(child.runs, start)
        frames += Frame(child, level, start, end, content)
    }

    /**
     * The source ranges of the node entered or exited, valid until the next
     * call: a window less the gaps between the runs that place it. In
     * content, the window is the source its content range was read from
     * through the root's runs; else it is its range, cut by its own runs.
     */
    fun places(): SourcePlaces {
        val node = node!!
        places.clear()
        if (content) {
            runs.content(start, end, places)
        } else {
            own.read(node.runs, start)
            own.cut(start, end, places)
        }
        return places
    }

    private fun item(
        step: Step,
        node: Markup?,
        level: Int,
        more: Boolean,
    ) {
        this.step = step
        this.node = node
        this.group = null
        this.count = 0
        this.level = level
        this.more = more
    }
}
