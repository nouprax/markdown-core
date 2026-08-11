package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupDumper

fun main() {
    Document("héllo 🚀\n").use { document ->
        check(document.content.size == 1)
        check(document.dump() == MarkupDumper.dump(document))
    }
}
