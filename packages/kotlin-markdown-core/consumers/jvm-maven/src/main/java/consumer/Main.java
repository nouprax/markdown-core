package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.ParseOptions;
import com.nouprax.markdown.core.TreeDumper;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        ParseOptions options = new ParseOptions(
                true, true, true, true, true, true, true, true, true, true, true);
        Document document = Document.Companion.parse("héllo 🚀\n", options);
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document.parse returned unexpected top-level content");
        }
        String dump = TreeDumper.INSTANCE.dump(document);
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("native payload returned an unexpected document: " + dump);
        }
    }
}
