package consumer;

import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.Markup;
import com.nouprax.markdown.core.MarkupDumper;
import com.nouprax.markdown.core.MarkupID;
import com.nouprax.markdown.core.ParseOptions;

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
        if (id.seriesBits() == 0L) {
            throw new IllegalStateException("series bits must carry the series salt");
        }
        MarkupID rebuilt = MarkupID.fromBits(id.seriesBits(), id.rawValueBits());
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

        // Editing from plain Java: (id, revision) is the entire update
        // protocol, so what changed is read off the successor tree itself.
        try (Document next = document.edit("héllo 🚀 world\n")) {
            if (next.revisionBits() == document.revisionBits()) {
                throw new IllegalStateException("a text change must advance the document revision");
            }
            Markup grown = next.getContent().get(0);
            if (!grown.getId().equals(paragraph.getId())) {
                throw new IllegalStateException("the surviving paragraph must keep its id across the edit");
            }
            if (grown.revisionBits() != next.revisionBits()) {
                throw new IllegalStateException("a changed node must carry the new document revision");
            }
            if (next.node(paragraph.getId()) == null) {
                throw new IllegalStateException("a held id must resolve in the successor");
            }
            next.getScope();
        }
    }
}
