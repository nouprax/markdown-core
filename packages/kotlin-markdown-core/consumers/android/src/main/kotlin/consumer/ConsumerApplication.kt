package consumer

import android.app.Application
import com.nouprax.markdown.core.Document
import com.nouprax.markdown.core.MarkupSession
import com.nouprax.markdown.core.footnote
import com.nouprax.markdown.core.footnotes
import com.nouprax.markdown.core.references

class ConsumerApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        check(Document.parse("Android consumer\n").content.size == 1)
        MarkupSession().use { session ->
            session.append("Footnote[^a].\n\n[^a]: note\n")
            check(session.length > 0)
            val commit = session.commit()
            check(session.revision == 1UL)
            check(session.node(commit.document.id) === commit.document)
            commit.document.scope(commit.document)
            val definition = session.footnotes().single()
            check(session.footnote(definition.id) != null)
            check(session.references(definition.id).size == 1)
        }
    }
}

class UnusedConsumerApplication : Application()
