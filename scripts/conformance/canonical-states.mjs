/**
 * THE DECLARED GRAMMAR STATES, AS PREDICATES OVER A CANONICAL DUMP.
 *
 * `specs/canonical-ast/manifest.json` lists the states its conformance corpus
 * must demonstrate, and `scripts/conformance/check-canonical-ast.mjs` holds that
 * corpus to them. The BENCHMARK corpus has to answer a different question
 * against the same vocabulary -- which states it reaches, and whether the case
 * that reaches one has a same-job ratio or only a bound -- and the two must ask
 * it with ONE set of predicates. A second copy would drift, and a state whose
 * two readings disagreed would be a coverage number nobody could act on.
 *
 * The key ORDER is part of the contract: the fixture checker compares these
 * keys against the manifest's state list element by element, so a reordering
 * here is a failure there.
 */
/**
 * The kind of every node and of its parent, read from the connectors: a line's
 * depth is the column of its connector, and its parent is the nearest line
 * above at the depth before it. The dump nests a `DirectiveLabel` under its
 * directive the same way, so the label reads as that directive's child here,
 * which is what a placement question needs.
 */
function parentEdges(tree) {
    const byDepth = ["Document"];
    const edges = [];
    for (const line of tree.split("\n")) {
        if (!line.length) continue;
        const marker = line.search(/[\u251c\u2514]/);
        const depth = marker < 0 ? 0 : marker / 4 + 1;
        const kind = (marker < 0 ? line : line.slice(marker + 4)).split(" ", 1)[0];
        if (depth > 0) edges.push({ kind, parent: byDepth[depth - 1] });
        byDepth[depth] = kind;
        byDepth.length = depth + 1;
    }
    return edges;
}
// A direct Footnote child can be inline or block content, so that parent
// alone cannot witness a comment placement. Use unambiguous content owners.
const BLOCK_CONTENT = new Set(["Document", "Callout", "ListItem", "Specimen", "DirectiveBlock"]);
const INLINE_CONTENT = new Set([
    "Paragraph",
    "Heading",
    "TableCell",
    "DirectiveLabel",
    "Emphasis",
    "Strong",
    "Strikethrough",
    "Link",
    "Embedded"
]);

function taskMarkers(tree) {
    return [...tree.matchAll(/^.*ListItem scope=.* marker=("(?:[^"\\]|\\.)*") /gm)].map((match) =>
        JSON.parse(match[1])
    );
}

const stateValidators = {
    "directiveBlock.name.null": (tree) => /DirectiveBlock scope=.* name=null /.test(tree),
    "definition.compact.true": (tree) => /Definition scope=.* compact=true /.test(tree),
    "definition.compact.false": (tree) => /Definition scope=.* compact=false /.test(tree),
    "definition.bodies.multiple": (tree) => /Definition scope=.* children=(?:[2-9]|[1-9]\d+)(?:\n|$)/.test(tree),
    "definition.body.empty": (tree) => /DefinitionBody children=0/.test(tree),
    "crossEmbedded.dimensions.width": (tree) =>
        /CrossEmbedded scope=.* dimensions=\(width=[1-9][0-9]*,height=null\) /.test(tree),
    "crossEmbedded.dimensions.width-height": (tree) =>
        /CrossEmbedded scope=.* dimensions=\(width=[1-9][0-9]*,height=[1-9][0-9]*\) /.test(tree),
    "crossEmbedded.dimensions.null": (tree) => /CrossEmbedded scope=.* dimensions=null /.test(tree),
    "crossLink.label.null": (tree) => /CrossLink scope=.* label=null /.test(tree),
    "crossLink.label.empty": (tree) => /CrossLink scope=.* label="" /.test(tree),
    "crossLink.label.value": (tree) => /CrossLink scope=.* label="[^"\n]+" /.test(tree),
    "crossEmbedded.label.null": (tree) => /CrossEmbedded scope=.* label=null /.test(tree),
    "crossEmbedded.label.empty": (tree) => /CrossEmbedded scope=.* label="" /.test(tree),
    "crossEmbedded.label.value": (tree) => /CrossEmbedded scope=.* label="[^"\n]+" /.test(tree),
    "destination.cross.path": (tree) => / dest=cross\(path="[^"\n]+",anchor=/.test(tree),
    "destination.cross.current-document": (tree) => / dest=cross\(path="",anchor="/.test(tree),
    "destination.cross.anchor.null": (tree) => / dest=cross\(path="[^"\n]*",anchor=null\)/.test(tree),
    "destination.cross.anchor.value": (tree) => / dest=cross\(path="[^"\n]*",anchor="[^"\n]+"\)/.test(tree),
    "placement.embedded": (tree) => / mode=embedded /.test(tree),
    "placement.standalone": (tree) => / mode=standalone /.test(tree),
    "list.flavor.bullet": (tree) => /^.*List scope=.* flavor=bullet /m.test(tree),
    "list.flavor.ordered": (tree) => /^.*List scope=.* flavor=ordered /m.test(tree),
    "list.start.null": (tree) => /^.*List scope=.* start=null /m.test(tree),
    "list.start.value": (tree) => /^.*List scope=.* start=-?\d+ /m.test(tree),
    "list.tight.false": (tree) => /^.*List scope=.* tight=false /m.test(tree),
    "list.tight.true": (tree) => /^.*List scope=.* tight=true /m.test(tree),
    "listItem.marker.null": (tree) => /^.*ListItem scope=.* marker=null /m.test(tree),
    "listItem.marker.space": (tree) => /^.*ListItem scope=.* marker=" " /m.test(tree),
    "listItem.marker.value": (tree) => taskMarkers(tree).some((marker) => [...marker].length === 1 && marker !== " "),
    "listItem.marker.custom": (tree) =>
        taskMarkers(tree).some((marker) => [...marker].length === 1 && ![" ", "x", "X"].includes(marker)),
    "list.variant.decimal": (tree) => /^.*List scope=.* variant=decimal /m.test(tree),
    "list.variant.null": (tree) => /^.*List scope=.* variant=null /m.test(tree),
    "list.delimiter.period": (tree) => /^.*List scope=.* delimiter=period /m.test(tree),
    "list.delimiter.parenthesis.unclosed": (tree) =>
        /^.*List scope=.* delimiter=parenthesis\(closed=false\) /m.test(tree),
    "codeBlock.info.null": (tree) => /^.*CodeBlock scope=.* info=null /m.test(tree),
    "codeBlock.info.value": (tree) => /^.*CodeBlock scope=.* info="/m.test(tree),
    "codeBlock.language.null": (tree) => /^.*CodeBlock scope=.* language=null /m.test(tree),
    "codeBlock.language.value": (tree) => /^.*CodeBlock scope=.* language="/m.test(tree),
    "codeBlock.fenced.false": (tree) => /^.*CodeBlock scope=.* fenced=false /m.test(tree),
    "codeBlock.fenced.true": (tree) => /^.*CodeBlock scope=.* fenced=true /m.test(tree),
    "codeBlock.closed.false": (tree) => /^.*CodeBlock scope=.* closed=false /m.test(tree),
    "codeBlock.closed.true": (tree) => /^.*CodeBlock scope=.* closed=true /m.test(tree),
    "table.caption.null": (tree) => /Table scope=.+\n[^\n]*TableHead/.test(tree),
    "table.caption.populated": (tree) => /TableCaption scope=.* children=[1-9]/.test(tree),
    "table.caption.empty": (tree) => /TableCaption scope=.* children=0/.test(tree),
    "table.column.relative.value": (tree) => /Table scope=.* columns=\[[^\]]*:[0-9]/.test(tree),
    "tableCell.span.rows": (tree) => /TableCell scope=.* rowspan=[2-9]/.test(tree),
    "tableCell.span.columns": (tree) => /TableCell scope=.* colspan=[2-9]/.test(tree),
    "tableCell.content.block": (tree) =>
        parentEdges(tree).some((edge) => edge.parent === "TableCell" && edge.kind === "Paragraph"),
    "tableCell.content.empty": (tree) => /TableCell scope=.* children=0/.test(tree),
    "tableRow.cells.empty": (tree) => /TableRow scope=.* children=0/.test(tree),
    "table.head.empty": (tree) => /TableHead children=0/.test(tree),
    "table.foot.populated": (tree) => /TableFoot children=[1-9]/.test(tree),
    "table.column.relative.null": (tree) => /Table scope=.* columns=\[(?:[a-z]+:null)(?:,[a-z]+:null)*\]/.test(tree),
    "tableCell.span.one": (tree) => /TableCell scope=.* rowspan=1 colspan=1 /.test(tree),
    "tableCell.content.inline": (tree) =>
        parentEdges(tree).some((edge) => edge.parent === "TableCell" && edge.kind === "Text"),
    "table.alignment.none": (tree) => /^.*Table scope=.*columns=\[[^\]]*none[^\]]*\]/m.test(tree),
    "table.alignment.left": (tree) => /^.*Table scope=.*columns=\[[^\]]*left[^\]]*\]/m.test(tree),
    "table.alignment.center": (tree) => /^.*Table scope=.*columns=\[[^\]]*center[^\]]*\]/m.test(tree),
    "table.alignment.right": (tree) => /^.*Table scope=.*columns=\[[^\]]*right[^\]]*\]/m.test(tree),
    "callout.variant.null": (tree) => /^.*Callout scope=\S+ anchor=null attributes=\{\} variant=null /m.test(tree),
    "callout.variant.value": (tree) => /^.*Callout scope=.* variant="[^"]+" /m.test(tree),
    "callout.collapsed.false": (tree) => /^.*Callout scope=.* collapsed=false /m.test(tree),
    "callout.collapsed.true": (tree) => /^.*Callout scope=.* collapsed=true /m.test(tree),
    "callout.title.populated": (tree) => /Title children=[1-9]\d*/.test(tree),
    "callout.collapsed.null": (tree) => /^.*Callout scope=.* collapsed=null /m.test(tree),
    "callout.title.null": (tree) => /^.*Callout scope=/m.test(tree) && !/Title children=/.test(tree),
    "markup.anchor.null": (tree) => / anchor=null /.test(tree),
    "markup.anchor.value": (tree) => / anchor="[^"\n]+" /.test(tree),
    "markup.attributes.empty": (tree) => / attributes=\{\} /.test(tree),
    "markup.attributes.classes": (tree) => / attributes=\{\./.test(tree),
    "markup.attributes.records": (tree) => / attributes=\{[^}]*[A-Za-z]+="/.test(tree),
    "document.metadata.present": (tree) => /Metadata scope=/.test(tree),
    "document.metadata.empty": (tree) =>
        /Metadata scope=\S+ anchor=null attributes=\{\}(?: [a-z]+=null){10} children=0/.test(tree),
    "metadata.fields.populated": (tree) => /Metadata scope=.*=(?:scalar|list)\(/.test(tree),
    "metadata.scalar.null": (tree) => /Metadata scope=.*[a-z]=scalar\(null\)/.test(tree),
    "metadata.scalar.bool": (tree) => /Metadata scope=.*[a-z]=scalar\(bool\(/.test(tree),
    "metadata.scalar.number": (tree) => /Metadata scope=.*[a-z]=scalar\(number\(/.test(tree),
    "metadata.scalar.text": (tree) => /Metadata scope=.*[a-z]=scalar\(text\(/.test(tree),
    "metadata.list.empty": (tree) => /Metadata scope=.*[a-z]=list\(\[\]\)/.test(tree),
    "metadata.list.populated": (tree) => /Metadata scope=.*[a-z]=list\(\[(?:text|number)\(/.test(tree),
    "document.metadata.null": (tree) => !/Metadata scope=/.test(tree),
    "embedded.dimensions.width": (tree) => /Embedded scope=.* dimensions=\(width=[1-9][0-9]*,height=null\) /.test(tree),
    "embedded.dimensions.width-height": (tree) =>
        /Embedded scope=.* dimensions=\(width=[1-9][0-9]*,height=[1-9][0-9]*\) /.test(tree),
    "embedded.dimensions.null": (tree) => /Embedded scope=.* dimensions=null /.test(tree),
    // The dump visualizes the DirectiveLabel field as a nested Markup node:
    // absent emits no label node, empty has `children=0`, and populated owns
    // inline descendants.
    "directive.label.null": (tree) =>
        /^(.*)Directive(?:Block)? scope=[^\n]*\n(?!\1(?:\u2502|\|)?\s*(?:\u251c|\u2514)\u2500\u2500 DirectiveLabel )/m.test(
            tree
        ),
    "directive.label.empty": (tree) => /DirectiveLabel scope=\S+ anchor=null attributes=\{\} children=0$/m.test(tree),
    "directive.label.populated": (tree) =>
        /DirectiveLabel scope=\S+ anchor=null attributes=\{\} children=[1-9]\d*$/m.test(tree),
    // M2: a reference occurrence is the `Link` or `Embedded` it names, and dumps
    // identically to a direct one apart from scope. The case holds one direct
    // and several reference occurrences of each kind, so every `Link` line and
    // every `Embedded` line, scope removed, must be one line.
    "reference.resolution.identical": (tree) =>
        ["Link", "Embedded"].every((kind) => {
            const lines = tree
                .split("\n")
                .filter((line) => new RegExp(`(?:^|\u2500 )${kind} scope=`).test(line))
                .map((line) => line.replace(/^.*?(?= scope=)/, "").replace(/ scope=\S+/, ""));
            return lines.length >= 2 && new Set(lines).size === 1;
        }),
    "link.title.null": (tree) => /^.*Link scope=.* title=null /m.test(tree),
    "link.title.empty": (tree) => /^.*Link scope=.* title="" /m.test(tree),
    "link.title.value": (tree) => /^.*Link scope=.* title=".+" /m.test(tree),
    "embedded.title.null": (tree) => /^.*Embedded scope=.* title=null /m.test(tree),
    "embedded.title.value": (tree) => /^.*Embedded scope=.* title=".+" /m.test(tree),
    "scope.positive": (tree) => / scope=[1-9]\d*:[1-9]\d*\.\./.test(tree),
    /* `scope.zero` was here, and it required the canonical corpus to demonstrate
       a node with NO position -- 0:0..0:0. Its only two witnesses in that corpus
       were the LineBreak and the SoftBreak in inlines.ast, and 0a.12b gave both
       of them a real position (D26). The remaining producers of that shape are
       D13's empty Text and the split-off table lead, and pinning either as
       canonical coverage would bless a defect the stage is closing -- which is
       a defect rather than canonical behavior. The state is therefore deleted
       rather than re-witnessed. This is a coverage obligation,
       not a grammar or schema change: the dump still permits 0:0..0:0, so no
       binding and no golden format moves. */
    "children.empty": (tree) => / children=0(?:\n|$)/.test(tree),
    "children.populated": (tree) => / children=[1-9]\d*(?:\n|$)/.test(tree),
    "escaping.empty-string": (tree) => /=""/.test(tree),
    "escaping.newline": (tree) => /\\n/.test(tree),
    // An attribute VALUE that contains a quote. It was called `escaping.json`
    // when the whole attribute map was one JSON string; the escaping it checks
    // is the dump's, and that is what it was always about.
    "escaping.attribute-value": (tree) => /attributes=\{[^\n]*="[^\n]*\\"/.test(tree),
    // `Comment` is the one kind valid in both block and inline content, and
    // the parent edge is what records which (M0).
    "comment.placement.block": (tree) =>
        parentEdges(tree).some((edge) => edge.kind === "Comment" && BLOCK_CONTENT.has(edge.parent)),
    "comment.placement.inline": (tree) =>
        parentEdges(tree).some((edge) => edge.kind === "Comment" && INLINE_CONTENT.has(edge.parent)),
    // `dest` is the tagged `Destination` value (M1). `[a]()` wrote a
    // destination and wrote nothing in it, so the empty branch is a state of
    // its own and not an absence.
    "destination.url.empty": (tree) => / dest=url\(""\) /.test(tree),
    "destination.url.value": (tree) => / dest=url\("(?:\\.|[^"\\])+"\) /.test(tree),
    // A `Citation` is a node line under its `Cite`
    // with a tagged referent, its affixes are groups printed even when
    // empty, and a `Footnote` is a node line under `Document` after the
    // content, or absent.
    "citation.referent.footnote": (tree) =>
        /^.*Citation scope=\S+ anchor=null attributes=\{\} referent=footnote\(id="[^"]*"\) children=0$/m.test(tree),
    "citation.affix.empty": (tree) => /CitationPrefix children=0\n.*CitationSuffix children=0(?:\n|$)/.test(tree),
    "document.specimens.empty": (tree) =>
        tree.startsWith("Document scope=") && !/^(?:├──|└──) Specimen scope=/m.test(tree),
    "footnote.content.inline": (tree) =>
        parentEdges(tree).some((edge) => edge.parent === "Footnote" && edge.kind === "Text"),
    "footnote.content.block": (tree) =>
        parentEdges(tree).some((edge) => edge.parent === "Footnote" && edge.kind === "Paragraph"),
    "document.footnotes.empty": (tree) =>
        tree.startsWith("Document scope=") && !/^(?:├──|└──) Footnote scope=/m.test(tree),
    "document.footnotes.populated": (tree) =>
        /^(?:├──|└──) Footnote scope=\S+ anchor=null attributes=\{\} id="[^"]*" children=\d+$/m.test(tree),
    "list.variant.alpha.lower": (tree) => /^.*List scope=.* variant=alpha\(lowercased=true\) /m.test(tree),
    "list.variant.alpha.upper": (tree) => /^.*List scope=.* variant=alpha\(lowercased=false\) /m.test(tree),
    "list.variant.roman.lower": (tree) => /^.*List scope=.* variant=roman\(lowercased=true\) /m.test(tree),
    "list.variant.roman.upper": (tree) => /^.*List scope=.* variant=roman\(lowercased=false\) /m.test(tree),
    "list.variant.default": (tree) => /^.*List scope=.* variant=default /m.test(tree),
    "list.delimiter.default": (tree) => /^.*List scope=.* delimiter=default /m.test(tree),
    "list.delimiter.parenthesis.closed": (tree) => /^.*List scope=.* delimiter=parenthesis\(closed=true\) /m.test(tree),
    "citation.bib.mode.normal": (tree) =>
        /Citation scope=\S+ anchor=null attributes=\{\} referent=bib\(key="(?:\\.|[^"\\])*",mode=normal\)/.test(tree),
    "citation.bib.mode.authorInText": (tree) =>
        /Citation scope=\S+ anchor=null attributes=\{\} referent=bib\(key="(?:\\.|[^"\\])*",mode=authorInText\)/.test(
            tree
        ),
    "citation.bib.mode.suppressAuthor": (tree) =>
        /Citation scope=\S+ anchor=null attributes=\{\} referent=bib\(key="(?:\\.|[^"\\])*",mode=suppressAuthor\)/.test(
            tree
        ),
    "citation.referent.specimen": (tree) =>
        /Citation scope=\S+ anchor=null attributes=\{\} referent=specimen\(id="[^"\n]+"\)/.test(tree),
    "citation.prefix.populated": (tree) => /CitationPrefix children=[1-9]\d*/.test(tree),
    "citation.suffix.populated": (tree) => /CitationSuffix children=[1-9]\d*/.test(tree),
    "document.specimens.populated": (tree) => /^(?:├──|└──) Specimen scope=/m.test(tree),
    "specimen.id.null": (tree) => /Specimen scope=\S+ anchor=null attributes=\{\} id=null /.test(tree),
    "specimen.id.value": (tree) => /Specimen scope=\S+ anchor=null attributes=\{\} id="[^"\n]+" /.test(tree),
    "specimen.start.null": (tree) => /Specimen scope=\S+ anchor=null attributes=\{\} id=\S+ start=null /.test(tree),
    "specimen.start.value": (tree) =>
        /Specimen scope=\S+ anchor=null attributes=\{\} id=\S+ start=[1-9]\d* /.test(tree),
    "span.content.empty": (tree) => /Span scope=.* children=0(?:\n|$)/.test(tree),
    "span.content.populated": (tree) => /Span scope=.* children=[1-9]\d*(?:\n|$)/.test(tree)
};
export { parentEdges, stateValidators };
