package consumer

import android.app.Application
import com.nouprax.markdown.core.Document

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        check(Document.parse("Android consumer\n").semantic.content.size == 1)
    }
}
