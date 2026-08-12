package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupDumper
import kotlin.test.Test
import kotlin.test.assertEquals

class KmpConsumerTest {
    @Test
    fun rootMetadataSelectsTheJvmVariant() {
        Document("# KMP consumer\n").use { document ->
            assertEquals(1, document.content.size)
            assertEquals(document.dump(), MarkupDumper.dump(document))
        }
    }
}
