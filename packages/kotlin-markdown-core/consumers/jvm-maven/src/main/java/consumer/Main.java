package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.ParseOptions;
import com.nouprax.markdown.core.Read;
import com.nouprax.markdown.core.TreeDumper;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        // Nine, not eleven: Step 6 deleted dollarFormulaDelimiters and
        // latexFormulaDelimiters, because attaching `formulas` is the only
        // switch the extension has. A Java caller has no default arguments, so
        // this positional call is what pins the arity from outside Kotlin.
        ParseOptions options = new ParseOptions(
                true, true, true, true, true, true, true, true, true);
        Read read = new Document("héllo 🚀\n", options).seal();
        if (read.getSemantic().getContent().size() != 1) {
            throw new IllegalStateException("the sealed read returned unexpected top-level content");
        }
        if (read.getConcrete().getSource().length != "héllo 🚀\n".getBytes().length) {
            throw new IllegalStateException("the concrete view did not survive the copy");
        }
        String dump = TreeDumper.INSTANCE.dump(read.getSemantic());
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("native payload returned an unexpected document: " + dump);
        }
    }
}
