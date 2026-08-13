package com.nouprax.markdown.core

/**
 * The shared streaming corpus: a three-turn LLM conversation and the one
 * fixed generator that cuts it into messages. The turn text and the
 * generator constants are byte-identical in the Swift and ES mirrors of the
 * streaming tests, so all three bindings replay the same bursts — a corpus
 * change here is a corpus change in three repositories' worth of tests.
 */
internal object StreamingCorpus {
    /** Three assistant turns extending one document. Blocks settled at a
     * turn boundary must stay frozen while later turns stream. */
    val turns =
        listOf(
            "# Streaming\n\nThe *quick* parser holds **steady** under bursts, " +
                "and the heading keeps its identity from the first render on.\n\n" +
                "Deltas stay proportional to what changed, so a renderer " +
                "reconciles by id instead of walking the whole tree.\n\n" +
                "> Snapshots are values: whatever a tick captured stays valid " +
                "while the socket races ahead.",
            "\n\n- append per message\n- edit per tick\n- settled blocks stay frozen" +
                "\n- identical items stress identity\n- identical items stress identity" +
                "\n\n```swift\nlet constant = 1\nlet mirror = [Int: String]()\n" +
                "for index in 0..<3 {\n    print(index, constant)\n}\n```\n\n" +
                "Fenced code arrives line by line and only closes at the final tick.",
            "\n\nA table lands late in the conversation:\n\n" +
                "| stage | commits | messages |\n| - | - | - |\n| one | 3 | 9 |\n" +
                "| two | 5 | 14 |\n| three | 8 | 21 |\n\n" +
                "Tail with a footnote[^n] whose definition arrives last.\n\n" +
                "[^n]: Resolved at the end, after every reference already rendered.",
        )

    /** One fixed generator for batch sizes and tick timing, so the burst
     * shapes are irregular but reproducible — and identical in the Swift
     * and ES mirrors. */
    class Splits {
        private var state = 0x9E3779B97F4A7C15UL.toLong()

        fun draw(bound: Long): Long {
            state = state * 6364136223846793005L + 1442695040888963407L
            return (state ushr 33) % bound
        }

        /** Mostly a 20-30 token batch (80-150 characters), with occasional
         * tiny flushes of a few words. Cuts land at raw character offsets —
         * mid-word, mid-marker, even between the two newlines of a block
         * boundary — because that is the steady state of LLM output. */
        fun width(): Int = if (draw(10L) < 2L) 2 + draw(18L).toInt() else 80 + draw(71L).toInt()
    }
}

/** [source] cut after every newline — the per-line replay stride the C
 * harness and the AST replays share. */
internal fun lineChunks(source: String): kotlin.collections.List<String> {
    val chunks = mutableListOf<String>()
    val current = StringBuilder()
    for (character in source) {
        current.append(character)
        if (character == '\n') {
            chunks += current.toString()
            current.clear()
        }
    }
    if (current.isNotEmpty()) {
        chunks += current.toString()
    }
    return chunks
}
