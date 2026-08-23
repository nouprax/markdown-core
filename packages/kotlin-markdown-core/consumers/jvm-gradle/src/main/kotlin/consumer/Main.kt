package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.TreeDumper

fun main() {
    val parsed = Document.parse("héllo 🚀\n")
    val document = parsed.semantic
    check(document.content.size == 1)
    check(document.dump() == TreeDumper.dump(document))
    check(parsed.concrete.source.size == "héllo 🚀\n".encodeToByteArray().size)
}
