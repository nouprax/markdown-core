package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.TreeDumper

fun main() {
    val document = Document.parse("héllo 🚀\n")
    check(document.content.size == 1)
    check(document.dump() == TreeDumper.dump(document))
}
