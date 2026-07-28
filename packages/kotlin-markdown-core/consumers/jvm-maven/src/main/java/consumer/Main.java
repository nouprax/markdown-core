package consumer;

import com.nouprax.markdown.core.Commit;
import com.nouprax.markdown.core.Document;
import com.nouprax.markdown.core.FootnoteDefinition;
import com.nouprax.markdown.core.FootnoteInfo;
import com.nouprax.markdown.core.FootnoteQueriesKt;
import com.nouprax.markdown.core.Markup;
import com.nouprax.markdown.core.MarkupDumper;
import com.nouprax.markdown.core.MarkupID;
import com.nouprax.markdown.core.MarkupSession;
import com.nouprax.markdown.core.ParseOptions;
import java.util.List;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        ParseOptions options = new ParseOptions();
        Document document = Document.parse("héllo 🚀\n", options);
        if (document.getContent().size() != 1) {
            throw new IllegalStateException("Document.parse returned unexpected top-level content");
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
            throw new IllegalStateException("lineage bits must carry the session salt");
        }
        MarkupID rebuilt = MarkupID.fromBits(id.lineageBits(), id.rawValueBits());
        if (!rebuilt.equals(id)) {
            throw new IllegalStateException("MarkupID.fromBits must round-trip the identity");
        }
        if (paragraph.revisionBits() <= 0L) {
            throw new IllegalStateException("a parsed node must carry a positive revision");
        }
        if (document.revisionBits() != paragraph.revisionBits()) {
            // Both were minted by the same single commit.
            throw new IllegalStateException("one-shot parse must commit every node at one revision");
        }

        // Sessions, deltas, and footnote queries from plain Java.
        try (MarkupSession session = new MarkupSession()) {
            session.append("See [^n].\n\n[^n]: note\n");
            Commit commit = session.commit();
            if (session.lineageBits() == 0L) {
                throw new IllegalStateException("session lineage bits must be nonzero");
            }
            if (commit.getDelta().beforeRevisionBits() != 0L
                    || commit.getDelta().afterRevisionBits() != session.revisionBits()) {
                throw new IllegalStateException("delta revision bits must bracket the commit");
            }
            if (commit.getDelta().getAdded().isEmpty()) {
                throw new IllegalStateException("first commit must report added nodes");
            }
            MarkupID first = commit.getDelta().getAdded().get(0);
            if (session.node(first) == null) {
                throw new IllegalStateException("session.node must resolve a delta id");
            }
            List<FootnoteDefinition> footnotes = FootnoteQueriesKt.footnotes(session);
            if (footnotes.size() != 1 || !"n".equals(footnotes.get(0).getLabel())) {
                throw new IllegalStateException("footnote query must list the winning definition");
            }
            FootnoteInfo info = FootnoteQueriesKt.footnote(session, footnotes.get(0).getId());
            if (info == null || info.getNumber() == null || info.getNumber() != 1) {
                throw new IllegalStateException("footnote info must number the definition");
            }
            if (FootnoteQueriesKt.references(session, footnotes.get(0).getId()).size() != 1) {
                throw new IllegalStateException("footnote back-references must list the reference");
            }
        }
    }
}
