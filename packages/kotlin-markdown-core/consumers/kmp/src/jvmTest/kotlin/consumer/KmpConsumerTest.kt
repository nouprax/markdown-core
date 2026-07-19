package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupDumper
import kotlin.test.Test
import kotlin.test.assertEquals

class KmpConsumerTest {
    @Test
    fun rootMetadataSelectsTheJvmVariant() {
        val document = Document.parse("# KMP consumer\n")
        assertEquals(1, document.content.size)
        assertEquals(document.dump(), MarkupDumper.dump(document))
    }
}
