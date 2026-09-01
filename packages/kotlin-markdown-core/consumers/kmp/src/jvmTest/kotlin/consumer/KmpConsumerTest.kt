package consumer

import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.TreeDumper
import kotlin.test.Test
import kotlin.test.assertEquals

class KmpConsumerTest {
    @Test
    fun rootMetadataSelectsTheJvmVariant() {
        val read = Document("# KMP consumer\n").seal()
        assertEquals(1, read.semantic.content.size)
        assertEquals(read.dump(), TreeDumper.dump(read.semantic))
        assertEquals(1, read.semantic.scope.start.line)
    }
}
