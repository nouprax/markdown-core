package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupDumper

fun main() {
    val source = "héllo 🚀\n"
    val document = Document.parse(source)
    check(document.content.size == 1)
    check(document.dump(source) == MarkupDumper.dump(document, source))
}
