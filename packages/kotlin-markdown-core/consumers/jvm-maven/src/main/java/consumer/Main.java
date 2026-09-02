package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.ParseOptions;
import com.nouprax.markdown.core.TreeDumper;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        // A Java caller has no default arguments, so this positional call pins
        // the complete nine-option constructor from outside Kotlin. Formula
        // syntax has one gate rather than delimiter-specific sub-options.
        ParseOptions options = new ParseOptions(
                true, true, true, true, true, true, true, true, true);
        Document document = Document.Companion.parse("héllo 🚀\n", options);
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document.parse returned unexpected top-level content");
        }
        String dump = TreeDumper.INSTANCE.dump(document);
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("JNI payload returned an unexpected document: " + dump);
        }
    }
}
