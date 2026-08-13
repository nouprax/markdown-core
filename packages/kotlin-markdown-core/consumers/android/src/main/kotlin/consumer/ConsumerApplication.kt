package consumer

import android.app.Application
import com.nouprax.markdown.core.Document

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        Document("Android consumer\n").use { document ->
            check(document.content.size == 1)
            document.edit("Android consumer, edited\n").use { next ->
                check(next.revision > document.revision)
                check(next.node(next.id) === next)
                next.scope
            }
        }
    }
}

class UnusedConsumerApplication : Application()
