package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.TreeDumper
import kotlin.test.Test
import kotlin.test.assertEquals

class KmpConsumerTest {
    @Test
    fun rootMetadataSelectsTheJvmVariant() {
        val parsed = Document.parse("# KMP consumer\n")
        val document = parsed.semantic
        assertEquals(1, document.content.size)
        assertEquals(document.dump(), TreeDumper.dump(document))
        assertEquals(0, parsed.concrete.region(0).start)
    }
}
