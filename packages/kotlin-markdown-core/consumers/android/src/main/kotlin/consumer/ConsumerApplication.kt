package consumer

import android.app.Application
import com.nouprax.markdown.core.Document

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        check(Document.parse("Android consumer\n").content.size == 1)
    }
}
