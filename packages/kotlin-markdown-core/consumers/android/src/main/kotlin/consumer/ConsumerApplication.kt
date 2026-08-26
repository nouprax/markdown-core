package consumer

import android.app.Application
import com.nouprax.markdown.core.Document

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        check(Document("Android consumer\n").seal().semantic.content.size == 1)
    }
}
