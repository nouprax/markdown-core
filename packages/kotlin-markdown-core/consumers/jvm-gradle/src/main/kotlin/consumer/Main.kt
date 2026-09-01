package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.TreeDumper

fun main() {
    val read = Document("héllo 🚀\n").seal()
    check(read.semantic.content.size == 1)
    check(read.dump() == TreeDumper.dump(read.semantic))
    check(read.semantic.scope.end.line >= 1)
}
