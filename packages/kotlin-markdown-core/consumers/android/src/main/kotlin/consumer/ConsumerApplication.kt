package consumer

import android.app.Application
import com.nouprax.markdown.core.Document

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        Document("Android consumer\n").use { document ->
            check(document.content.size == 1)
            // Both mutations, so the release shrinker keeps every JNI export
            // an application path reaches - which is all of them.
            document.append("streamed\n").use { streamed ->
                check(streamed.revision > document.revision)
                streamed.edit("Android consumer, edited\n").use { next ->
                    check(next.revision > streamed.revision)
                    check(next.node(next.id) === next)
                    next.scope
                }
            }
        }
    }
}

class UnusedConsumerApplication : Application()
