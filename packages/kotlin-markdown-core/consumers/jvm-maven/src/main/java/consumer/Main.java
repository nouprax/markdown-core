package consumer;

import com.nouprax.markdown.core.Commit;
import com.nouprax.markdown.core.Diff;
import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.Markup;
import com.nouprax.markdown.core.MarkupDumper;
import com.nouprax.markdown.core.MarkupID;
import com.nouprax.markdown.core.ParseOptions;
import java.util.List;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        ParseOptions options = new ParseOptions();
        Document document = new Document("héllo 🚀\n", options);
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document returned unexpected top-level content");
        }
        String dump = MarkupDumper.INSTANCE.dump(document);
        if (!dump.contains("héllo 🚀")) {
            throw new IllegalStateException("native payload returned an unexpected document: " + dump);
        }

        // Identity and revision facade: plain Java reads the unsigned APIs
        // through their bit-preserving signed views.
        Markup paragraph = document.getContent().get(0);
        MarkupID id = paragraph.getId();
        if (id.lineageBits() == 0L) {
            throw new IllegalStateException("lineage bits must carry the lineage salt");
        }
        MarkupID rebuilt = MarkupID.fromBits(id.lineageBits(), id.rawValueBits());
        if (!rebuilt.equals(id)) {
            throw new IllegalStateException("MarkupID.fromBits must round-trip the identity");
        }
        if (document.revisionBits() != paragraph.revisionBits()) {
            // Both were minted by the same parse, which is revision zero: a
            // revision counts the edits a node has survived, and this one has
            // survived none.
            throw new IllegalStateException("one parse must mint every node at one revision");
        }
        if (document.revisionBits() != 0L) {
            throw new IllegalStateException("an unedited parse is revision zero");
        }

        // Editing and deltas from plain Java.
        Commit commit = document.edit("héllo 🚀 world\n");
        try (Document next = commit.getDocument()) {
            if (commit.getDelta().beforeRevisionBits() != document.revisionBits()
                    || commit.getDelta().afterRevisionBits() != next.revisionBits()) {
                throw new IllegalStateException("delta revision bits must bracket the edit");
            }
            List<Diff> diffs = commit.getDelta().getDiffs();
            if (diffs.isEmpty()) {
                throw new IllegalStateException("a text change must report a delta");
            }
            Diff last = diffs.get(diffs.size() - 1);
            if (!last.getMarkup().equals(next.getId()) || !last.getParts().getDescendant()) {
                throw new IllegalStateException("the document root must close a postorder delta");
            }
            for (Diff diff : diffs) {
                if (!diff.getParts().isRetired() && next.node(diff.getMarkup()) == null) {
                    throw new IllegalStateException("every surviving delta id must resolve");
                }
            }
            next.scope(next);
        }
    }
}
