package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupDumper
import kotlin.test.Test
import kotlin.test.assertEquals

class KmpConsumerTest {
    @Test
    fun rootMetadataSelectsTheJvmVariant() {
        val source = "# KMP consumer\n"
        val document = Document.parse(source)
        assertEquals(1, document.content.size)
        assertEquals(document.dump(source), MarkupDumper.dump(document, source))
    }
}
