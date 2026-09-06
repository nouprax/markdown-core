package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.TreeDumper;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        // A Java caller reaches the one parse entry through the companion: the
        // dialect has no switches, so there is nothing else to pass.
        Document document = Document.Companion.parse("héllo 🚀\n");
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document.parse returned unexpected top-level content");
        }
        String dump = TreeDumper.INSTANCE.dump(document);
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("JNI payload returned an unexpected document: " + dump);
        }
    }
}
