package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.MarkupDumper;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        // A Java caller reaches the one parse entry through the companion: the
        // dialect has no switches, so there is nothing else to pass.
        String source = "héllo 🚀\n";
        Document document = Document.Companion.parse(source);
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document.parse returned unexpected top-level content");
        }
        String dump = MarkupDumper.INSTANCE.dump(document, source);
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("JNI payload returned an unexpected document: " + dump);
        }
    }
}
