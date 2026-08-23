package com.nouprax.markdown.core

/**
 * One parse under two total views.
 *
 * [semantic] is the tree with policy applied, which may omit bytes -- a fence,
 * a bullet and a reference definition's punctuation are in no literal anywhere.
 * [concrete] omits nothing. Every byte of the source is in exactly one region
 * of the concrete view and every region has exactly one owner in the semantic
 * one, so the pair is complete.
 */
public class Document internal constructor(
    public val semantic: DocumentRoot,
    public val concrete: Concrete,
) {
    /**
     * The node a region's [Region.owner] path names, or `null` when the path
     * names no node in this tree.
     *
     * The path counts children the way the C tree holds them, and the value
     * tree splits some of those runs into named fields -- a directive's label
     * and its content, a table's header and its rows -- so descending it is not
     * `content[i]` at every step. This is the descent.
     */
    public fun ownerOf(region: Region): Markup? {
        var node: Markup = semantic
        for (step in region.owner) {
            val children = node.children()
            if (step < 0 || step >= children.size) return null
            node = children[step]
        }
        return node
    }

    public companion object {
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document = WireDecoder.decodeDocument(nativeParse(source.encodeToByteArray(), options))
    }
}
