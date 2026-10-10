package com.nouprax.markdown.core

/**
 * One owned node-valued field of a node: its nodes in stored order, and the
 * name of the group line the dump draws for it, or null when its nodes nest
 * directly under the owner. A relation that [continues] the one before it (a
 * table's body and foot rows go on from its head rows) leads its first node
 * from the end of that one's last.
 */
internal class Relation(
    val group: String?,
    val nodes: kotlin.collections.List<Markup>,
    val continues: Boolean = false,
)

/**
 * THE CANONICAL WALK: depth first, each node's relations in canonical field
 * order, as the C facade's walk. Each [next] moves to one item: a node's
 * entry, a group line, or a node's exit. The walk's stack is a list of
 * frames, never the call stack, so its depth is the tree's.
 *
 * It also places the source of every node it enters: a relation's first
 * node leads from the start of its owner's source, or from the end of the
 * relation before when it continues that one, and every later one from the
 * end of the source of the node before it; the root leads from 0. A
 * node's source runs from where its first run starts to where its last ends.
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
    ) {
        val relations = node.relations()

        /** The last relation that draws a line at the owner's level: a group, or nodes. */
        val lastDrawn = relations.indexOfLast { it.group != null || it.nodes.isNotEmpty() }
        var relation: Relation? = null
        var next = 0
        var index = 0
        var anchor = start
    }

    private val frames = ArrayList<Frame>()
    private var first: Markup? = root

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

    /** The absolute source of the node entered or exited. */
    var start: Long = 0
        private set
    var end: Long = 0
        private set

    fun next(): Boolean {
        first?.let {
            first = null
            enter(it, 0, 0, false)
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
                    return true
                }
                val opened = frame.relations[frame.next++]
                frame.relation = opened
                frame.index = 0
                if (!opened.continues) {
                    frame.anchor = frame.start
                }
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
                enter(child, frame.anchor, frame.level + if (direct) 1 else 2, more)
                frame.anchor = end
                return true
            }
            frame.relation = null
        }
        return false
    }

    /**
     * Enters [child], whose source leads from [anchor]: from where its first
     * run starts to where its last ends.
     */
    private fun enter(
        child: Markup,
        anchor: Long,
        level: Int,
        more: Boolean,
    ) {
        item(Step.ENTER, child, level, more)
        val runs = child.runs
        var at = anchor
        for (index in runs.indices) {
            at += runs[index].lead
            if (index == 0) start = at
            at += runs[index].span.toLong()
        }
        end = at
        frames += Frame(child, level, start, end)
    }

    /**
     * The source ranges of the node entered or exited, valid until the next
     * call: its runs.
     */
    fun places(): SourcePlaces {
        val runs = node!!.runs
        places.clear()
        var at = start - runs[0].lead
        for (run in runs) {
            val from = at + run.lead
            at = from + run.span.toLong()
            places.add(from, at)
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
