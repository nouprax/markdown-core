package consumer

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.Heading
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AndroidConsumerTest {
    @Test
    fun aarSelectsAndLoadsTheDeviceNativeLibrary() {
        val heading = Document.parse("# Android\n").content.single() as Heading
        assertEquals(1, heading.level)
    }
}
