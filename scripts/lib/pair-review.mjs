/** Closed adjudication of the original 30 pairs; no global impossibility claim.
 * A boundary is a declared corpus intervention, NOT an additive parser stage.
 * Its delta includes changes to recognition, construction and their interaction.
 */
const entries = [
    [
        "runs",
        ["run-insertion", "run-mark", "run-strike", "run-super", "run-sub"],
        "Split the many-to-one substitution into five marker languages; never pool their inverses."
    ],
    [
        "comment",
        ["opaque-comment"],
        "Reconstruct a nonempty, single-line opaque body without padding or delimiter runs."
    ],
    [
        "formula",
        ["opaque-formula", "opaque-display"],
        "Separate embedded and standalone modes; each has an explicit constructor mapping."
    ],
    [
        "anchor",
        [],
        "A block-owned identity is not a free name-to-URL definition; cut the explicit declaration, retaining its host."
    ],
    [
        "specimen",
        ["specimen-graph"],
        "Reconstruct unique labelled definitions/calls; verify full XML structure plus HTML IDs, forward/back edges and first-reference numbering."
    ],
    ["task", ["task-value"], "Map the singleton marker constructors ~ and x and preserve the ordered item body."],
    [
        "span",
        ["record-span", "class-span"],
        "Use separate one-record and one-class domains; preserve key/value and class fields rather than replacing their keys by c."
    ],
    [
        "xlink",
        ["cross-link", "cross-link-absent", "cross-link-empty", "cross-anchor", "cross-local"],
        "Split null, empty, populated and anchor forms; preserve target and label through explicit field mappings."
    ],
    [
        "citegroup",
        ["cite-normal"],
        "The former merged link label loses the prefix/suffix boundary; cut affixes and prove the singleton empty-affix citation."
    ],
    [
        "cite",
        ["cite-author", "cite-suppress"],
        "Separate citation modes and reconstruct explicit URI autolinks with a reversible key encoding."
    ],
    [
        "formulablock",
        ["leaf-formula"],
        "Make the mandatory code-body terminal newline an explicit reversible representation map."
    ],
    [
        "embed",
        ["cross-embed", "cross-embed-absent", "cross-embed-empty"],
        "Preserve label states; image has no numeric dimension field, so cut dimensions at their suffix boundary."
    ],
    [
        "simpletable",
        ["simple-matrix"],
        "Repair none versus left alignment with explicit :---; preserve ordered header/body cells."
    ],
    [
        "blockcomment",
        ["leaf-comment"],
        "A fenced literal leaf with its final newline maps to the same literal payload in a code block."
    ],
    [
        "formulafence",
        ["leaf-fence"],
        "Use an empty code info field and map the fixed formula fence tag to the leaf constructor."
    ],
    [
        "metadataempty",
        [],
        "Discarded unknown members cannot be recovered from an empty field map; remove the envelope and retain the following document."
    ],
    [
        "metadata",
        [],
        "Typed scalars, lists and duplicate-key overwrite are absent from a code literal; cut the envelope from the body."
    ],
    [
        "tcaption",
        ["simple-matrix"],
        "A loose continuation has no table/caption ownership; retain the table and cut only its trailing caption."
    ],
    [
        "idirective",
        ["inline-directive", "empty-directive"],
        "Repair the spurious destination index: the fixed directive name maps to /note and label inlines remain children."
    ],
    [
        "fancylist",
        [
            "alpha-list",
            "upper-list",
            "roman-list",
            "upper-roman-list",
            "default-list",
            "enclosed-default-list",
            "decimal-list"
        ],
        "Separate marker alphabets/delimiters and preserve start values (including 3 and 4)."
    ],
    [
        "formulapromo",
        ["leaf-promotion"],
        "Replace the annihilated reference definition with a literal code block; both sides now construct the corresponding leaf."
    ],
    [
        "specimenstart",
        ["specimen-graph"],
        "The explicit sequence reset has no footnote counterpart; remove only the reset digit, retaining the proved definition/call graph."
    ],
    [
        "ldirective",
        ["leaf-directive"],
        "Replace headings and their derived anchors with a one-image paragraph; the block and label wrappers have explicit roles."
    ],
    [
        "cdirective",
        ["anonymous-container", "named-container"],
        "Split fixed named and anonymous container languages; preserve the owned block sequence."
    ],
    [
        "callout",
        [],
        "Three collapsed states and title ownership collapse to one completed task value; remove the callout header metadata within the quote."
    ],
    [
        "headless",
        ["simple-matrix"],
        "The former first-row text and header role both differ; compare a reconstructed headed matrix and isolate headless syntax by removing table borders."
    ],
    [
        "sparsegrid",
        ["grid-cell"],
        "The old list reordered h/a/b/c and lost spans, empty rows, caption and foot; isolate geometry with border/cell-marker ablation and prove the one-cell restriction."
    ],
    [
        "gridcell",
        ["grid-cell"],
        "Reconstruct complete fixed-width cells, preserve the two ordered paragraphs, and delimit independent units."
    ],
    [
        "caption",
        ["simple-matrix"],
        "A caption role is not a header cell role; cut the leading caption while retaining the table's h/v matrix."
    ],
    [
        "deflist",
        ["loose-definition"],
        "The old list loses per-definition compactness and empty-body identity; prove one loose term/body domain and cut definition ownership for the full mixed workload."
    ]
];

const cuts = {
    anchor: (s) => s.replace(/^#anchor-\d+#\n/gmu, ""),
    specimenstart: (s) => s.replace(/\(5(@spec-\d+\))/gu, "($1"),
    citegroup: (s) => s.replace(/\[see (@key-\d+), p\. 3\]/gu, "[$1]"),
    embed: (s) => s.replace(/\|100(?:x200)?\]\]/gu, "|]]"),
    metadataempty: (s) => s.replace(/^---\n[\s\S]*?\n---\n\n/u, ""),
    metadata: (s) => s.replace(/^---\n[\s\S]*?\n---\n\n/u, ""),
    tcaption: (s) => s.replace(/^: Caption \d+\n\n/gmu, ""),
    callout: (s) => s.replace(/\[!note\][-+]? /gu, ""),
    headless: (s) => s.replace(/^-{10} {2}-{10}\n/gmu, ""),
    sparsegrid: (s) =>
        s
            .replace(/^\+[^\n]*\n/gmu, "")
            .replace(/\|/gu, "")
            .replace(/^Table:\n\n/gmu, ""),
    caption: (s) => s.replace(/^Table: Caption \d+\n/gmu, ""),
    deflist: (s) => s.replace(/^: ?/gmu, "")
};

export const pairReviews = new Map(
    entries.map(([id, proofs, reason]) => [
        id,
        {
            id,
            proofs: proofs.map((name) => `${name}-v1`),
            reason,
            outcome: cuts[id] ? "boundary" : "reconstructed",
            ...(cuts[id] ? { baseline: `boundary-${id}-without` } : {})
        }
    ])
);

export function pairReview(pair) {
    const id = pair.contract?.review;
    const review = pairReviews.get(id);
    if (!review || pair.case !== `pair-${id}-dialect` || pair.isomorph !== `pair-${id}-common`)
        throw new Error(`${pair.case}: unknown pairing review`);
    return review;
}
export function boundarySource(id, source) {
    const cut = cuts[id];
    if (!cut) throw new Error(`unknown boundary ${id}`);
    const result = cut(source);
    if (result === source || !result.trim()) throw new Error(`${id}: boundary removes no work or the entire host`);
    return result;
}
