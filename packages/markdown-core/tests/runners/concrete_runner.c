/* Ownership-region and marker-capture gates (M2.5,
 * incremental-canonical-ast.md 0, 11.1, 14.1.9).
 *
 * The delivery plan's standing failure mode is a classification whose
 * completeness was checked while its content was not: "the check confirmed
 * the partition was complete but never that a class matched the inventory's
 * own content category". These gates close both sides over a real parse of
 * every node kind:
 *
 *   region_partition   every node of a 34-kind document classifies, the
 *                      class agrees with the node's observed content
 *                      category (who actually holds inline children, who
 *                      holds only blocks), and every kind was seen — a
 *                      kind missing from the fixture cannot hide.
 *   region_of_walk     concrete ownership resolves for every node: a
 *                      region resolves to itself, an inline resolves to
 *                      the nearest LEAF or INLINE_SEQUENCE ancestor and
 *                      never to a CONTAINER (marker material owns no
 *                      inline content), at any nesting depth.
 *
 * The capture gates hold the block phase's concrete marker records
 * (concrete_records.h) to the source, not to the capture code's own idea
 * of itself: every record must dereference into the stored line bytes and
 * spell the marker its kind claims, agree with the canonical AST fields
 * derived from the same marker, and appear on exactly the nodes 11.1 makes
 * owners — no record on a kind that owns no marker bytes.
 *
 *   capture_shape        one-shot parses over fixtures that reach every
 *                        record kind and every consumption edge (lazy
 *                        continuation, clamped fences, BOM, NUL
 *                        replacement, CRLF, tabs): records dereferenced
 *                        against the line bytes, cross-checked against
 *                        AST fields, counted per owner.
 *   capture_document     Document.concrete returns the one physical tree
 *                        from a one-shot document and from a session's
 *                        committed view alike (14.1.9), records reachable
 *                        through both.
 *   capture_equivalence  after every commit of an edit script that forces
 *                        suffix reflow, nested reparses, marker edits,
 *                        fence reflow, and definition flips, the session
 *                        tree's records equal a fresh parse's, node for
 *                        node — which is exactly the region-relative
 *                        encoding claim: a record an edit did not own
 *                        moved with its region, untouched.
 *   capture_oom_sweep    a lost record is a failed parse, never a
 *                        silently thinner tree: under single-shot
 *                        allocation failure at every ordinal, the parse
 *                        either fails cleanly or captures everything.
 *
 * The recovery gates hold 14.1.10's boundary: unmatched core Markdown and
 * unmatched island openers stay literal (recovery_literal_fallback — no
 * guessed structure, byte-exact Text reconstruction, every surviving
 * inline record an unconsumed candidate), while a committed bounded
 * island recovers only inside its own termination rule or closing parent,
 * preserves its authored source, and never consumes the unrelated region
 * that follows (recovery_island_boundary — eligibility bound to each
 * form's commit point, never its node kind).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "ast_internal.h"
#include "buffer.h"
#include "concrete.h"
#include "concrete_records.h"
#include "cross_reference.h"
#include "directive.h"
#include "formula.h"
#include "houdini.h"
#include "inlines.h"
#include "markdown-core-extensions.h"
#include "markdown_core_ctype.h"
#include "parser.h"
#include "strikethrough.h"
#include "table.h"

/* The complete engine node inventory with each kind's expected region
 * class, verbatim from 11.1 — the gate asserts the exact class, not a
 * class family, so LEAF and INLINE_SEQUENCE cannot drift into each other
 * unnoticed. DirectiveLabel's entry holds its detached/inline-parent class;
 * the block-parent side of the contextual pair is asserted explicitly in
 * the walk. Completeness is anchored to the public kind enumeration, not
 * to this table: the runtime check below fails the moment a kind is added
 * to the facade without a row (and a fixture production) here. */
typedef struct expected_type {
    markdown_core_node_type type;
    const char *name;
    markdown_core_region_class expected_class;
} expected_type;

/* One past the last public node kind; grows with the facade enum, which is
 * what lets the completeness check notice a kind this file has never heard
 * of. Mirrors test_support's TS_KIND_COUNT and shares its caveat: the end
 * marker must track the enum's real end. */
#define CONCRETE_KIND_COUNT (MARKDOWN_CORE_KIND_IMAGE_REFERENCE + 1)

static const expected_type EXPECTED_TYPES[] = {
    {MARKDOWN_CORE_NODE_DOCUMENT, "Document", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_BLOCK_QUOTE, "BlockQuote", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_LIST, "List", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_LIST_ITEM, "ListItem", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_CODE_BLOCK, "CodeBlock", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_HTML_BLOCK, "HTMLBlock", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_PARAGRAPH, "Paragraph", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_HEADING, "Heading", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_THEMATIC_BREAK, "ThematicBreak", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION, "FootnoteDefinition", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, "ReferenceDefinition", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_TABLE, "Table", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_TABLE_ROW, "TableRow", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_TABLE_CELL, "TableCell", MARKDOWN_CORE_REGION_INLINE_SEQUENCE},
    {MARKDOWN_CORE_NODE_FORMULA_BLOCK, "FormulaBlock", MARKDOWN_CORE_REGION_LEAF},
    {MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, "DirectiveBlock", MARKDOWN_CORE_REGION_CONTAINER},
    {MARKDOWN_CORE_NODE_TEXT, "Text", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_SOFT_BREAK, "SoftBreak", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_LINE_BREAK, "LineBreak", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_CODE, "Code", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_HTML, "HTML", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_EMPHASIS, "Emphasis", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_STRONG, "Strong", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_LINK, "Link", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_IMAGE, "Image", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE, "FootnoteReference", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_LINK_REFERENCE, "LinkReference", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_IMAGE_REFERENCE, "ImageReference", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_STRIKETHROUGH, "Strikethrough", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_FORMULA, "Formula", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_DIRECTIVE, "Directive", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, "DirectiveLabel", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_CROSS_LINK, "CrossLink", MARKDOWN_CORE_REGION_NONE},
    {MARKDOWN_CORE_NODE_EMBED, "Embed", MARKDOWN_CORE_REGION_NONE},
};

#define EXPECTED_TYPE_COUNT (sizeof(EXPECTED_TYPES) / sizeof(EXPECTED_TYPES[0]))

/* One document reaching all 34 kinds. Block directives carry a label (the
 * contextual INLINE_SEQUENCE case) and an inline directive carries the
 * NONE-side label of the same kind. */
static const char FIXTURE[] = "# Heading *emphasis* **strong** `code` <b>html</b>\n"
                              "\n"
                              "Setext with ~~strike~~ and $x+y$ and :badge[inline label]\n"
                              "======\n"
                              "\n"
                              "Paragraph with [link](/url), ![image](/img), [ref][x], ![imgref][x],\n"
                              "soft\n"
                              "break and hard  \n"
                              "break, autolink <https://example.com>, [[cross-link]], ![[embed]],\n"
                              "and a footnote [^note].\n"
                              "\n"
                              "> quoted paragraph\n"
                              "\n"
                              "- bullet item\n"
                              "- [x] task item\n"
                              "\n"
                              "1. ordered item\n"
                              "\n"
                              "    indented code\n"
                              "\n"
                              "```info\n"
                              "fenced code\n"
                              "```\n"
                              "\n"
                              "$$\n"
                              "formula block\n"
                              "$$\n"
                              "\n"
                              "---\n"
                              "\n"
                              "<div>\n"
                              "html block\n"
                              "</div>\n"
                              "\n"
                              "| a | b |\n"
                              "| - | - |\n"
                              "| *c* | d |\n"
                              "\n"
                              ":::note[block label]\n"
                              "directive body\n"
                              ":::\n"
                              "\n"
                              "[^note]: footnote body\n"
                              "\n"
                              "[x]: /defined\n";

static markdown_core_parse_options capture_options(void) {
    markdown_core_parse_options options;
    markdown_core_parse_options_init(&options);
    options.footnotes = true;
    options.tables = true;
    options.strikethrough = true;
    options.autolinks = true;
    options.task_lists = true;
    options.formulas = true;
    options.directives = true;
    options.cross_links = true;
    options.embeds = true;
    return options;
}

static markdown_core_document *parse_fixture(void) {
    markdown_core_parse_options options = capture_options();
    return markdown_core_document_parse((const uint8_t *)FIXTURE, sizeof(FIXTURE) - 1, &options, NULL);
}

static bool is_block(const markdown_core_node *node) {
    return (node->type & MARKDOWN_CORE_NODE_TYPE_MASK) == MARKDOWN_CORE_NODE_TYPE_BLOCK;
}

/* Iterative preorder over the engine tree; no recursion, so pathological
 * depth cannot overflow the gate itself. */
typedef struct walk_state {
    const markdown_core_node *next;
    const markdown_core_node *root;
} walk_state;

static const markdown_core_node *walk_next(walk_state *walk) {
    const markdown_core_node *node = walk->next;
    if (!node) {
        return NULL;
    }
    if (node->first_child) {
        walk->next = node->first_child;
    } else {
        const markdown_core_node *up = node;
        while (up && up != walk->root && !up->next) {
            up = up->parent;
        }
        walk->next = (up && up != walk->root) ? up->next : NULL;
    }
    return node;
}

static void walk_init(walk_state *walk, const markdown_core_node *root) {
    walk->next = root;
    walk->root = root;
}

static const char *type_name(markdown_core_node_type type) {
    size_t i;
    for (i = 0; i < EXPECTED_TYPE_COUNT; i++) {
        if (EXPECTED_TYPES[i].type == type) {
            return EXPECTED_TYPES[i].name;
        }
    }
    return "(unknown)";
}

static size_t tree_inline_record_total(const markdown_core_node *root);

/* --- region_partition --------------------------------------------------- */

static int case_region_partition(void) {
    markdown_core_document *document = parse_fixture();
    const markdown_core_node *node;
    walk_state walk;
    bool seen[EXPECTED_TYPE_COUNT] = {false};
    bool seen_kinds[CONCRETE_KIND_COUNT] = {false};
    size_t i;
    int failed = 0;
    if (!document) {
        fprintf(stderr, "region_partition: fixture failed to parse\n");
        return -1;
    }
    walk_init(&walk, document->root);
    while ((node = walk_next(&walk)) != NULL) {
        markdown_core_region_class region_class = markdown_core_region_classify(node);
        bool has_owned_inline_child = false; /* an inline child that is not itself a region */
        bool has_block_child = false;
        bool has_non_region_child = false;
        const markdown_core_node *child;
        for (child = node->first_child; child; child = child->next) {
            bool child_is_region = markdown_core_region_classify(child) != MARKDOWN_CORE_REGION_NONE;
            if (is_block(child)) {
                has_block_child = true;
            } else if (!child_is_region) {
                has_owned_inline_child = true;
            }
            if (!child_is_region) {
                has_non_region_child = true;
            }
        }
        for (i = 0; i < EXPECTED_TYPE_COUNT; i++) {
            if (EXPECTED_TYPES[i].type == node->type) {
                seen[i] = true;
                break;
            }
        }
        if (i == EXPECTED_TYPE_COUNT) {
            fprintf(stderr, "region_partition: unexpected node type 0x%04x\n", node->type);
            failed = 1;
        } else {
            /* The exact class of 11.1's own table, per kind — LEAF and
             * INLINE_SEQUENCE are different capture semantics and must not
             * drift into each other behind the family invariants below.
             * The block directive's label is the one contextual override. */
            markdown_core_region_class expected = EXPECTED_TYPES[i].expected_class;
            if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL && node->parent &&
                node->parent->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
                expected = MARKDOWN_CORE_REGION_INLINE_SEQUENCE;
            }
            if (region_class != expected) {
                fprintf(
                    stderr,
                    "region_partition: %s classifies as %d, 11.1 says %d\n",
                    type_name(node->type),
                    (int)region_class,
                    (int)expected
                );
                failed = 1;
            }
        }
        seen_kinds[markdown_core_node_get_kind(node)] = true;

        /* Partition totality over blocks: every block-typed node is a
         * region — a block the classification does not place has nowhere
         * to write its concrete records (11.1). */
        if (is_block(node) && region_class == MARKDOWN_CORE_REGION_NONE) {
            fprintf(stderr, "region_partition: block %s classifies as NONE (11.1)\n", type_name(node->type));
            failed = 1;
        }
        /* Inline nodes are never regions, except the block directive's
         * label — the one contextual kind. */
        if (!is_block(node) && region_class != MARKDOWN_CORE_REGION_NONE &&
            !(node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL && node->parent &&
              node->parent->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)) {
            fprintf(stderr, "region_partition: inline %s classifies as a region (11.1)\n", type_name(node->type));
            failed = 1;
        }

        /* Content-category agreement, the check the plan's failure mode
         * says must exist: owned (non-region) inline children are held
         * exactly by LEAF and INLINE_SEQUENCE regions, and a container's
         * children are all regions themselves — the block directive's
         * label is inline-typed yet a region of its own, which is why the
         * invariant is region-hood, not block-typing. */
        if (has_owned_inline_child && region_class != MARKDOWN_CORE_REGION_LEAF &&
            region_class != MARKDOWN_CORE_REGION_INLINE_SEQUENCE && is_block(node)) {
            fprintf(
                stderr,
                "region_partition: %s owns inline children but classifies as %d (11.1)\n",
                type_name(node->type),
                (int)region_class
            );
            failed = 1;
        }
        if (region_class == MARKDOWN_CORE_REGION_CONTAINER && has_non_region_child) {
            fprintf(
                stderr,
                "region_partition: container %s holds a non-region child (marker material owns no inline content)\n",
                type_name(node->type)
            );
            failed = 1;
        }
        if ((region_class == MARKDOWN_CORE_REGION_LEAF || region_class == MARKDOWN_CORE_REGION_INLINE_SEQUENCE) &&
            has_block_child) {
            fprintf(
                stderr,
                "region_partition: inline-owning region %s holds block children (11.1)\n",
                type_name(node->type)
            );
            failed = 1;
        }
    }
    /* The fixture must reach every kind, or a misclassification hides in
     * the kinds it skipped. */
    for (i = 0; i < EXPECTED_TYPE_COUNT; i++) {
        if (!seen[i]) {
            fprintf(stderr, "region_partition: fixture never produced %s\n", EXPECTED_TYPES[i].name);
            failed = 1;
        }
    }
    /* Completeness against the authority, not this file: every public node
     * kind must appear, and the expectation table must be exactly as long
     * as the kind enumeration. A kind added to the facade fails here until
     * this table, the fixture, and the classification all learn it. */
    for (i = MARKDOWN_CORE_KIND_NONE + 1; i < CONCRETE_KIND_COUNT; i++) {
        if (!seen_kinds[i]) {
            fprintf(stderr, "region_partition: fixture never produced public kind %zu\n", i);
            failed = 1;
        }
    }
    if (EXPECTED_TYPE_COUNT != CONCRETE_KIND_COUNT - 1) { /* minus the KIND_NONE sentinel */
        fprintf(
            stderr,
            "region_partition: %zu expected types for %d public kinds — the inventory moved\n",
            EXPECTED_TYPE_COUNT,
            (int)(CONCRETE_KIND_COUNT - 1)
        );
        failed = 1;
    }
    markdown_core_document_free(document);
    return failed ? -1 : 0;
}

/* --- region_of_walk ----------------------------------------------------- */

static int case_region_of_walk(void) {
    markdown_core_document *document = parse_fixture();
    const markdown_core_node *node;
    walk_state walk;
    int failed = 0;
    if (!document) {
        return -1;
    }
    walk_init(&walk, document->root);
    while ((node = walk_next(&walk)) != NULL) {
        markdown_core_region_class region_class = markdown_core_region_classify(node);
        const markdown_core_node *region = markdown_core_region_of(node);
        if (region_class != MARKDOWN_CORE_REGION_NONE) {
            if (region != node) {
                fprintf(stderr, "region_of_walk: region %s does not resolve to itself\n", type_name(node->type));
                failed = 1;
            }
            continue;
        }
        /* A non-region resolves to its nearest region ancestor, which owns
         * an inline sequence — never a container (11.1). */
        if (!region) {
            fprintf(stderr, "region_of_walk: %s resolves to no region\n", type_name(node->type));
            failed = 1;
            continue;
        }
        {
            markdown_core_region_class owner_class = markdown_core_region_classify(region);
            const markdown_core_node *ancestor = node->parent;
            bool is_ancestor = false;
            while (ancestor) {
                if (ancestor == region) {
                    is_ancestor = true;
                    break;
                }
                ancestor = ancestor->parent;
            }
            if (!is_ancestor) {
                fprintf(stderr, "region_of_walk: %s's region is not an ancestor\n", type_name(node->type));
                failed = 1;
            }
            if (owner_class != MARKDOWN_CORE_REGION_LEAF && owner_class != MARKDOWN_CORE_REGION_INLINE_SEQUENCE) {
                fprintf(
                    stderr,
                    "region_of_walk: inline %s owned by class %d, not an inline-owning region (11.1)\n",
                    type_name(node->type),
                    (int)owner_class
                );
                failed = 1;
            }
        }
    }
    markdown_core_document_free(document);

    /* The one NULL result the header documents: a detached non-region node
     * has no region ancestor to resolve to. No committed tree contains one;
     * the contract edge is pinned directly. */
    if (!failed) {
        markdown_core_node *detached = markdown_core_node_new(MARKDOWN_CORE_NODE_TEXT);
        if (!detached) {
            return -1;
        }
        if (markdown_core_region_of(detached) != NULL) {
            fprintf(stderr, "region_of_walk: detached inline resolved to a region\n");
            failed = 1;
        }
        markdown_core_node_free(detached);
    }
    /* Same edge for the contextual kind: a directive label with no parent
     * has no block-directive context, so it classifies as NONE. */
    if (!failed) {
        markdown_core_node *label = markdown_core_node_new(MARKDOWN_CORE_NODE_DIRECTIVE_LABEL);
        if (!label) {
            return -1;
        }
        if (markdown_core_region_classify(label) != MARKDOWN_CORE_REGION_NONE) {
            fprintf(stderr, "region_of_walk: detached directive label classified as a region\n");
            failed = 1;
        }
        markdown_core_node_free(label);
    }

    /* Depth safety: region resolution inside a deep container spine is a
     * parent-chain walk with no recursion; a thousand-deep quote chain must
     * resolve its innermost text to the innermost paragraph. */
    if (!failed) {
        size_t depth = 1000;
        size_t text_length = depth * 2 + 2;
        char *text = (char *)malloc(text_length + 1);
        markdown_core_document *deep;
        size_t d;
        if (!text) {
            return -1;
        }
        for (d = 0; d < depth; d++) {
            text[d * 2] = '>';
            text[d * 2 + 1] = ' ';
        }
        text[depth * 2] = 'a';
        text[depth * 2 + 1] = '\n';
        text[text_length] = 0;
        deep = markdown_core_document_parse((const uint8_t *)text, text_length, NULL, NULL);
        free(text);
        if (!deep) {
            return -1;
        }
        {
            const markdown_core_node *cursor = deep->root;
            const markdown_core_node *paragraph = NULL;
            while (cursor->first_child) {
                cursor = cursor->first_child;
                if (cursor->type == MARKDOWN_CORE_NODE_PARAGRAPH) {
                    paragraph = cursor;
                }
            }
            if (!paragraph || cursor->type != MARKDOWN_CORE_NODE_TEXT || markdown_core_region_of(cursor) != paragraph) {
                fprintf(stderr, "region_of_walk: deep spine did not resolve to the innermost paragraph\n");
                failed = 1;
            }
        }
        markdown_core_document_free(deep);
    }
    return failed ? -1 : 0;
}

/* --- capture gates ------------------------------------------------------ */

/* The normalized line the parser actually scanned: the 1-based line_number'th
 * line of `text`, EOL excluded, NUL bytes replaced by U+FFFD — the same two
 * normalizations S_parser_feed applies, restated here independently so the
 * gate checks the capture against the source, not against the parser's own
 * buffer. Returns false when the document has no such line. */
static bool normalized_line(
    const char *text,
    size_t length,
    uint32_t line_number,
    char *out,
    size_t out_capacity,
    size_t *out_length
) {
    size_t pos = 0;
    uint32_t line = 1;
    size_t filled = 0;
    while (line < line_number) {
        if (pos >= length) {
            return false;
        }
        if (text[pos] == '\n') {
            line++;
        } else if (text[pos] == '\r') {
            line++;
            if (pos + 1 < length && text[pos + 1] == '\n') {
                pos++;
            }
        }
        pos++;
    }
    while (pos < length && text[pos] != '\n' && text[pos] != '\r') {
        if (text[pos] == '\0') {
            if (filled + 3 > out_capacity) {
                return false;
            }
            memcpy(out + filled, "\xef\xbf\xbd", 3);
            filled += 3;
        } else {
            if (filled + 1 > out_capacity) {
                return false;
            }
            out[filled++] = text[pos];
        }
        pos++;
    }
    *out_length = filled;
    return true;
}

/* Absolute start line under the sealed parent-relative encoding — the same
 * resolution scope_with_parent_start applies for the dump. */
static int resolved_start_line(const markdown_core_node *node, int parent_resolved) {
    if (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
        return parent_resolved + node->start_line;
    }
    return node->start_line;
}

static bool run_all(const char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != bytes[0]) {
            return false;
        }
    }
    return length > 0;
}

/* True when the extent is the maximal run of its own first byte: nothing of
 * the same byte follows it on the line. What pins a run record to the whole
 * marker rather than a prefix of it. */
static bool run_maximal(const char *line, size_t line_length, const markdown_core_concrete_record *record) {
    return (size_t)record->column + record->length == line_length ||
           line[record->column + record->length] != line[record->column];
}

typedef struct capture_source {
    const char *name;
    const char *text;
    size_t length;
    /* True for the fixture whose point is that nothing may be captured. */
    bool recordless;
} capture_source;

/* Validates one node's record vector against the source and the node's own
 * canonical fields; recurses over children carrying the resolved line.
 * Returns the number of failures. */
static int check_node_records(const capture_source *source, const markdown_core_node *node, int parent_resolved) {
    int failed = 0;
    int resolved = resolved_start_line(node, parent_resolved);
    size_t count = 0;
    const markdown_core_concrete_record *records = markdown_core_node_concrete_records(node, &count);
    const markdown_core_node *child;
    size_t i;
    char line[4096];
    size_t line_length = 0;

    /* Capture order ascends by (line, column) within one owner, flags are
     * reserved-zero, and every extent dereferences into its line. */
    for (i = 0; i < count; i++) {
        const markdown_core_concrete_record *record = &records[i];
        if (record->flags != 0 || record->length == 0) {
            fprintf(
                stderr,
                "%s: %s record %zu carries flags %u length %u\n",
                source->name,
                type_name(node->type),
                i,
                record->flags,
                record->length
            );
            failed = 1;
        }
        if (i > 0 && (records[i - 1].line > record->line ||
                      (records[i - 1].line == record->line && records[i - 1].column >= record->column))) {
            fprintf(
                stderr,
                "%s: %s records %zu,%zu out of source order\n",
                source->name,
                type_name(node->type),
                i - 1,
                i
            );
            failed = 1;
        }
        if (!normalized_line(
                source->text,
                source->length,
                (uint32_t)resolved + record->line,
                line,
                sizeof(line),
                &line_length
            ) ||
            (size_t)record->column + record->length > line_length) {
            fprintf(
                stderr,
                "%s: %s record %zu extent [%u,%u+%u) escapes line %u\n",
                source->name,
                type_name(node->type),
                i,
                record->column,
                record->column,
                record->length,
                (uint32_t)resolved + record->line
            );
            failed = 1;
            continue;
        }
        switch (record->kind) {
        case MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER:
            /* The opening record is additionally pinned to the quote's own
             * start position; continuation columns vary per line and are
             * pinned by the per-fixture expectation tables. */
            if (node->type != MARKDOWN_CORE_NODE_BLOCK_QUOTE || record->length != 1 || line[record->column] != '>' ||
                (i == 0 && (record->line != 0 || (int)record->column != node->start_column - 1))) {
                fprintf(stderr, "%s: block-quote marker record does not spell the quote's '>'\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_LIST_MARKER: {
            const char *marker = line + record->column;
            if (node->type != MARKDOWN_CORE_NODE_LIST_ITEM || record->line != 0) {
                fprintf(stderr, "%s: list marker record on %s\n", source->name, type_name(node->type));
                failed = 1;
            } else if (node->as.list.list_type == MARKDOWN_CORE_BULLET_LIST) {
                if (record->length != 1 || (unsigned char)marker[0] != node->as.list.bullet_char) {
                    fprintf(stderr, "%s: bullet record does not spell bullet_char\n", source->name);
                    failed = 1;
                }
            } else {
                int value = 0;
                size_t d;
                char delimiter = marker[record->length - 1];
                for (d = 0; d + 1 < record->length; d++) {
                    if (marker[d] < '0' || marker[d] > '9') {
                        break;
                    }
                    value = value * 10 + (marker[d] - '0');
                }
                if (d + 1 != record->length || value != node->as.list.start ||
                    delimiter != (node->as.list.delimiter == MARKDOWN_CORE_PERIOD_DELIM ? '.' : ')')) {
                    fprintf(stderr, "%s: ordinal record does not spell start/delimiter\n", source->name);
                    failed = 1;
                }
            }
            break;
        }
        case MARKDOWN_CORE_CONCRETE_ATX_OPENER:
            if (node->type != MARKDOWN_CORE_NODE_HEADING || node->as.heading.setext || record->line != 0 ||
                !run_all(line + record->column, record->length) || line[record->column] != '#' ||
                !run_maximal(line, line_length, record) || (int)record->length != node->as.heading.level ||
                (int)record->column != node->start_column - 1) {
                fprintf(stderr, "%s: ATX opener record disagrees with heading\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_ATX_CLOSER:
            if (node->type != MARKDOWN_CORE_NODE_HEADING || node->as.heading.setext || record->line != 0 ||
                !run_all(line + record->column, record->length) || line[record->column] != '#' ||
                !run_maximal(line, line_length, record)) {
                fprintf(stderr, "%s: ATX closer record does not spell a '#' run\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE:
            if (node->type != MARKDOWN_CORE_NODE_HEADING || !node->as.heading.setext || record->line == 0 ||
                !run_all(line + record->column, record->length) || !run_maximal(line, line_length, record) ||
                line[record->column] != (node->as.heading.level == 1 ? '=' : '-')) {
                fprintf(stderr, "%s: setext underline record disagrees with heading level\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_FENCE_OPEN:
            if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
                if (!node->as.code.fenced || record->line != 0 || record->length < 3 ||
                    !run_all(line + record->column, record->length) || !run_maximal(line, line_length, record) ||
                    (unsigned char)line[record->column] != node->as.code.fence_char ||
                    node->as.code.fence_length != (record->length > 255 ? 255 : (uint8_t)record->length) ||
                    (int)record->column != node->start_column - 1) {
                    fprintf(stderr, "%s: fence-open record disagrees with code block\n", source->name);
                    failed = 1;
                }
            } else if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
                if (record->line != 0 || record->length < 2 || line[record->column] != ':' ||
                    !run_all(line + record->column, record->length) || !run_maximal(line, line_length, record) ||
                    (int)record->column != node->start_column - 1) {
                    fprintf(stderr, "%s: fence-open record disagrees with directive block\n", source->name);
                    failed = 1;
                }
            } else if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
                if (record->line != 0 ||
                    !((record->length == 2 && memcmp(line + record->column, "$$", 2) == 0) ||
                      (record->length == 3 && memcmp(line + record->column, "\\\\[", 3) == 0)) ||
                    (int)record->column != node->start_column - 1) {
                    fprintf(stderr, "%s: fence-open record disagrees with formula block\n", source->name);
                    failed = 1;
                }
            } else {
                fprintf(stderr, "%s: fence-open record on %s\n", source->name, type_name(node->type));
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_FENCE_INFO: {
            /* The record is the spelling; as.code.info is the decoded
             * scalar. Decoding the spelled bytes the way finalize does must
             * reproduce the scalar exactly. */
            markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
            markdown_core_houdini_unescape_html_f(
                &decoded,
                (const uint8_t *)line + record->column,
                (markdown_core_bufsize)record->length
            );
            markdown_core_strbuf_trim(&decoded);
            markdown_core_strbuf_unescape(&decoded);
            if (node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !node->as.code.fenced || record->line != 0 ||
                markdown_core_isspace(line[record->column]) ||
                markdown_core_isspace(line[record->column + record->length - 1]) ||
                decoded.size != (markdown_core_bufsize)node->as.code.info.len ||
                memcmp(decoded.ptr, node->as.code.info.data, decoded.size) != 0) {
                fprintf(stderr, "%s: fence-info record does not decode to as.code.info\n", source->name);
                failed = 1;
            }
            markdown_core_strbuf_free(&decoded);
            break;
        }
        case MARKDOWN_CORE_CONCRETE_FENCE_CLOSE:
            if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
                if (!node->as.code.fenced || !node->as.code.fence_closed ||
                    record->length < node->as.code.fence_length || !run_all(line + record->column, record->length) ||
                    !run_maximal(line, line_length, record) ||
                    (unsigned char)line[record->column] != node->as.code.fence_char ||
                    (int)(record->line) !=
                        node->end_line - (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE ? 0 : node->start_line)) {
                    fprintf(stderr, "%s: fence-close record disagrees with code block\n", source->name);
                    failed = 1;
                }
            } else if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
                if (record->line == 0 || record->length < 2 || line[record->column] != ':' ||
                    !run_all(line + record->column, record->length) || !run_maximal(line, line_length, record)) {
                    fprintf(stderr, "%s: fence-close record disagrees with directive block\n", source->name);
                    failed = 1;
                }
            } else if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
                if (record->line == 0 || !((record->length == 2 && memcmp(line + record->column, "$$", 2) == 0) ||
                                           (record->length == 3 && memcmp(line + record->column, "\\\\]", 3) == 0))) {
                    fprintf(stderr, "%s: fence-close record disagrees with formula block\n", source->name);
                    failed = 1;
                }
            } else {
                fprintf(stderr, "%s: fence-close record on %s\n", source->name, type_name(node->type));
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_THEMATIC_BREAK: {
            const char *bytes = line + record->column;
            char marker = bytes[0];
            size_t marker_count = 0;
            size_t b;
            bool composition = (marker == '*' || marker == '-' || marker == '_');
            for (b = 0; b < record->length && composition; b++) {
                if (bytes[b] == marker) {
                    marker_count++;
                } else if (bytes[b] != ' ' && bytes[b] != '\t') {
                    composition = false;
                }
            }
            if (node->type != MARKDOWN_CORE_NODE_THEMATIC_BREAK || record->line != 0 || !composition ||
                marker_count < 3 || bytes[record->length - 1] != marker ||
                (int)record->column != node->start_column - 1) {
                fprintf(stderr, "%s: thematic-break record is not the construct\n", source->name);
                failed = 1;
            }
            break;
        }
        case MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER:
            if (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION || record->line != 0 || record->length < 5 ||
                memcmp(line + record->column, "[^", 2) != 0 ||
                memcmp(line + record->column + 2, node->as.literal.data, node->as.literal.len) != 0 ||
                record->length != node->as.literal.len + 4 ||
                memcmp(line + record->column + 2 + node->as.literal.len, "]:", 2) != 0) {
                fprintf(stderr, "%s: footnote-opener record does not spell [^label]:\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW: {
            /* The whole trimmed row, spelled from delimiter-row bytes only,
             * and decoding to the Table's own alignments column for column:
             * a record that drifts onto a data row, or a captured row that
             * disagrees with the alignments the parse kept, fails here. */
            const uint8_t *alignments = markdown_core_extensions_get_table_alignments((markdown_core_node *)node);
            uint16_t columns = markdown_core_extensions_get_table_columns((markdown_core_node *)node);
            size_t b = 0;
            uint16_t column_index = 0;
            bool composition = true;
            const char *bytes = line + record->column;
            if (node->type != MARKDOWN_CORE_NODE_TABLE || record->line == 0 || record->length < 1 || bytes[0] == ' ' ||
                bytes[0] == '\t' || bytes[record->length - 1] == ' ' || bytes[record->length - 1] == '\t' ||
                !alignments) {
                fprintf(stderr, "%s: table delimiter-row record is not the trimmed row\n", source->name);
                failed = 1;
                break;
            }
            while (b < record->length && composition) {
                if (bytes[b] == '|') {
                    b++;
                    continue;
                }
                if (bytes[b] == ' ' || bytes[b] == '\t') {
                    b++;
                    continue;
                }
                if (bytes[b] == ':' || bytes[b] == '-') {
                    bool left = bytes[b] == ':';
                    size_t start = b;
                    bool right;
                    while (b < record->length && (bytes[b] == ':' || bytes[b] == '-')) {
                        b++;
                    }
                    right = bytes[b - 1] == ':';
                    if (b - start < (size_t)(left ? 1 : 0) + (right ? 1 : 0) + 1) {
                        composition = false;
                        break;
                    }
                    if (column_index >= columns ||
                        alignments[column_index] != (left && right ? 'c' : (left ? 'l' : (right ? 'r' : 0)))) {
                        composition = false;
                        break;
                    }
                    column_index++;
                    continue;
                }
                composition = false;
            }
            if (!composition || column_index == 0) {
                fprintf(
                    stderr,
                    "%s: table delimiter-row record does not decode to the table's alignments\n",
                    source->name
                );
                failed = 1;
            }
            break;
        }
        case MARKDOWN_CORE_CONCRETE_TABLE_PIPE:
            if (node->type != MARKDOWN_CORE_NODE_TABLE_ROW || record->length != 1 || line[record->column] != '|') {
                fprintf(stderr, "%s: table-pipe record does not spell one '|'\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE:
            if (node->type != MARKDOWN_CORE_NODE_TABLE_CELL || record->length != 1 || line[record->column] != '\\' ||
                (size_t)record->column + 1 >= line_length || line[record->column + 1] != '|') {
                fprintf(stderr, "%s: cell-escape record is not the backslash of a \\| pair\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_TASK_MARKER:
            if (node->type != MARKDOWN_CORE_NODE_LIST_ITEM || record->length != 3 || line[record->column] != '[' ||
                (line[record->column + 1] != ' ' && line[record->column + 1] != 'x' &&
                 line[record->column + 1] != 'X') ||
                line[record->column + 2] != ']' ||
                /* The last firing is the one whose state the item keeps. */
                (i == count - 1 && node->as.list.checked != (line[record->column + 1] != ' '))) {
                fprintf(stderr, "%s: task-marker record does not spell the item's checkbox\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME: {
            const char *name = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
            if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK || record->line != 0 || !name ||
                strlen(name) != record->length || memcmp(line + record->column, name, record->length) != 0) {
                fprintf(stderr, "%s: directive-name record does not spell the directive's name\n", source->name);
                failed = 1;
            }
            break;
        }
        case MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_OPEN:
            if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK || record->line != 0 || record->length != 1 ||
                line[record->column] != '[') {
                fprintf(stderr, "%s: directive-label-open record is not one '['\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_CLOSE:
            if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK || record->line != 0 || record->length != 1 ||
                line[record->column] != ']') {
                fprintf(stderr, "%s: directive-label-close record is not one ']'\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES:
            if (node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK || record->line != 0 || record->length < 2 ||
                line[record->column] != '{' || line[record->column + record->length - 1] != '}' ||
                !markdown_core_extensions_get_directive_attributes((markdown_core_node *)node)) {
                fprintf(stderr, "%s: directive-attributes record does not span its braces\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_REFDEF_LABEL: {
            /* Bracket-through-colon segments: the first starts the node's
             * spelling with `[` on its own first line, the one before the
             * destination ends `]:`, and a single-segment label's interior
             * is byte-for-byte the raw label the node keeps. */
            bool first = i == 0;
            bool last = i + 1 >= count || records[i + 1].kind != MARKDOWN_CORE_CONCRETE_REFDEF_LABEL;
            if (node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION ||
                (first && (record->line != 0 || line[record->column] != '[')) ||
                (last && (record->length < 2 || line[record->column + record->length - 1] != ':' ||
                          line[record->column + record->length - 2] != ']')) ||
                (first && last && node->as.definition.label.data &&
                 ((markdown_core_bufsize)record->length != node->as.definition.label.len + 3 ||
                  memcmp(line + record->column + 1, node->as.definition.label.data, node->as.definition.label.len) !=
                      0))) {
                fprintf(stderr, "%s: refdef label record does not spell its `[label]:` segment\n", source->name);
                failed = 1;
            }
            break;
        }
        case MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION: {
            /* The spelling; as.definition.url is the decoded scalar. Decode
             * the spelled bytes the way clean_url does — the angle form
             * loses its brackets (the scanner returns only the interior),
             * surrounding spaces trim, entities decode, backslash escapes
             * drop — and the scalar must reproduce exactly. */
            const char *bytes = line + record->column;
            size_t len = record->length;
            markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
            if (len >= 2 && bytes[0] == '<' && bytes[len - 1] == '>') {
                bytes++;
                len -= 2;
            }
            while (len && markdown_core_isspace((unsigned char)bytes[0])) {
                bytes++;
                len--;
            }
            while (len && markdown_core_isspace((unsigned char)bytes[len - 1])) {
                len--;
            }
            markdown_core_houdini_unescape_html_f(&decoded, (const uint8_t *)bytes, (markdown_core_bufsize)len);
            markdown_core_strbuf_unescape(&decoded);
            if (node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION ||
                (node->as.definition.url.data &&
                 (decoded.size != (markdown_core_bufsize)node->as.definition.url.len ||
                  memcmp(decoded.ptr, node->as.definition.url.data, decoded.size) != 0))) {
                fprintf(stderr, "%s: refdef destination record does not decode to the url\n", source->name);
                failed = 1;
            }
            markdown_core_strbuf_free(&decoded);
            break;
        }
        case MARKDOWN_CORE_CONCRETE_REFDEF_TITLE: {
            /* The first segment opens with a quote or paren, the last
             * closes with its counterpart, and a single-segment title also
             * decodes to the definition's title the way clean_title does. */
            bool first = i == 0 || records[i - 1].kind != MARKDOWN_CORE_CONCRETE_REFDEF_TITLE;
            bool last = i + 1 == count;
            int title_failed = node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION;
            if (!title_failed && first &&
                (line[record->column] != '"' && line[record->column] != '\'' && line[record->column] != '(')) {
                title_failed = 1;
            }
            if (!title_failed && last) {
                size_t j = i;
                char opener_buffer[4096];
                size_t opener_length = 0;
                while (j > 0 && records[j - 1].kind == MARKDOWN_CORE_CONCRETE_REFDEF_TITLE) {
                    j--;
                }
                if (!normalized_line(
                        source->text,
                        source->length,
                        (uint32_t)resolved + records[j].line,
                        opener_buffer,
                        sizeof(opener_buffer),
                        &opener_length
                    )) {
                    title_failed = 1;
                } else {
                    char opener = opener_buffer[records[j].column];
                    char closer = opener == '(' ? ')' : opener;
                    if (line[record->column + record->length - 1] != closer) {
                        title_failed = 1;
                    }
                }
            }
            if (!title_failed && first && last && record->length >= 2 && node->as.definition.title.data) {
                markdown_core_strbuf decoded = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
                markdown_core_houdini_unescape_html_f(
                    &decoded,
                    (const uint8_t *)line + record->column + 1,
                    (markdown_core_bufsize)record->length - 2
                );
                markdown_core_strbuf_unescape(&decoded);
                if (decoded.size != (markdown_core_bufsize)node->as.definition.title.len ||
                    memcmp(decoded.ptr, node->as.definition.title.data, decoded.size) != 0) {
                    title_failed = 1;
                }
                markdown_core_strbuf_free(&decoded);
            }
            if (title_failed) {
                fprintf(stderr, "%s: refdef title record does not spell its title segment\n", source->name);
                failed = 1;
            }
            break;
        }
        default:
            fprintf(stderr, "%s: unknown record kind %u on %s\n", source->name, record->kind, type_name(node->type));
            failed = 1;
            break;
        }
    }

    /* Ownership: records appear on exactly the owners 11.1 names, in the
     * multiplicity the grammar admits. A kind with no marker bytes — every
     * inline, Paragraph, HTMLBlock, indented code, List, Document — must
     * hold none. */
    switch (node->type) {
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
        if (count < 1) {
            fprintf(stderr, "%s: BlockQuote holds no marker records\n", source->name);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM: {
        /* The list marker, then one record per checkbox firing — the
         * scanner fires on any item line that spells one, so the tail is
         * unbounded but homogeneous. */
        bool wrong = count < 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_LIST_MARKER;
        for (i = 1; i < count && !wrong; i++) {
            wrong = records[i].kind != MARKDOWN_CORE_CONCRETE_TASK_MARKER;
        }
        if (wrong) {
            fprintf(stderr, "%s: ListItem holds a wrong record set (%zu)\n", source->name, count);
            failed = 1;
        }
        break;
    }
    case MARKDOWN_CORE_NODE_TABLE:
        if (count != 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW) {
            fprintf(stderr, "%s: Table holds %zu records, not its one delimiter row\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_TABLE_ROW:
        /* Every pipe of the row and nothing else; a pipeless row (no
         * leading, trailing, or separating pipe spelled) holds none. */
        for (i = 0; i < count; i++) {
            if (records[i].kind != MARKDOWN_CORE_CONCRETE_TABLE_PIPE) {
                fprintf(stderr, "%s: TableRow holds a non-pipe record\n", source->name);
                failed = 1;
            }
        }
        break;
    case MARKDOWN_CORE_NODE_TABLE_CELL:
        for (i = 0; i < count; i++) {
            if (records[i].kind != MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE) {
                fprintf(stderr, "%s: TableCell holds a non-escape record\n", source->name);
                failed = 1;
            }
        }
        break;
    case MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK: {
        /* The open fence, the name, then exactly what the open line spells
         * in source order — label brackets as a pair, attributes when they
         * parsed — and the close fence last iff one closed the block. The
         * label pair must agree with the label child's existence. */
        size_t at = 2;
        bool has_label_child = node->first_child && node->first_child->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
        bool has_attributes = markdown_core_extensions_get_directive_attributes((markdown_core_node *)node) != NULL;
        bool wrong = count < 2 || records[0].kind != MARKDOWN_CORE_CONCRETE_FENCE_OPEN ||
                     records[1].kind != MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME;
        if (!wrong && has_label_child) {
            wrong = count < at + 2 || records[at].kind != MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_OPEN ||
                    records[at + 1].kind != MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_CLOSE;
            at += 2;
        }
        if (!wrong) {
            /* A block directive's attributes have exactly one source
             * spelling, on the open line: field and record exist together. */
            if (has_attributes) {
                wrong = at >= count || records[at].kind != MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES;
                at++;
            } else if (at < count && records[at].kind == MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES) {
                wrong = true;
            }
        }
        if (!wrong && at < count) {
            wrong = at + 1 != count || records[at].kind != MARKDOWN_CORE_CONCRETE_FENCE_CLOSE;
        }
        if (wrong) {
            fprintf(stderr, "%s: DirectiveBlock holds a wrong record set (%zu)\n", source->name, count);
            failed = 1;
        }
        break;
    }
    case MARKDOWN_CORE_NODE_FORMULA_BLOCK:
        if (count < 1 || count > 2 || records[0].kind != MARKDOWN_CORE_CONCRETE_FENCE_OPEN ||
            (count == 2 && records[1].kind != MARKDOWN_CORE_CONCRETE_FENCE_CLOSE)) {
            fprintf(stderr, "%s: FormulaBlock holds a wrong record set (%zu)\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_HEADING:
        if (node->as.heading.setext) {
            if (count != 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE) {
                fprintf(stderr, "%s: setext heading holds %zu records\n", source->name, count);
                failed = 1;
            }
        } else if (
            count < 1 || count > 2 || records[0].kind != MARKDOWN_CORE_CONCRETE_ATX_OPENER ||
            (count == 2 && records[1].kind != MARKDOWN_CORE_CONCRETE_ATX_CLOSER)
        ) {
            fprintf(stderr, "%s: ATX heading holds a wrong record set (%zu)\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        if (node->as.code.fenced) {
            size_t expected = 1 + (node->as.code.info.len > 0 ? 1 : 0) + (node->as.code.fence_closed ? 1 : 0);
            bool wrong = count < 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_FENCE_OPEN;
            /* An info spelling that decodes to nothing (e.g. `&#32;`) is a
             * record with an empty scalar, so expected is a floor. */
            if (wrong || count < expected || count > 3 ||
                (node->as.code.fence_closed && records[count - 1].kind != MARKDOWN_CORE_CONCRETE_FENCE_CLOSE)) {
                fprintf(stderr, "%s: fenced code holds a wrong record set (%zu)\n", source->name, count);
                failed = 1;
            }
        } else if (count != 0) {
            fprintf(stderr, "%s: indented code holds %zu records\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        if (count != 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_THEMATIC_BREAK) {
            fprintf(stderr, "%s: ThematicBreak holds %zu records\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        if (count != 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER) {
            fprintf(stderr, "%s: FootnoteDefinition holds %zu records\n", source->name, count);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION: {
        /* The `[label]:` segments (one per line the label spans), then the
         * one destination the grammar requires, then the title's segments
         * when one was written. */
        size_t at = 0;
        bool wrong;
        while (at < count && records[at].kind == MARKDOWN_CORE_CONCRETE_REFDEF_LABEL) {
            at++;
        }
        wrong = at == 0 || at >= count || records[at].kind != MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION;
        for (at++; !wrong && at < count; at++) {
            wrong = records[at].kind != MARKDOWN_CORE_CONCRETE_REFDEF_TITLE;
        }
        if (wrong) {
            fprintf(stderr, "%s: ReferenceDefinition holds a wrong record set (%zu)\n", source->name, count);
            failed = 1;
        }
        break;
    }
    default:
        if (count != 0) {
            fprintf(
                stderr,
                "%s: %s holds %zu records but owns no marker bytes\n",
                source->name,
                type_name(node->type),
                count
            );
            failed = 1;
        }
        break;
    }

    for (child = node->first_child; child; child = child->next) {
        failed |= check_node_records(source, child, resolved);
    }
    return failed;
}

/* An expected record, matched field-for-field against a node's vector. */
typedef struct expected_record {
    uint8_t kind;
    uint32_t line;
    uint32_t column;
    uint32_t length;
} expected_record;

static int expect_records(
    const char *context,
    const markdown_core_node *node,
    const expected_record *expected,
    size_t expected_count
) {
    size_t count = 0;
    const markdown_core_concrete_record *records;
    size_t i;
    int failed = 0;
    if (!node) {
        fprintf(stderr, "%s: expected node is missing from the tree\n", context);
        return 1;
    }
    records = markdown_core_node_concrete_records(node, &count);
    if (count != expected_count) {
        fprintf(stderr, "%s: expected %zu records, found %zu\n", context, expected_count, count);
        return 1;
    }
    for (i = 0; i < expected_count; i++) {
        if (records[i].kind != expected[i].kind || records[i].line != expected[i].line ||
            records[i].column != expected[i].column || records[i].length != expected[i].length) {
            fprintf(
                stderr,
                "%s: record %zu is {kind %u line %u column %u length %u}, expected {%u %u %u %u}\n",
                context,
                i,
                records[i].kind,
                records[i].line,
                records[i].column,
                records[i].length,
                expected[i].kind,
                expected[i].line,
                expected[i].column,
                expected[i].length
            );
            failed = 1;
        }
    }
    return failed;
}

/* An expected inline record, matched field-for-field; defined here so the
 * block-phase shape case can pin the inline spelling of a construct it
 * repositions (the split-off lead's escape). Checker defined with the
 * inline cases below. */
typedef struct expected_inline_record {
    uint8_t kind;
    uint32_t start;
    uint32_t length;
    uint32_t head;
    uint32_t tail;
} expected_inline_record;

static int expect_inline_records(
    const char *context,
    const markdown_core_node *node,
    const expected_inline_record *expected,
    size_t expected_count
);

static size_t count_kind(const markdown_core_node *node, uint16_t type);

/* The n'th node of `type` in preorder, or NULL. */
static const markdown_core_node *nth_node_of_type(const markdown_core_node *root, uint16_t type, size_t n) {
    walk_state walk;
    const markdown_core_node *node;
    walk_init(&walk, root);
    while ((node = walk_next(&walk)) != NULL) {
        if (node->type == type) {
            if (n == 0) {
                return node;
            }
            n--;
        }
    }
    return NULL;
}

static size_t record_count_of(const markdown_core_node *node) {
    size_t count = 0;
    markdown_core_node_concrete_records(node, &count);
    return count;
}

static size_t tree_record_total(const markdown_core_node *root) {
    walk_state walk;
    const markdown_core_node *node;
    size_t total = 0;
    walk_init(&walk, root);
    while ((node = walk_next(&walk)) != NULL) {
        total += record_count_of(node);
    }
    return total;
}

/* --- capture_shape ------------------------------------------------------ */

/* Every marker kind, and every consumption edge the block phase owns: lazy
 * continuation (no `>`, no record), nested prefixes, clamped fences, a
 * close fence longer than its open, BOM-prefixed line 1, NUL replacement
 * inside an info string, CRLF, tabs after markers, the extension marker
 * material (table delimiter row, row pipes, cell `\|` escapes, task
 * checkboxes, directive and formula fences), reference-definition
 * spellings (multi-line labels and titles included, and the mid-tab lazy
 * continuation whose buffered spaces stand in for one tab byte), and the
 * constructs that must stay recordless (indented code, HTML blocks). */
static const capture_source SHAPE_SOURCES[] = {
    {"nested_quotes",
     "> # h1 ##\n"
     "> para\n"
     "> still para\n"
     ">\n"
     "> - item one\n"
     ">   continues\n"
     "> - item two\n"
     "\n"
     "> lazy quote\n"
     "lazy tail\n",
     0},
    {"deep_quotes", "> > > deep\n> > still\n", 0},
    {"fences",
     "```info string  \n"
     "body\n"
     "``````\n"
     "\n"
     "~~~~\n"
     "tilde body\n"
     "~~~\n"
     "~~~~~\n"
     "\n"
     "   ```offset\n"
     "  kept\n"
     "```\n"
     "\n"
     "```unclosed\n"
     "tail\n",
     0},
    {"setext",
     "title line\n"
     "==========\n"
     "\n"
     "second\n"
     "-\n",
     0},
    {"thematic",
     "***\r\n"
     "\r\n"
     "- - -  \r\n"
     "\r\n"
     "__ __ __\n"
     "\n"
     "para\n"
     "***\n",
     0},
    {"atx_edges",
     "#\n"
     "## x\n"
     "### y ###\n"
     "#### z #\n"
     "# #\n"
     "##\n"
     "# tab\t##\n",
     0},
    {"footnotes",
     "para with [^a] and [^long-label].\n"
     "\n"
     "[^a]: one\n"
     "\n"
     "[^long-label]:    two\n",
     0},
    {"lists",
     "- a\n"
     "+ b\n"
     "* c\n"
     "\n"
     "1. x\n"
     "\n"
     "007) y\n"
     "\n"
     "2. z\n",
     0},
    {"bom", "\xef\xbb\xbf# bom heading\n", 0},
    {"nul_info",
     "```i\0nfo\nx\n```\n\npara\0text\n\n# a\0b ##\n\n[^a\0b]: x\n",
     sizeof("```i\0nfo\nx\n```\n\npara\0text\n\n# a\0b ##\n\n[^a\0b]: x\n") - 1},
    {"entity_info", "```&#x26;amp\nx\n```\n\n```&#32;\ny\n```\n\n```  padded  \nz\n```\n", 0},
    {"tabs", ">\tq\n\n-\tt\n", 0},
    {"recordless",
     "    indented code\n"
     "\n"
     "<div>\n"
     "html\n"
     "</div>\n",
     0,
     true},
    {"interrupts",
     "foo\n"
     "***\n"
     "\n"
     "bar\n"
     "---\n",
     0},
    {"table",
     "| a | b\\|c |\n"
     "| :-: | - |\n"
     "| d | e |\n"
     "x | y\\|z\n",
     0},
    {"table_lookback",
     "lead \\| para \t\n"
     "| a | b |\n"
     "| - | - |\n"
     "x | y |\n",
     0},
    {"table_indent",
     "  | a | b |\n"
     "  | - | - |\n"
     "  | c | d |\n",
     0},
    {"tasklist",
     "- [x] done\n"
     "- [ ] todo\n"
     "* [X] caps\n"
     "- [@] not\n"
     "\n"
     "1. [x] num\n"
     "\n"
     "- [ ] first\n"
     "\n"
     "  [x] second\n",
     0},
    {"directive_blocks",
     ":::note[lbl]{#id .cls key=\"v\"}\n"
     "body\n"
     ":::\n"
     "\n"
     "::leaf\n"
     "\n"
     "::::outer\n"
     ":::inner\n"
     "inner body\n"
     ":::\n"
     "::::\n"
     "\n"
     ":::open\n"
     "tail\n",
     0},
    {"formula_blocks",
     "$$\n"
     "x + y\n"
     "$$\n"
     "\n"
     "\\\\[\n"
     "z\n"
     "\\\\]\n"
     "\n"
     "$$\n"
     "tail\n",
     0},
    {"table_lazy_header",
     "> | a |\n"
     "  | b |\n"
     "> | - | - |\n",
     0},
    {"table_crlf",
     "| a | b |\r\n"
     "| - | - |\t\r\n"
     "| c | d |\r\n",
     0},
    {"refdefs",
     "[a]: /one \"t1\"\n"
     "[b]: </two three> (t2)\n"
     "[multi\n"
     "label]: /three\n"
     "'long\n"
     "title'\n"
     "[d]: /four\n"
     "\"not title\" tail\n",
     0},
    {"refdef_lazy_tab",
     "> > [l]: /u \"a\n"
     ">\tb\"\n",
     0},
    {"table_lazy_tab",
     "> > x\n"
     ">\t| a\\|b |\n"
     "> > | - | - |\n",
     0},
};

static int case_capture_shape(void) {
    int failed = 0;
    size_t f;
    markdown_core_parse_options options = capture_options();
    for (f = 0; f < sizeof(SHAPE_SOURCES) / sizeof(SHAPE_SOURCES[0]); f++) {
        capture_source source = SHAPE_SOURCES[f];
        markdown_core_document *document;
        if (source.length == 0) {
            source.length = strlen(source.text);
        }
        document = markdown_core_document_parse((const uint8_t *)source.text, source.length, &options, NULL);
        if (!document) {
            fprintf(stderr, "capture_shape: %s failed to parse\n", source.name);
            return -1;
        }
        failed |= check_node_records(&source, document->root, 0);
        if ((tree_record_total(document->root) == 0) != source.recordless) {
            fprintf(
                stderr,
                "capture_shape: %s %s\n",
                source.name,
                source.recordless ? "captured records where no marker owns any" : "captured nothing"
            );
            failed = 1;
        }
        markdown_core_document_free(document);
    }

    /* The kind-complete fixture from the partition gates, under the same
     * per-record scrutiny: every owner among the 34 kinds — extension
     * blocks included — holds exactly its own marker records. */
    {
        capture_source source = {"kind_fixture", FIXTURE, sizeof(FIXTURE) - 1};
        markdown_core_document *document = parse_fixture();
        if (!document) {
            fprintf(stderr, "capture_shape: kind fixture failed to parse\n");
            return -1;
        }
        failed |= check_node_records(&source, document->root, 0);
        markdown_core_document_free(document);
    }

    /* Exact multiplicities where the grammar's edges hide: a lazy line and
     * a blank `>` line inside quotes, and one marker per prefix depth. */
    {
        markdown_core_document *document = markdown_core_document_parse(
            (const uint8_t *)SHAPE_SOURCES[0].text,
            strlen(SHAPE_SOURCES[0].text),
            &options,
            NULL
        );
        const markdown_core_node *quote_one;
        const markdown_core_node *quote_two;
        if (!document) {
            return -1;
        }
        quote_one = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, 0);
        quote_two = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, 1);
        if (!quote_one || record_count_of(quote_one) != 7) {
            fprintf(
                stderr,
                "capture_shape: first quote captured %zu of 7 '>' lines\n",
                quote_one ? record_count_of(quote_one) : 0
            );
            failed = 1;
        }
        if (!quote_two || record_count_of(quote_two) != 1) {
            fprintf(stderr, "capture_shape: lazy quote must capture exactly its one '>' line\n");
            failed = 1;
        }
        markdown_core_document_free(document);
    }
    /* Exact record tuples where a plausible wrong capture still spells a
     * plausible marker: nested continuation columns, closer runs, close
     * fences longer than their opens, full underlines, trimmed info
     * extents, and markers beside NUL replacements. A capture that drops,
     * widens, narrows, or misplaces any of these fails field-for-field. */
    {
        static const expected_record QUOTE_DEPTH_0[] = {
            {MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 1, 0, 1}
        };
        static const expected_record QUOTE_DEPTH_1[] = {
            {MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 0, 2, 1},
            {MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 1, 2, 1}
        };
        static const expected_record QUOTE_DEPTH_2[] = {{MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 0, 4, 1}};
        markdown_core_document *document = markdown_core_document_parse(
            (const uint8_t *)SHAPE_SOURCES[1].text,
            strlen(SHAPE_SOURCES[1].text),
            &options,
            NULL
        );
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: deep_quotes depth 0",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, 0),
            QUOTE_DEPTH_0,
            2
        );
        failed |= expect_records(
            "capture_shape: deep_quotes depth 1",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, 1),
            QUOTE_DEPTH_1,
            2
        );
        failed |= expect_records(
            "capture_shape: deep_quotes depth 2",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, 2),
            QUOTE_DEPTH_2,
            1
        );
        markdown_core_document_free(document);
    }
    {
        /* atx_edges, heading by heading: opener always, closer exactly
         * where the source writes one — total omission of the closer
         * capture fails here (the reviewed gate gap). */
        static const expected_record H_HASH[] = {{MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 1}};
        static const expected_record H_X[] = {{MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 2}};
        static const expected_record H_Y[] = {
            {MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_ATX_CLOSER, 0, 6, 3}
        };
        static const expected_record H_Z[] = {
            {MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_ATX_CLOSER, 0, 7, 1}
        };
        static const expected_record H_EMPTY_CLOSED[] = {
            {MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_ATX_CLOSER, 0, 2, 1}
        };
        static const expected_record H_TWO[] = {{MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 2}};
        static const expected_record H_TAB[] = {
            {MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_ATX_CLOSER, 0, 6, 2}
        };
        const expected_record *expected[7] = {H_HASH, H_X, H_Y, H_Z, H_EMPTY_CLOSED, H_TWO, H_TAB};
        const size_t expected_counts[7] = {1, 1, 2, 2, 2, 1, 2};
        const capture_source *atx = &SHAPE_SOURCES[5];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)atx->text, strlen(atx->text), &options, NULL);
        size_t h;
        if (!document) {
            return -1;
        }
        for (h = 0; h < 7; h++) {
            char context[64];
            snprintf(context, sizeof(context), "capture_shape: atx_edges heading %zu", h);
            failed |= expect_records(
                context,
                nth_node_of_type(document->root, MARKDOWN_CORE_NODE_HEADING, h),
                expected[h],
                expected_counts[h]
            );
        }
        markdown_core_document_free(document);
    }
    {
        /* fences, block by block: open runs at true length, info extents
         * trimmed to the byte, close runs longer than their opens kept at
         * the close's own length, the unclosed tail without a close. */
        static const expected_record FENCE_BACKTICK[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_FENCE_INFO, 0, 3, 11},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 6}
        };
        static const expected_record FENCE_TILDE[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 3, 0, 5}
        };
        static const expected_record FENCE_OFFSET[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 3, 3},
            {MARKDOWN_CORE_CONCRETE_FENCE_INFO, 0, 6, 6},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 3}
        };
        static const expected_record FENCE_UNCLOSED[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_FENCE_INFO, 0, 3, 8}
        };
        const capture_source *fences = &SHAPE_SOURCES[2];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)fences->text, strlen(fences->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: backtick fence",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_CODE_BLOCK, 0),
            FENCE_BACKTICK,
            3
        );
        failed |= expect_records(
            "capture_shape: tilde fence",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_CODE_BLOCK, 1),
            FENCE_TILDE,
            2
        );
        failed |= expect_records(
            "capture_shape: offset fence",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_CODE_BLOCK, 2),
            FENCE_OFFSET,
            3
        );
        failed |= expect_records(
            "capture_shape: unclosed fence",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_CODE_BLOCK, 3),
            FENCE_UNCLOSED,
            2
        );
        markdown_core_document_free(document);
    }
    {
        /* setext: the record is the whole underline run, not a prefix. */
        static const expected_record SETEXT_FULL[] = {{MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE, 1, 0, 10}};
        static const expected_record SETEXT_ONE[] = {{MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE, 1, 0, 1}};
        const capture_source *setext = &SHAPE_SOURCES[3];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)setext->text, strlen(setext->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: setext full underline",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_HEADING, 0),
            SETEXT_FULL,
            1
        );
        failed |= expect_records(
            "capture_shape: setext single-byte underline",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_HEADING, 1),
            SETEXT_ONE,
            1
        );
        markdown_core_document_free(document);
    }
    {
        /* nul_info: normalized-line coordinates beside NUL replacements —
         * the ATX closer entirely after one, the footnote opener holding
         * one inside its label (concrete_records.h states exactly this
         * divergence from stored-source offsets). */
        static const expected_record NUL_HEADING[] = {
            {MARKDOWN_CORE_CONCRETE_ATX_OPENER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_ATX_CLOSER, 0, 8, 2}
        };
        static const expected_record NUL_FOOTNOTE[] = {{MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER, 0, 0, 9}};
        const capture_source *nul = &SHAPE_SOURCES[9];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)nul->text, nul->length, &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: heading beside a NUL replacement",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_HEADING, 0),
            NUL_HEADING,
            2
        );
        failed |= expect_records(
            "capture_shape: footnote label holding a NUL replacement",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION, 0),
            NUL_FOOTNOTE,
            1
        );
        markdown_core_document_free(document);
    }

    /* The 255 clamp: the AST saturates, the record must not. A 300-char
     * open fence closed by a 300-char close fence keeps both true lengths
     * while fence_length reads 255. */
    {
        char text[1024];
        size_t pos = 0;
        capture_source source = {"clamped_fence", text, 0};
        markdown_core_document *document;
        const markdown_core_node *code;
        memset(text + pos, '`', 300);
        pos += 300;
        text[pos++] = '\n';
        memcpy(text + pos, "body\n", 5);
        pos += 5;
        memset(text + pos, '`', 300);
        pos += 300;
        text[pos++] = '\n';
        source.length = pos;
        document = markdown_core_document_parse((const uint8_t *)text, pos, &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= check_node_records(&source, document->root, 0);
        code = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
        if (!code || record_count_of(code) != 2) {
            fprintf(stderr, "capture_shape: clamped fence lost a record\n");
            failed = 1;
        } else {
            size_t count = 0;
            const markdown_core_concrete_record *records = markdown_core_node_concrete_records(code, &count);
            if (records[0].length != 300 || records[1].length != 300 || code->as.code.fence_length != 255) {
                fprintf(stderr, "capture_shape: clamp must saturate the AST field, never the record\n");
                failed = 1;
            }
        }
        markdown_core_document_free(document);
    }
    /* Table marker material, tuple by tuple: the delimiter row on the
     * Table itself, each row's pipes on the row, each collapsed `\|`
     * backslash on its cell — including the row spelled without a leading
     * or trailing pipe, whose single separator still records. */
    {
        static const expected_record TABLE_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 0, 11}};
        static const expected_record HEADER_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 4, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 11, 1}
        };
        static const expected_record ROW_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 4, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 8, 1}
        };
        static const expected_record BARE_PIPE[] = {{MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 2, 1}};
        static const expected_record HEADER_ESCAPE[] = {{MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE, 0, 7, 1}};
        static const expected_record DATA_ESCAPE[] = {{MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE, 0, 5, 1}};
        const capture_source *table = &SHAPE_SOURCES[14];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)table->text, strlen(table->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: table delimiter row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0),
            TABLE_DELIM,
            1
        );
        failed |= expect_records(
            "capture_shape: table header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            HEADER_PIPES,
            3
        );
        failed |= expect_records(
            "capture_shape: table data-row pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 1),
            ROW_PIPES,
            3
        );
        failed |= expect_records(
            "capture_shape: table pipeless-edge row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 2),
            BARE_PIPE,
            1
        );
        failed |= expect_records(
            "capture_shape: header cell escape",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_CELL, 1),
            HEADER_ESCAPE,
            1
        );
        failed |= expect_records(
            "capture_shape: plain header cell",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_CELL, 0),
            NULL,
            0
        );
        failed |= expect_records(
            "capture_shape: data cell escape",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_CELL, 5),
            DATA_ESCAPE,
            1
        );
        markdown_core_document_free(document);
    }
    /* The look-back header: the row the table layer recovers from the
     * paragraph's buffered content records at its true line and columns.
     * The retyped Table starts on the header row's own line — the
     * split-off lead owns the prefix — so the header pipes sit at delta 0
     * and the delimiter row at delta 1. The lead is an ordinary
     * positioned paragraph: block-recordless, its authored `\|` spelled
     * by the inline pass's ESCAPE record over its raw (not
     * pipe-unescaped) content. */
    {
        static const expected_record LOOKBACK_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 0, 9}};
        static const expected_record LOOKBACK_HEADER[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 4, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 8, 1}
        };
        static const expected_record LOOKBACK_DATA[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 2, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 6, 1}
        };
        static const expected_inline_record LOOKBACK_LEAD_ESCAPE[] = {
            {MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, 5, 1, 1, 0}
        };
        const capture_source *lookback = &SHAPE_SOURCES[15];
        const markdown_core_node *lead;
        const markdown_core_node *table;
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)lookback->text, strlen(lookback->text), &options, NULL);
        if (!document) {
            return -1;
        }
        table = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0);
        failed |= expect_records("capture_shape: lookback delimiter row", table, LOOKBACK_DELIM, 1);
        failed |= expect_records(
            "capture_shape: lookback header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            LOOKBACK_HEADER,
            3
        );
        failed |= expect_records(
            "capture_shape: lookback data pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 1),
            LOOKBACK_DATA,
            2
        );
        lead = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_PARAGRAPH, 0);
        failed |= expect_records("capture_shape: split-off lead paragraph", lead, NULL, 0);
        failed |= expect_inline_records("capture_shape: split-off lead escape", lead, LOOKBACK_LEAD_ESCAPE, 1);
        {
            int root_resolved = resolved_start_line(document->root, 0);
            int lead_start = lead ? resolved_start_line(lead, root_resolved) : 0;
            int lead_end = lead ? lead_start + lead->end_line -
                                      ((lead->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) ? 0 : lead->start_line)
                                : 0;
            if (!lead || lead_start != 1 || lead->start_column != 1 || lead_end != 1 || lead->end_column != 12) {
                fprintf(stderr, "capture_shape: the split-off lead paragraph is not positioned at its source\n");
                failed = 1;
            }
            /* The record indexes the content buffer, so the buffer itself
             * is pinned through the parsed text: the escape consumed, the
             * pipe literal, nothing pipe-collapsed away. */
            if (!lead || !lead->first_child || lead->first_child->type != MARKDOWN_CORE_NODE_TEXT ||
                lead->first_child->as.literal.len != 11 ||
                memcmp(lead->first_child->as.literal.data, "lead | para", 11) != 0) {
                fprintf(stderr, "capture_shape: the split-off lead's text is not its authored reading\n");
                failed = 1;
            }
            if (!table || resolved_start_line(table, root_resolved) != 2) {
                fprintf(stderr, "capture_shape: the split table does not start on its header row's line\n");
                failed = 1;
            }
        }
        markdown_core_document_free(document);
    }
    /* Indented table: every column in these records is a normalized-line
     * byte offset, so the two-space indent shifts all of them. */
    {
        static const expected_record INDENT_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 2, 9}};
        static const expected_record INDENT_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 2, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 6, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 10, 1}
        };
        const capture_source *indent = &SHAPE_SOURCES[16];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)indent->text, strlen(indent->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: indented delimiter row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0),
            INDENT_DELIM,
            1
        );
        failed |= expect_records(
            "capture_shape: indented header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            INDENT_PIPES,
            3
        );
        failed |= expect_records(
            "capture_shape: indented data pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 1),
            INDENT_PIPES,
            3
        );
        markdown_core_document_free(document);
    }
    /* Task checkboxes: one record per firing wherever the scanner fires —
     * bullet and ordinal items, the non-marker `[@]` firing nothing, and
     * a second firing on a later paragraph line of the same item, whose
     * state (the last spelled one) the item keeps. */
    {
        static const expected_record TASK_BULLET[] = {
            {MARKDOWN_CORE_CONCRETE_LIST_MARKER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TASK_MARKER, 0, 2, 3}
        };
        static const expected_record TASK_NONE[] = {{MARKDOWN_CORE_CONCRETE_LIST_MARKER, 0, 0, 1}};
        static const expected_record TASK_ORDINAL[] = {
            {MARKDOWN_CORE_CONCRETE_LIST_MARKER, 0, 0, 2},
            {MARKDOWN_CORE_CONCRETE_TASK_MARKER, 0, 3, 3}
        };
        static const expected_record TASK_REFIRED[] = {
            {MARKDOWN_CORE_CONCRETE_LIST_MARKER, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TASK_MARKER, 0, 2, 3},
            {MARKDOWN_CORE_CONCRETE_TASK_MARKER, 2, 2, 3}
        };
        const expected_record *expected[6] =
            {TASK_BULLET, TASK_BULLET, TASK_BULLET, TASK_NONE, TASK_ORDINAL, TASK_REFIRED};
        const size_t expected_counts[6] = {2, 2, 2, 1, 2, 3};
        const capture_source *tasks = &SHAPE_SOURCES[17];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)tasks->text, strlen(tasks->text), &options, NULL);
        size_t t;
        if (!document) {
            return -1;
        }
        for (t = 0; t < 6; t++) {
            char context[64];
            snprintf(context, sizeof(context), "capture_shape: tasklist item %zu", t);
            failed |= expect_records(
                context,
                nth_node_of_type(document->root, MARKDOWN_CORE_NODE_LIST_ITEM, t),
                expected[t],
                expected_counts[t]
            );
        }
        markdown_core_document_free(document);
    }
    /* Directive fences: open run, name, label brackets as a pair, the
     * attribute block braces included, close run at its own length and
     * line — plus the born-closed `::leaf`, nested fences at both
     * lengths, and the unclosed tail without a close. */
    {
        static const expected_record DIR_FULL[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 3, 4},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_OPEN, 0, 7, 1},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_CLOSE, 0, 11, 1},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES, 0, 12, 18},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 3}
        };
        static const expected_record DIR_LEAF[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 2},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 2, 4}
        };
        static const expected_record DIR_OUTER[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 4, 5},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 4, 0, 4}
        };
        static const expected_record DIR_INNER[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 3, 5},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 3}
        };
        static const expected_record DIR_OPEN[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 3, 4}
        };
        const expected_record *expected[5] = {DIR_FULL, DIR_LEAF, DIR_OUTER, DIR_INNER, DIR_OPEN};
        const size_t expected_counts[5] = {6, 2, 3, 3, 2};
        const capture_source *directives = &SHAPE_SOURCES[18];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)directives->text, strlen(directives->text), &options, NULL);
        size_t d;
        if (!document) {
            return -1;
        }
        for (d = 0; d < 5; d++) {
            char context[64];
            snprintf(context, sizeof(context), "capture_shape: directive block %zu", d);
            failed |= expect_records(
                context,
                nth_node_of_type(document->root, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, d),
                expected[d],
                expected_counts[d]
            );
        }
        markdown_core_document_free(document);
    }
    /* Formula fences: the CodeBlock triple's precedent minus the info
     * (the scanners accept none), in both spellings, unclosed without a
     * close record. */
    {
        static const expected_record FORMULA_CLOSED[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 2},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 2}
        };
        static const expected_record FORMULA_LATEX[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 2, 0, 3}
        };
        static const expected_record FORMULA_UNCLOSED[] = {{MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 2}};
        const capture_source *formulas = &SHAPE_SOURCES[19];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)formulas->text, strlen(formulas->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: dollar formula fences",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_FORMULA_BLOCK, 0),
            FORMULA_CLOSED,
            2
        );
        failed |= expect_records(
            "capture_shape: latex formula fences",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_FORMULA_BLOCK, 1),
            FORMULA_LATEX,
            2
        );
        failed |= expect_records(
            "capture_shape: unclosed formula fence",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_FORMULA_BLOCK, 2),
            FORMULA_UNCLOSED,
            1
        );
        markdown_core_document_free(document);
    }
    /* A lazy continuation line as the look-back header: the line entered
     * the paragraph buffer from parser->offset 0 with its two-space
     * indent included, so the mark's byte_offset (0) differs from the
     * line's first_nonspace (2) — pipe columns must come from the former
     * plus the in-buffer distance. The delimiter row's column meanwhile
     * sits after the quote's own consumed prefix. */
    {
        static const expected_record LAZY_HEADER[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 2, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 6, 1}
        };
        static const expected_record LAZY_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 2, 9}};
        const capture_source *lazy = &SHAPE_SOURCES[20];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)lazy->text, strlen(lazy->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: lazy-header delimiter row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0),
            LAZY_DELIM,
            1
        );
        failed |= expect_records(
            "capture_shape: lazy-header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            LAZY_HEADER,
            2
        );
        markdown_core_document_free(document);
    }
    /* CRLF lines and a tab after the delimiter row's last pipe: the
     * delimiter record's extent trims the tab and the whole EOL. */
    {
        static const expected_record CRLF_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 0, 9}};
        static const expected_record CRLF_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 4, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 8, 1}
        };
        const capture_source *crlf = &SHAPE_SOURCES[21];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)crlf->text, strlen(crlf->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: crlf delimiter row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0),
            CRLF_DELIM,
            1
        );
        failed |= expect_records(
            "capture_shape: crlf header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            CRLF_PIPES,
            3
        );
        failed |= expect_records(
            "capture_shape: crlf data pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 1),
            CRLF_PIPES,
            3
        );
        markdown_core_document_free(document);
    }
    /* Reference definitions: `[label]:` bracket-through-colon, the raw
     * destination (angle brackets kept), the raw title (delimiters kept) —
     * one segment record per line a label or title spans, and the
     * title-rewind definition keeps no title record because the grammar
     * rewound it back into the paragraph. */
    {
        static const expected_record RD_A[] = {
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION, 0, 5, 4},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 0, 10, 4}
        };
        static const expected_record RD_B[] = {
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION, 0, 5, 12},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 0, 18, 4}
        };
        static const expected_record RD_MULTI[] = {
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 0, 0, 6},
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 1, 0, 7},
            {MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION, 1, 8, 6},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 2, 0, 5},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 3, 0, 6}
        };
        static const expected_record RD_D[] = {
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 0, 0, 4},
            {MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION, 0, 5, 5}
        };
        const expected_record *expected[4] = {RD_A, RD_B, RD_MULTI, RD_D};
        const size_t expected_counts[4] = {3, 3, 5, 2};
        const capture_source *refdefs = &SHAPE_SOURCES[22];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)refdefs->text, strlen(refdefs->text), &options, NULL);
        size_t d;
        if (!document) {
            return -1;
        }
        for (d = 0; d < 4; d++) {
            char context[64];
            snprintf(context, sizeof(context), "capture_shape: reference definition %zu", d);
            failed |= expect_records(
                context,
                nth_node_of_type(document->root, MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, d),
                expected[d],
                expected_counts[d]
            );
        }
        /* The rewound title belongs to the paragraph, not the definition:
         * once the grammar hands those bytes back, the node's scalar must
         * agree with its missing REFDEF_TITLE record (CommonMark: "This is
         * a link reference definition, but it has no title"). Upstream
         * cmark-gfm keeps the bogus title — a registered deliberate
         * difference; micromark agrees with this reading. */
        {
            const markdown_core_node *rewound =
                nth_node_of_type(document->root, MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, 3);
            if (!rewound || rewound->as.definition.title.len != 0) {
                fprintf(stderr, "capture_shape: the rewound title leaked into the definition's scalar\n");
                failed = 1;
            }
        }
        markdown_core_document_free(document);
    }
    /* A definition whose title continues onto a lazy line that began
     * mid-tab: the buffered line holds two spaces standing in for the tab
     * byte's remaining columns, so the continuation segment's extent must
     * come out one tab byte wide where the buffer holds two spaces —
     * anything else dereferences into the wrong source bytes. */
    {
        static const expected_record RD_LAZY[] = {
            {MARKDOWN_CORE_CONCRETE_REFDEF_LABEL, 0, 4, 4},
            {MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION, 0, 9, 2},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 0, 12, 2},
            {MARKDOWN_CORE_CONCRETE_REFDEF_TITLE, 1, 1, 3}
        };
        const capture_source *lazy = &SHAPE_SOURCES[23];
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)lazy->text, strlen(lazy->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: mid-tab lazy title continuation",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, 0),
            RD_LAZY,
            4
        );
        markdown_core_document_free(document);
    }
    /* The same mid-tab lazy line as a look-back table header: the pipes'
     * and the cell escape's columns must land on the source line's own
     * bytes — the buffered stand-in spaces are two bytes where the source
     * holds one tab — the Table starts on the header row's line, and the
     * split-off lead paragraph is positioned and block-recordless. */
    {
        static const expected_record LAZYTAB_DELIM[] = {{MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW, 1, 4, 9}};
        static const expected_record LAZYTAB_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 2, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 9, 1}
        };
        static const expected_record LAZYTAB_ESCAPE[] = {{MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE, 0, 5, 1}};
        const capture_source *lazy = &SHAPE_SOURCES[24];
        const markdown_core_node *lead;
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)lazy->text, strlen(lazy->text), &options, NULL);
        if (!document) {
            return -1;
        }
        failed |= expect_records(
            "capture_shape: mid-tab lazy-header delimiter row",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE, 0),
            LAZYTAB_DELIM,
            1
        );
        failed |= expect_records(
            "capture_shape: mid-tab lazy-header pipes",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_ROW, 0),
            LAZYTAB_PIPES,
            2
        );
        failed |= expect_records(
            "capture_shape: mid-tab lazy-header cell escape",
            nth_node_of_type(document->root, MARKDOWN_CORE_NODE_TABLE_CELL, 1),
            LAZYTAB_ESCAPE,
            1
        );
        lead = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_PARAGRAPH, 0);
        failed |= expect_records("capture_shape: mid-tab split-off lead paragraph", lead, NULL, 0);
        {
            int root_resolved = resolved_start_line(document->root, 0);
            int quote_resolved = lead && lead->parent && lead->parent->parent
                                     ? resolved_start_line(lead->parent->parent, root_resolved)
                                     : root_resolved;
            int inner_resolved = lead && lead->parent ? resolved_start_line(lead->parent, quote_resolved) : 0;
            int lead_start = lead ? resolved_start_line(lead, inner_resolved) : 0;
            int lead_end = lead ? lead_start + lead->end_line -
                                      ((lead->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) ? 0 : lead->start_line)
                                : 0;
            if (!lead || lead_start != 1 || lead->start_column != 5 || lead_end != 1 || lead->end_column != 5) {
                fprintf(stderr, "capture_shape: the mid-tab split-off lead paragraph is not positioned\n");
                failed = 1;
            }
        }
        markdown_core_document_free(document);
    }
    /* A tab before the lead: node columns are byte-based (start_column is
     * first_nonspace + 1, end_column is the last line's byte length), so
     * the split-off lead's end must match what the same paragraph reports
     * unsplit — not the tab-expanded column the content-buffer marks
     * carry. */
    {
        static const char TEXT[] = ">\tlead para\n"
                                   "> | a | b |\n"
                                   "> | - | - |\n";
        const markdown_core_node *lead;
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        if (!document) {
            return -1;
        }
        lead = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_PARAGRAPH, 0);
        {
            int root_resolved = resolved_start_line(document->root, 0);
            int quote_resolved =
                lead && lead->parent ? resolved_start_line(lead->parent, root_resolved) : root_resolved;
            int lead_start = lead ? resolved_start_line(lead, quote_resolved) : 0;
            int lead_end = lead ? lead_start + lead->end_line -
                                      ((lead->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) ? 0 : lead->start_line)
                                : 0;
            if (!lead || lead_start != 1 || lead->start_column != 3 || lead_end != 1 || lead->end_column != 11) {
                fprintf(stderr, "capture_shape: the tab-led split-off lead's columns left the byte convention\n");
                failed = 1;
            }
        }
        markdown_core_document_free(document);
    }
    return failed ? -1 : 0;
}

/* --- capture_document --------------------------------------------------- */

static int case_capture_document(void) {
    int failed = 0;

    /* One-shot: the concrete owner is the one physical tree the semantic
     * root views (14.1.9), and records resolve through it. */
    {
        markdown_core_document *document = parse_fixture();
        if (!document) {
            return -1;
        }
        if (markdown_core_document_concrete(document) != markdown_core_document_root(document)) {
            fprintf(stderr, "capture_document: one-shot concrete owner is not the semantic tree\n");
            failed = 1;
        }
        if (tree_record_total(markdown_core_document_concrete(document)) == 0) {
            fprintf(stderr, "capture_document: one-shot parse captured no records\n");
            failed = 1;
        }
        if (tree_inline_record_total(markdown_core_document_concrete(document)) == 0) {
            fprintf(stderr, "capture_document: one-shot parse captured no inline records\n");
            failed = 1;
        }
        markdown_core_document_free(document);
    }

    /* Incremental: Document.concrete must reach the owner from a session's
     * committed view, across commits — not only from a one-shot parse. */
    if (!failed) {
        markdown_core_parse_options options = capture_options();
        markdown_core_session *session = markdown_core_session_open(&options, NULL);
        static const char first[] = "> quoted *q*\n> more\n\n# head #\n";
        static const char tail[] = "\n- item\n";
        const markdown_core_document *view;
        if (!session) {
            return -1;
        }
        if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)first, sizeof(first) - 1, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "capture_document: first commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        if (markdown_core_document_concrete(view) != markdown_core_document_root(view) ||
            tree_record_total(markdown_core_document_concrete(view)) == 0 ||
            tree_inline_record_total(markdown_core_document_concrete(view)) == 0) {
            fprintf(stderr, "capture_document: committed view exposes no concrete owner\n");
            failed = 1;
        }
        if (!markdown_core_session_edit(
                session,
                markdown_core_session_length(session),
                markdown_core_session_length(session),
                (const uint8_t *)tail,
                sizeof(tail) - 1,
                NULL
            ) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "capture_document: second commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        if (markdown_core_document_concrete(view) != markdown_core_document_root(view) ||
            tree_record_total(markdown_core_document_concrete(view)) == 0) {
            fprintf(stderr, "capture_document: concrete owner lost across commits\n");
            failed = 1;
        }
        markdown_core_session_free(session);
    }
    return failed ? -1 : 0;
}

/* --- capture_equivalence ------------------------------------------------ */

/* Lockstep compare of two trees on (type, record vector). The dumps of the
 * two documents are byte-identical by the equivalence suite's own gates, so
 * a shape mismatch here is reported rather than tolerated. */
static int compare_tree_records(
    const char *context,
    const markdown_core_node *committed,
    const markdown_core_node *fresh,
    int commit_index
) {
    int failed = 0;
    size_t committed_count = 0;
    size_t fresh_count = 0;
    const markdown_core_concrete_record *committed_records =
        markdown_core_node_concrete_records(committed, &committed_count);
    const markdown_core_concrete_record *fresh_records = markdown_core_node_concrete_records(fresh, &fresh_count);
    size_t committed_inline_count = 0;
    size_t fresh_inline_count = 0;
    const markdown_core_inline_concrete_record *committed_inline =
        markdown_core_node_inline_concrete_records(committed, &committed_inline_count);
    const markdown_core_inline_concrete_record *fresh_inline =
        markdown_core_node_inline_concrete_records(fresh, &fresh_inline_count);
    const markdown_core_node *committed_child = committed->first_child;
    const markdown_core_node *fresh_child = fresh->first_child;
    size_t i;

    if (committed->type != fresh->type) {
        fprintf(
            stderr,
            "%s: commit %d: tree shapes diverge (%s vs %s)\n",
            context,
            commit_index,
            type_name(committed->type),
            type_name(fresh->type)
        );
        return 1;
    }
    if (committed_count != fresh_count) {
        fprintf(
            stderr,
            "%s: commit %d: %s holds %zu records, fresh parse %zu\n",
            context,
            commit_index,
            type_name(committed->type),
            committed_count,
            fresh_count
        );
        failed = 1;
    } else {
        for (i = 0; i < committed_count; i++) {
            if (committed_records[i].line != fresh_records[i].line ||
                committed_records[i].column != fresh_records[i].column ||
                committed_records[i].length != fresh_records[i].length ||
                committed_records[i].kind != fresh_records[i].kind ||
                committed_records[i].flags != fresh_records[i].flags) {
                fprintf(
                    stderr,
                    "%s: commit %d: %s record %zu diverges "
                    "(line %u/%u column %u/%u length %u/%u kind %u/%u)\n",
                    context,
                    commit_index,
                    type_name(committed->type),
                    i,
                    committed_records[i].line,
                    fresh_records[i].line,
                    committed_records[i].column,
                    fresh_records[i].column,
                    committed_records[i].length,
                    fresh_records[i].length,
                    committed_records[i].kind,
                    fresh_records[i].kind
                );
                failed = 1;
            }
        }
    }
    if (committed_inline_count != fresh_inline_count) {
        fprintf(
            stderr,
            "%s: commit %d: %s holds %zu inline records, fresh parse %zu\n",
            context,
            commit_index,
            type_name(committed->type),
            committed_inline_count,
            fresh_inline_count
        );
        failed = 1;
    } else {
        for (i = 0; i < committed_inline_count; i++) {
            if (committed_inline[i].start != fresh_inline[i].start ||
                committed_inline[i].length != fresh_inline[i].length ||
                committed_inline[i].head != fresh_inline[i].head || committed_inline[i].tail != fresh_inline[i].tail ||
                committed_inline[i].kind != fresh_inline[i].kind ||
                committed_inline[i].flags != fresh_inline[i].flags) {
                fprintf(
                    stderr,
                    "%s: commit %d: %s inline record %zu diverges "
                    "(start %u/%u length %u/%u head %u/%u tail %u/%u kind %u/%u)\n",
                    context,
                    commit_index,
                    type_name(committed->type),
                    i,
                    committed_inline[i].start,
                    fresh_inline[i].start,
                    committed_inline[i].length,
                    fresh_inline[i].length,
                    committed_inline[i].head,
                    fresh_inline[i].head,
                    committed_inline[i].tail,
                    fresh_inline[i].tail,
                    committed_inline[i].kind,
                    fresh_inline[i].kind
                );
                failed = 1;
            }
        }
    }
    while (committed_child && fresh_child) {
        failed |= compare_tree_records(context, committed_child, fresh_child, commit_index);
        committed_child = committed_child->next;
        fresh_child = fresh_child->next;
    }
    if (committed_child || fresh_child) {
        fprintf(
            stderr,
            "%s: commit %d: child counts diverge under %s\n",
            context,
            commit_index,
            type_name(committed->type)
        );
        failed = 1;
    }
    return failed;
}

typedef struct capture_shadow {
    char *bytes;
    size_t length;
    size_t capacity;
} capture_shadow;

static bool shadow_splice(capture_shadow *shadow, size_t start, size_t end, const char *bytes, size_t length) {
    size_t grown = shadow->length - (end - start) + length;
    if (grown + 1 > shadow->capacity) {
        size_t capacity = (grown + 1) * 2;
        char *bigger = (char *)realloc(shadow->bytes, capacity);
        if (!bigger) {
            return false;
        }
        shadow->bytes = bigger;
        shadow->capacity = capacity;
    }
    memmove(shadow->bytes + start + length, shadow->bytes + end, shadow->length - end);
    memcpy(shadow->bytes + start, bytes, length);
    shadow->length = grown;
    shadow->bytes[shadow->length] = 0;
    return true;
}

/* One edit: delete `remove` at its first occurrence (when non-NULL), insert
 * `insert` there (or at `fallback_offset` when `remove` is NULL). */
typedef struct capture_edit {
    const char *remove;
    const char *insert;
    size_t fallback_offset; /* SIZE_MAX = append at end */
} capture_edit;

static const char EQUIVALENCE_INITIAL[] = "# Title\n"
                                          "\n"
                                          "> alpha [x] and [^n] here\n"
                                          "> beta continues\n"
                                          "\n"
                                          "- one\n"
                                          "  - sub item\n"
                                          "- two\n"
                                          "\n"
                                          "007. seven\n"
                                          "\n"
                                          "```fence info\n"
                                          "body\n"
                                          "```\n"
                                          "\n"
                                          "Setext head\n"
                                          "===========\n"
                                          "\n"
                                          "| a | b |\n"
                                          "| - | - |\n"
                                          "| c | d |\n"
                                          "\n"
                                          "- [ ] tick\n"
                                          "- [x] tock\n"
                                          "\n"
                                          "inline :name[lb]{.k} here\n"
                                          "\n"
                                          ":::note[lbl]\n"
                                          "directive body\n"
                                          ":::\n"
                                          "\n"
                                          "$$\n"
                                          "formula\n"
                                          "$$\n"
                                          "\n"
                                          "closing para [x] again\n"
                                          "\n"
                                          "[^n]: note body\n"
                                          "\n"
                                          "[x]: /url\n"
                                          "\n"
                                          "***\n";

/* Suffix reflow, a nested reparse, a marker edit, a lazy-continuation flip,
 * fence breakage and repair, both definition flips, and appends: the edits
 * whose locality the region-relative encoding exists to survive. The
 * extension tail then edits inside every extension owner: cell text
 * growing a `\|`, the delimiter row's alignment, a checkbox flip, the
 * directive's label and attributes, the formula body, and an inline
 * directive's respelling. */
static const capture_edit EQUIVALENCE_EDITS[] = {
    {NULL, "intro paragraph\n\n", 0},
    {"sub item", "sub itXm", 0},
    {"- two", "* two", 0},
    {"> beta continues", "beta continues", 0},
    {"\n```\n", "\n``x\n", 0},
    {"\n``x\n", "\n```\n", 0},
    {"[x]: /url\n", "", 0},
    {"[^n]: note body\n", "", 0},
    {NULL, "\n## tail ##\n", (size_t)-1},
    {NULL, "> new quote\n> more\n\n", 0},
    {"| c | d |", "| c\\|q | d |", 0},
    {"| - | - |", "| :-: | - |", 0},
    {"- [ ] tick", "- [x] tick", 0},
    {":::note[lbl]", ":::note[lbz]{#i .y}", 0},
    {"formula\n$$", "for$mula\n$$", 0},
    {":name[lb]{.k}", ":other{q=1}", 0},
};

static int case_capture_equivalence(void) {
    int failed = 0;
    markdown_core_parse_options options = capture_options();
    markdown_core_session *session = markdown_core_session_open(&options, NULL);
    capture_shadow shadow = {NULL, 0, 0};
    size_t step;

    if (!session) {
        return -1;
    }
    if (!shadow_splice(&shadow, 0, 0, EQUIVALENCE_INITIAL, sizeof(EQUIVALENCE_INITIAL) - 1) ||
        !markdown_core_session_edit(
            session,
            0,
            0,
            (const uint8_t *)EQUIVALENCE_INITIAL,
            sizeof(EQUIVALENCE_INITIAL) - 1,
            NULL
        )) {
        markdown_core_session_free(session);
        free(shadow.bytes);
        return -1;
    }

    for (step = 0; step <= sizeof(EQUIVALENCE_EDITS) / sizeof(EQUIVALENCE_EDITS[0]) && !failed; step++) {
        const markdown_core_document *view;
        markdown_core_document *fresh;
        if (step > 0) {
            const capture_edit *edit = &EQUIVALENCE_EDITS[step - 1];
            size_t start;
            size_t end;
            size_t insert_length = strlen(edit->insert);
            if (edit->remove) {
                const char *found = strstr(shadow.bytes, edit->remove);
                if (!found) {
                    fprintf(stderr, "capture_equivalence: edit %zu anchor not found\n", step);
                    failed = 1;
                    break;
                }
                start = (size_t)(found - shadow.bytes);
                end = start + strlen(edit->remove);
            } else {
                start = edit->fallback_offset == (size_t)-1 ? shadow.length : edit->fallback_offset;
                end = start;
            }
            if (!markdown_core_session_edit(session, start, end, (const uint8_t *)edit->insert, insert_length, NULL) ||
                !shadow_splice(&shadow, start, end, edit->insert, insert_length)) {
                fprintf(stderr, "capture_equivalence: edit %zu failed\n", step);
                failed = 1;
                break;
            }
        }
        if (!markdown_core_session_commit(session, NULL, NULL)) {
            fprintf(stderr, "capture_equivalence: commit %zu failed\n", step);
            failed = 1;
            break;
        }
        view = markdown_core_session_document(session);
        fresh = markdown_core_document_parse((const uint8_t *)shadow.bytes, shadow.length, &options, NULL);
        if (!fresh) {
            fprintf(stderr, "capture_equivalence: fresh parse %zu failed\n", step);
            failed = 1;
            break;
        }
        failed |= compare_tree_records(
            "capture_equivalence",
            markdown_core_document_concrete(view),
            markdown_core_document_concrete(fresh),
            (int)step
        );
        if (tree_record_total(markdown_core_document_concrete(view)) == 0) {
            fprintf(stderr, "capture_equivalence: commit %zu captured nothing\n", step);
            failed = 1;
        }
        markdown_core_document_free(fresh);
    }

    markdown_core_session_free(session);
    free(shadow.bytes);
    return failed ? -1 : 0;
}

/* --- capture_oom_sweep -------------------------------------------------- */

/* Fails exactly the countdown'th allocation, then recovers: a transient
 * loss must yield a failed parse or a complete tree, never a quietly
 * thinner one — the property the line-mark machinery already pins and the
 * capture inherits. */
typedef struct sweep_mem {
    markdown_core_mem mem;
    long countdown;
} sweep_mem;

static void *sweep_calloc(markdown_core_mem *mem, size_t count, size_t size) {
    sweep_mem *sweep = (sweep_mem *)mem;
    if (--sweep->countdown == 0) {
        return NULL;
    }
    return calloc(count, size);
}

static void *sweep_realloc(markdown_core_mem *mem, void *pointer, size_t size) {
    sweep_mem *sweep = (sweep_mem *)mem;
    if (--sweep->countdown == 0) {
        return NULL;
    }
    return realloc(pointer, size);
}

static void sweep_free(markdown_core_mem *mem, void *pointer) {
    (void)mem;
    free(pointer);
}

/* The opening definition is the document's first paragraph line on purpose:
 * its line mark is the parse's first mark allocation, so one sweep ordinal
 * lands on it and drives the harvest's marks-lost capture skip. */
static const char SWEEP_TEXT[] = "[sw]: /s \"sq\"\n"
                                 "\n"
                                 "# head *em* ##\n"
                                 "\n"
                                 "> one\n"
                                 "> two\n"
                                 "> three\n"
                                 "> four\n"
                                 "> five\n"
                                 "> six\n"
                                 "\n"
                                 "- a\n"
                                 "12. b\n"
                                 "\n"
                                 "```info\n"
                                 "x\n"
                                 "```\n"
                                 "\n"
                                 "setext\n"
                                 "======\n"
                                 "\n"
                                 "***\n"
                                 "\n"
                                 "*a* ***mix*** `co`de` [l](/u \"t\") ![i](/i) [r][x] [x] [u](no\n"
                                 "\n"
                                 "esc \\* ent &amp; auto <https://e.co/> 'q' d--e w...\n"
                                 "\n"
                                 "see [^n] and [^e&amp;e] and [^miss] end\n"
                                 "\n"
                                 "~~del~~ ~~odd~ $f*x$ :cite[a *b*]{k=v} [[cx*z]] ![[em&amp;b]]\n"
                                 "\n"
                                 "visit www.eg.com and https://a.bc here\n"
                                 "\n"
                                 "cm <!-- s --> here and $f<!--t-->g$\n"
                                 "\n"
                                 "tbl lead\n"
                                 "| h | i |\n"
                                 "| - | - |\n"
                                 "| *c* | d |\n"
                                 "\n"
                                 ":::note[*bl* lbl]\n"
                                 "body\n"
                                 ":::\n"
                                 "\n"
                                 "[^n]: def\n"
                                 "\n"
                                 "[^e&amp;e]: def two\n"
                                 "\n"
                                 "[x]: /X\n";

/* The sweep parser carries every attached extension and SMART, so a single
 * countdown covers the extension funnel's capture allocations too. */
static markdown_core_parser *sweep_parser_new(markdown_core_mem *mem) {
    static const char *extensions[] =
        {"table", "strikethrough", "autolink", "tasklist", "formula", "directive", "cross_link", "embed"};
    int options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_SMART | MARKDOWN_CORE_OPT_DIRECTIVE;
    markdown_core_parser *parser =
        mem ? markdown_core_parser_new_with_mem(options, mem) : markdown_core_parser_new(options);
    size_t i;
    if (!parser) {
        return NULL;
    }
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        markdown_core_extension *extension = markdown_core_extension_find(extensions[i]);
        if (!extension || !markdown_core_parser_attach_extension(parser, extension)) {
            markdown_core_parser_free(parser);
            return NULL;
        }
    }
    return parser;
}

static int case_capture_oom_sweep(void) {
    markdown_core_node *clean_root;
    markdown_core_parser *clean_parser = sweep_parser_new(NULL);
    long fail_at;
    bool succeeded = false;

    if (!clean_parser) {
        return -1;
    }
    markdown_core_parser_feed(clean_parser, SWEEP_TEXT, sizeof(SWEEP_TEXT) - 1);
    clean_root = markdown_core_parser_finish(clean_parser);
    if (!clean_root) {
        markdown_core_parser_free(clean_parser);
        fprintf(stderr, "capture_oom_sweep: clean parse failed\n");
        return -1;
    }

    for (fail_at = 1; fail_at < 100000 && !succeeded; fail_at++) {
        sweep_mem sweep = {{sweep_calloc, sweep_realloc, sweep_free}, fail_at};
        markdown_core_parser *parser = sweep_parser_new(&sweep.mem);
        markdown_core_node *root;
        if (!parser) {
            continue;
        }
        markdown_core_parser_feed(parser, SWEEP_TEXT, sizeof(SWEEP_TEXT) - 1);
        root = markdown_core_parser_finish(parser);
        if (root) {
            /* The injected loss fell outside the parse (or beyond its
             * allocations): the tree must carry every record the clean
             * parse does. */
            if (compare_tree_records("capture_oom_sweep", root, clean_root, (int)fail_at) != 0) {
                fprintf(stderr, "capture_oom_sweep: allocation %ld yielded a thinner tree\n", fail_at);
                markdown_core_node_free(root);
                markdown_core_parser_free(parser);
                markdown_core_node_free(clean_root);
                markdown_core_parser_free(clean_parser);
                return -1;
            }
            /* countdown > 0: the injected loss fell beyond the parse's
             * allocations, so the sweep has covered every ordinal. A loss
             * that fired (countdown <= 0) under a returned tree is legal
             * only if nothing was actually lost, which the comparison
             * above just proved — keep sweeping. */
            if (sweep.countdown > 0) {
                succeeded = true;
            }
            markdown_core_node_free(root);
        }
        markdown_core_parser_free(parser);
    }

    markdown_core_node_free(clean_root);
    markdown_core_parser_free(clean_parser);
    if (!succeeded) {
        fprintf(stderr, "capture_oom_sweep: never survived the sweep\n");
        return -1;
    }
    return 0;
}

/* --- capture_growth_ceiling --------------------------------------------- */

/* The append's refusal at the capacity wrap point, driven directly: a
 * vector whose capacity cannot double without the byte request wrapping
 * size_t must refuse the append and leave the vector untouched — the
 * failure contract cannot lean on the address-space size making the
 * ceiling unreachable (concrete_records.h). */
static int case_capture_growth_ceiling(void) {
    markdown_core_mem *mem = markdown_core_mem_default();
    markdown_core_concrete_records *vector =
        (markdown_core_concrete_records *)mem->calloc(mem, 1, sizeof(markdown_core_concrete_records));
    markdown_core_concrete_records *witness;
    int failed = 0;
    if (!vector) {
        return -1;
    }
    vector->capacity = SIZE_MAX / sizeof(markdown_core_concrete_record);
    vector->count = vector->capacity;
    witness = vector;
    if (markdown_core_concrete_records_append(mem, &vector, MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER, 0, 0, 1)) {
        fprintf(stderr, "capture_growth_ceiling: append past the wrap point reported success\n");
        failed = 1;
    }
    if (vector != witness || vector->count != vector->capacity ||
        vector->capacity != SIZE_MAX / sizeof(markdown_core_concrete_record)) {
        fprintf(stderr, "capture_growth_ceiling: refused append disturbed the vector\n");
        failed = 1;
    }
    mem->free(mem, vector);
    return failed ? -1 : 0;
}

/* --- inline capture gates ----------------------------------------------- */

/* The inline gates hold the inline-phase token records to the same standard
 * the block gates hold the marker records: every record dereferences into
 * the owning node's content buffer and spells what its kind claims, exact
 * per-fixture tuple tables pin position, length, and the reduce-time
 * consumption split, and the doctrine's negative space — plain text,
 * whitespace, hard breaks, raw HTML, invalid entities, unmatched ticks —
 * is pinned recordless, which is also what makes the incremental seam's
 * zero-record inert prefix a theorem rather than a hope. */

static size_t inline_record_count_of(const markdown_core_node *node) {
    size_t count = 0;
    markdown_core_node_inline_concrete_records(node, &count);
    return count;
}

static size_t tree_inline_record_total(const markdown_core_node *root) {
    walk_state walk;
    const markdown_core_node *node;
    size_t total = 0;
    walk_init(&walk, root);
    while ((node = walk_next(&walk)) != NULL) {
        total += inline_record_count_of(node);
    }
    return total;
}

static int expect_inline_records(
    const char *context,
    const markdown_core_node *node,
    const expected_inline_record *expected,
    size_t expected_count
) {
    size_t count = 0;
    const markdown_core_inline_concrete_record *records =
        node ? markdown_core_node_inline_concrete_records(node, &count) : NULL;
    size_t i;
    int failed = 0;
    if (!node || count != expected_count) {
        fprintf(stderr, "%s: expected %zu inline records, found %zu\n", context, expected_count, node ? count : 0);
        return 1;
    }
    for (i = 0; i < expected_count; i++) {
        if (records[i].kind != expected[i].kind || records[i].start != expected[i].start ||
            records[i].length != expected[i].length || records[i].head != expected[i].head ||
            records[i].tail != expected[i].tail) {
            fprintf(
                stderr,
                "%s: inline record %zu is {kind %u start %u length %u head %u tail %u}, "
                "expected {%u %u %u %u %u}\n",
                context,
                i,
                records[i].kind,
                records[i].start,
                records[i].length,
                records[i].head,
                records[i].tail,
                expected[i].kind,
                expected[i].start,
                expected[i].length,
                expected[i].head,
                expected[i].tail
            );
            failed = 1;
        }
    }
    return failed;
}

/* Structural invariants over a whole tree: vectors only on inline-owning
 * region nodes, records ascending without overlap inside the content
 * buffer, the consumption split within bounds, `flags` clean (no tombstone
 * survives handoff), and the spelling every kind promises unconditionally.
 * Exactness beyond this — maximality, pairing outcomes — lives in the
 * tuple tables, which is the reviewed lesson from the block slice. */
static int check_inline_invariants(const char *context, const markdown_core_node *root) {
    walk_state walk;
    const markdown_core_node *node;
    int failed = 0;

    walk_init(&walk, root);
    while ((node = walk_next(&walk)) != NULL) {
        size_t count = 0;
        const markdown_core_inline_concrete_record *records = markdown_core_node_inline_concrete_records(node, &count);
        const char *content = (const char *)node->content.ptr;
        size_t content_size = (size_t)node->content.size;
        size_t i;

        if (count > 0 && !markdown_core_node_owns_inlines((markdown_core_node *)node)) {
            fprintf(
                stderr,
                "%s: %s holds inline records but owns no inline sequence\n",
                context,
                type_name(node->type)
            );
            failed = 1;
            continue;
        }
        for (i = 0; i < count; i++) {
            const markdown_core_inline_concrete_record *record = &records[i];
            const char *bytes = content + record->start;
            if (record->length == 0 || (size_t)record->start + record->length > content_size ||
                record->head > record->length || record->tail > record->length - record->head || record->flags != 0 ||
                (i > 0 && (size_t)records[i - 1].start + records[i - 1].length > record->start)) {
                fprintf(
                    stderr,
                    "%s: %s inline record %zu breaks the vector invariants "
                    "{start %u length %u head %u tail %u flags %u}\n",
                    context,
                    type_name(node->type),
                    i,
                    record->start,
                    record->length,
                    record->head,
                    record->tail,
                    record->flags
                );
                failed = 1;
                continue;
            }
            switch (record->kind) {
            case MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN:
                if ((bytes[0] == '*' || bytes[0] == '_' || bytes[0] == '~') && !run_all(bytes, record->length)) {
                    fprintf(stderr, "%s: delimiter-run record is not one run\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE:
                if (record->length != 1 || (bytes[0] != '\'' && bytes[0] != '"') || record->head != 1) {
                    fprintf(stderr, "%s: smart-quote record does not spell a consumed quote\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH:
                if (record->length < 2 || !run_all(bytes, record->length) || bytes[0] != '-' ||
                    record->head != record->length) {
                    fprintf(stderr, "%s: smart-dash record does not spell a hyphen run\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS:
                if (record->length != 3 || memcmp(bytes, "...", 3) != 0 || record->head != 3) {
                    fprintf(stderr, "%s: smart-ellipsis record does not spell ...\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS:
                if (bytes[0] != '`' || !run_all(bytes, record->length) || record->head != record->length) {
                    fprintf(stderr, "%s: code-ticks record does not spell a consumed tick run\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE:
                if (record->length != 1 || bytes[0] != '\\' || record->head != 1) {
                    fprintf(stderr, "%s: escape record is not one consumed backslash\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_ENTITY:
                if (record->length < 3 || bytes[0] != '&' || bytes[record->length - 1] != ';' ||
                    record->head != record->length) {
                    fprintf(stderr, "%s: entity record does not spell &...;\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK:
                if (record->length < 3 || bytes[0] != '<' || bytes[record->length - 1] != '>' ||
                    record->head != record->length) {
                    fprintf(stderr, "%s: autolink record does not spell <...>\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN:
                if (!((record->length == 1 && bytes[0] == '[') ||
                      (record->length == 2 && bytes[0] == '!' && bytes[1] == '[')) ||
                    record->tail != 0 || (record->head != 0 && record->head != record->length)) {
                    fprintf(stderr, "%s: bracket-open record does not spell [ or ![\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE:
                if (record->length != 1 || bytes[0] != ']' || record->head != 1) {
                    fprintf(stderr, "%s: bracket-close record is not one consumed ]\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL:
                if ((bytes[0] != '(' && bytes[0] != '[') ||
                    (bytes[record->length - 1] != ')' && bytes[record->length - 1] != ']') ||
                    record->head != record->length) {
                    fprintf(stderr, "%s: link-tail record does not span a consumed tail\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN:
                if (record->length != 2 || memcmp(bytes, "[^", 2) != 0 || record->head != 2) {
                    fprintf(stderr, "%s: footnote-open record does not spell [^\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME:
                if (record->length < 2 || bytes[0] != ':' || record->head != record->length || record->tail != 0) {
                    fprintf(stderr, "%s: directive-name record does not spell a consumed :name\n", context);
                    failed = 1;
                }
                break;
            case MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_ATTRIBUTES:
                if (record->length < 2 || bytes[0] != '{' || bytes[record->length - 1] != '}' ||
                    record->head != record->length) {
                    fprintf(stderr, "%s: directive-attributes record does not span its braces\n", context);
                    failed = 1;
                }
                break;
            default:
                fprintf(stderr, "%s: unknown inline record kind %u\n", context, record->kind);
                failed = 1;
                break;
            }
        }
    }
    return failed;
}

/* --- inline_shape ------------------------------------------------------- */

/* One paragraph (or heading) per construct family, each pinned to its exact
 * tuple table. The reduce-time consumption split is the load-bearing part:
 * openers consume from the tail, closers from the head, `*a***b*` splits
 * its middle run both ways around a surviving literal byte, and everything
 * the doctrine leaves recordless is pinned at zero. */
static const char INLINE_SHAPE_TEXT[] = "*a* **b** ***c***\n"
                                        "\n"
                                        "*a***b*\n"
                                        "\n"
                                        "a *b c* d *e\n"
                                        "\n"
                                        "x `code` y ``li`ral`` z\n"
                                        "\n"
                                        "um `no close\n"
                                        "\n"
                                        "esc \\* pair \\\\ lone \\z\n"
                                        "\n"
                                        "hb\\\n"
                                        "after\n"
                                        "\n"
                                        "a &amp; b &#x26; c &bogus; d\n"
                                        "\n"
                                        "<https://e.co/> <x@y.zw> <b>i</b> a<b\n"
                                        "\n"
                                        "[a](/u)\n"
                                        "\n"
                                        "![i](/i \"t\")\n"
                                        "\n"
                                        "[r][x]\n"
                                        "\n"
                                        "[c][]\n"
                                        "\n"
                                        "[x] short\n"
                                        "\n"
                                        "[u](nope ] end\n"
                                        "\n"
                                        "see [^n] and [^e&amp;e] and [^miss].\n"
                                        "\n"
                                        "[*em* txt](/u)\n"
                                        "\n"
                                        "plain text one\n"
                                        "with a hard  \n"
                                        "break and soft\n"
                                        "lines here\n"
                                        "\n"
                                        "## *h* ##\n"
                                        "\n"
                                        "c <!-- hidden --> d and <b>kept</b> e\n"
                                        "\n"
                                        "f <!--x\n"
                                        "y--> g\n"
                                        "\n"
                                        "[x<!--i-->y](/u)\n"
                                        "\n"
                                        "[x]: /X\n"
                                        "\n"
                                        "[c]: /C\n"
                                        "\n"
                                        "[^n]: n body\n"
                                        "\n"
                                        "[^e&amp;e]: e body\n";

static int case_inline_shape(void) {
    int failed = 0;
    markdown_core_parse_options options = capture_options();
    markdown_core_document *document =
        markdown_core_document_parse((const uint8_t *)INLINE_SHAPE_TEXT, sizeof(INLINE_SHAPE_TEXT) - 1, &options, NULL);
    const markdown_core_node *root;

    static const expected_inline_record EMPH_TRIPLE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 2, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 4, 2, 0, 2},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 7, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 10, 3, 0, 3},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 14, 3, 3, 0},
    };
    static const expected_inline_record EMPH_MIXED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 2, 3, 1, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 6, 1, 1, 0},
    };
    static const expected_inline_record EMPH_CANDIDATE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 2, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 6, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 10, 1, 0, 0},
    };
    static const expected_inline_record TICKS[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS, 2, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS, 7, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS, 11, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS, 19, 2, 2, 0},
    };
    static const expected_inline_record ESCAPES[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, 4, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, 12, 1, 1, 0},
    };
    static const expected_inline_record HARD_BREAK[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, 2, 1, 1, 0},
    };
    static const expected_inline_record ENTITIES[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_ENTITY, 2, 5, 5, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_ENTITY, 10, 6, 6, 0},
    };
    static const expected_inline_record AUTOLINKS[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK, 0, 15, 15, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK, 16, 8, 8, 0},
    };
    static const expected_inline_record LINK_INLINE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 2, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 3, 4, 4, 0},
    };
    static const expected_inline_record IMAGE_TITLED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 3, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 4, 8, 8, 0},
    };
    static const expected_inline_record REF_FULL[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 2, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 3, 3, 3, 0},
    };
    static const expected_inline_record REF_COLLAPSED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 2, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 3, 2, 2, 0},
    };
    static const expected_inline_record REF_SHORTCUT[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 2, 1, 1, 0},
    };
    static const expected_inline_record BRACKET_CANDIDATE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 0, 0},
    };
    static const expected_inline_record FOOTNOTES_MIXED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN, 4, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 7, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN, 13, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 22, 1, 1, 0},
    };
    static const expected_inline_record LINK_INTERIOR[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 1, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 4, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 9, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 10, 4, 4, 0},
    };
    static const expected_inline_record HEADING_EMPH[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 2, 1, 1, 0},
    };
    static const expected_inline_record COMMENT_IN_LINK[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE, 11, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL, 12, 4, 4, 0},
    };

    if (!document) {
        fprintf(stderr, "inline_shape: fixture failed to parse\n");
        return -1;
    }
    root = markdown_core_document_root(document);
    failed |= check_inline_invariants("inline_shape", root);

    failed |= expect_inline_records(
        "inline_shape: emphasis triple",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 0),
        EMPH_TRIPLE,
        6
    );
    failed |= expect_inline_records(
        "inline_shape: emphasis mixed run",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 1),
        EMPH_MIXED,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: emphasis candidate",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 2),
        EMPH_CANDIDATE,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: code ticks",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 3),
        TICKS,
        4
    );
    failed |= expect_inline_records(
        "inline_shape: unmatched ticks",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 4),
        NULL,
        0
    );
    failed |= expect_inline_records(
        "inline_shape: escapes",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 5),
        ESCAPES,
        2
    );
    failed |= expect_inline_records(
        "inline_shape: hard-break escape",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 6),
        HARD_BREAK,
        1
    );
    failed |= expect_inline_records(
        "inline_shape: entities",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 7),
        ENTITIES,
        2
    );
    failed |= expect_inline_records(
        "inline_shape: autolinks",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 8),
        AUTOLINKS,
        2
    );
    failed |= expect_inline_records(
        "inline_shape: inline link",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 9),
        LINK_INLINE,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: titled image",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 10),
        IMAGE_TITLED,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: full reference",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 11),
        REF_FULL,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: collapsed reference",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 12),
        REF_COLLAPSED,
        3
    );
    failed |= expect_inline_records(
        "inline_shape: shortcut reference",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 13),
        REF_SHORTCUT,
        2
    );
    failed |= expect_inline_records(
        "inline_shape: bracket candidate",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 14),
        BRACKET_CANDIDATE,
        1
    );
    failed |= expect_inline_records(
        "inline_shape: footnote references",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 15),
        FOOTNOTES_MIXED,
        4
    );
    failed |= expect_inline_records(
        "inline_shape: link interior emphasis",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 16),
        LINK_INTERIOR,
        5
    );
    failed |= expect_inline_records(
        "inline_shape: recordless prose",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 17),
        NULL,
        0
    );
    if (!nth_node_of_type(root, MARKDOWN_CORE_NODE_LINE_BREAK, 0)) {
        fprintf(stderr, "inline_shape: recordless prose lost its hard break\n");
        failed = 1;
    }
    failed |= expect_inline_records(
        "inline_shape: heading emphasis",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_HEADING, 0),
        HEADING_EMPH,
        2
    );
    /* Inline HTML comments are ordinary raw HTML: the node keeps the exact
     * source bytes and, like all raw HTML, records nothing — including
     * inside link text, where only the link's own markup records. The
     * comment bit is the facade's derived classification, not capture
     * material. */
    failed |= expect_inline_records(
        "inline_shape: comment paragraph",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 18),
        NULL,
        0
    );
    failed |= expect_inline_records(
        "inline_shape: multi-line comment paragraph",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 19),
        NULL,
        0
    );
    failed |= expect_inline_records(
        "inline_shape: comment inside link text",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 20),
        COMMENT_IN_LINK,
        3
    );
    if (count_kind(root, MARKDOWN_CORE_NODE_HTML) < 3) {
        fprintf(stderr, "inline_shape: an inline comment lost its HTML node\n");
        failed = 1;
    }

    markdown_core_document_free(document);
    return failed ? -1 : 0;
}

/* --- inline_smart ------------------------------------------------------- */

/* SMART destroys quote spellings at scan time whether or not they pair, so
 * a paired, an unpaired, and a non-flanking quote all read fully consumed;
 * dash and ellipsis records exist exactly where the replacement does. */
static const char INLINE_SMART_TEXT[] = "'a' x\n"
                                        "\n"
                                        "it's b\n"
                                        "\n"
                                        "q ' q\n"
                                        "\n"
                                        "\"d\" e\n"
                                        "\n"
                                        "a--b ---c d----e f-g\n"
                                        "\n"
                                        "w... x.. y. z....\n"
                                        "\n"
                                        "*sm* '*q*'\n";

static int case_inline_smart(void) {
    int failed = 0;
    markdown_core_parse_options options = capture_options();
    markdown_core_document *document;
    const markdown_core_node *root;

    static const expected_inline_record QUOTE_PAIR[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 2, 1, 1, 0},
    };
    static const expected_inline_record QUOTE_APOSTROPHE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 2, 1, 1, 0},
    };
    static const expected_inline_record QUOTE_LONE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 2, 1, 1, 0},
    };
    static const expected_inline_record QUOTE_DOUBLE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 2, 1, 1, 0},
    };
    static const expected_inline_record DASHES[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH, 1, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH, 5, 3, 3, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH, 11, 4, 4, 0},
    };
    static const expected_inline_record ELLIPSES_RECORDS[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS, 1, 3, 3, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS, 13, 3, 3, 0},
    };
    static const expected_inline_record MIXED_SMART[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 3, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 5, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 6, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 8, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE, 9, 1, 1, 0},
    };

    options.smart_punctuation = true;
    document =
        markdown_core_document_parse((const uint8_t *)INLINE_SMART_TEXT, sizeof(INLINE_SMART_TEXT) - 1, &options, NULL);
    if (!document) {
        fprintf(stderr, "inline_smart: fixture failed to parse\n");
        return -1;
    }
    root = markdown_core_document_root(document);
    failed |= check_inline_invariants("inline_smart", root);
    failed |= expect_inline_records(
        "inline_smart: paired quotes",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 0),
        QUOTE_PAIR,
        2
    );
    failed |= expect_inline_records(
        "inline_smart: apostrophe",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 1),
        QUOTE_APOSTROPHE,
        1
    );
    failed |= expect_inline_records(
        "inline_smart: non-flanking quote",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 2),
        QUOTE_LONE,
        1
    );
    failed |= expect_inline_records(
        "inline_smart: double quotes",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 3),
        QUOTE_DOUBLE,
        2
    );
    failed |= expect_inline_records(
        "inline_smart: dashes",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 4),
        DASHES,
        3
    );
    failed |= expect_inline_records(
        "inline_smart: ellipses",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 5),
        ELLIPSES_RECORDS,
        2
    );
    failed |= expect_inline_records(
        "inline_smart: quoted emphasis",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 6),
        MIXED_SMART,
        6
    );
    markdown_core_document_free(document);
    return failed ? -1 : 0;
}

/* --- inline_extension_funnel -------------------------------------------- */

/* Every extension flows through the one engine funnel: delimiter records
 * appear with exact spans, a successful range reduce consumes both
 * endpoints, a no-op reduce (mismatched strikethrough) leaves candidates,
 * formula and cross-reference re-borrow raw interiors and must retract the
 * interior records they orphan, directive keeps its label children and
 * their records, and the autolink extension records nothing at all — a
 * bare autolink owns no markup byte. The inline directive's scan-time
 * consumes are the one sanctioned capture outside the engine: `:name`
 * and a nameside `{attrs}` record at their consume_source site, while a
 * labeled form's `]{attrs}` stays the engine closer's run. */
static const char INLINE_FUNNEL_TEXT[] = "~~del~~ und ~one~ mm ~~no~\n"
                                         "\n"
                                         "$a*b$ y $c&amp;d$\n"
                                         "\n"
                                         "[[note*x]] ![[img&amp;v]]\n"
                                         "\n"
                                         "visit www.eg.com now https://a.bc end x@y.de q\n"
                                         "\n"
                                         "$a<!--c-->b$\n"
                                         "\n"
                                         "| h1 | h2 |\n"
                                         "| - | - |\n"
                                         "| *c* | d |\n"
                                         "\n"
                                         ":::note[*bl* lbl]\n"
                                         "body\n"
                                         ":::\n"
                                         "\n"
                                         ":a one :b[lb]{.c} two\n"
                                         "\n"
                                         ":d{k=v} three :e{=bad} four\n";

static int case_inline_extension_funnel(void) {
    int failed = 0;
    markdown_core_parse_options options = capture_options();
    markdown_core_document *document = markdown_core_document_parse(
        (const uint8_t *)INLINE_FUNNEL_TEXT,
        sizeof(INLINE_FUNNEL_TEXT) - 1,
        &options,
        NULL
    );
    const markdown_core_node *root;

    static const expected_inline_record STRIKE[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 5, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 12, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 16, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 21, 2, 0, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 25, 1, 0, 0},
    };
    static const expected_inline_record FORMULA_PAIRS[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 4, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 8, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 16, 1, 1, 0},
    };
    static const expected_inline_record CROSS_EMBED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 8, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 11, 3, 3, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 23, 2, 2, 0},
    };
    static const expected_inline_record CELL_EMPH[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 2, 1, 1, 0},
    };
    static const expected_inline_record LABEL_EMPH[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 0, 1},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 3, 1, 1, 0},
    };
    static const expected_inline_record FORMULA_COMMENT[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 0, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 11, 1, 1, 0},
    };
    /* The labeled form: `:name` records at its scan-time consume, the `[`
     * through the engine, the `]{attrs}` closer as the engine's one run.
     * The bare `:a` records name-only. */
    static const expected_inline_record DIRECTIVE_LABELED[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME, 0, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME, 7, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 9, 1, 1, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN, 12, 5, 5, 0},
    };
    /* The attribute form consumes `:name{attrs}` in one scan and records
     * it as two spellings; attributes that fail to parse stay literal and
     * record nothing beside their name. */
    static const expected_inline_record DIRECTIVE_ATTRS[] = {
        {MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME, 0, 2, 2, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_ATTRIBUTES, 2, 5, 5, 0},
        {MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME, 14, 2, 2, 0},
    };

    if (!document) {
        fprintf(stderr, "inline_extension_funnel: fixture failed to parse\n");
        return -1;
    }
    root = markdown_core_document_root(document);
    failed |= check_inline_invariants("inline_extension_funnel", root);
    failed |= expect_inline_records(
        "inline_extension_funnel: strikethrough",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 0),
        STRIKE,
        6
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: formula",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 1),
        FORMULA_PAIRS,
        4
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: cross-link and embed",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 2),
        CROSS_EMBED,
        4
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: autolink extension",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 3),
        NULL,
        0
    );
    /* A comment inside a formula body: the reducer re-borrows the raw
     * interior, so the comment's parsed records must be retracted with
     * the rest and the bytes stay spelled. */
    failed |= expect_inline_records(
        "inline_extension_funnel: comment inside formula",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 4),
        FORMULA_COMMENT,
        2
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: table cell emphasis",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_TABLE_CELL, 2),
        CELL_EMPH,
        2
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: directive label",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, 0),
        LABEL_EMPH,
        2
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: labeled inline directives",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 6),
        DIRECTIVE_LABELED,
        4
    );
    failed |= expect_inline_records(
        "inline_extension_funnel: attribute inline directives",
        nth_node_of_type(root, MARKDOWN_CORE_NODE_PARAGRAPH, 7),
        DIRECTIVE_ATTRS,
        3
    );
    markdown_core_document_free(document);
    return failed ? -1 : 0;
}

/* --- inline_seam_barrier ------------------------------------------------ */

/* The seam contract the capture leans on: every byte a capture site can
 * begin a record at is a seam barrier (or, for the tail records, sits
 * strictly after a barrier `]` of the same construct), so the inert prefix
 * an incremental reparse fast-forwards over holds zero records and no
 * record ever needs transplanting. SMART bytes are checked behaviorally
 * because the seam function owns their clause. */
static int case_inline_seam_barrier(void) {
    static const unsigned char RECORD_TRIGGER_BYTES[] = {'*', '_', '`', '\\', '&', '<', '[', ']', '!', '~', '$', ':'};
    static const unsigned char SMART_TRIGGER_BYTES[] = {'-', '.', '\'', '"'};
    int failed = 0;
    markdown_core_parser *parser = sweep_parser_new(NULL);
    size_t i;

    if (!parser) {
        return -1;
    }
    for (i = 0; i < sizeof(RECORD_TRIGGER_BYTES); i++) {
        if (!parser->inline_config->seam_barrier_chars[RECORD_TRIGGER_BYTES[i]]) {
            fprintf(
                stderr,
                "inline_seam_barrier: record trigger byte '%c' is not a seam barrier\n",
                RECORD_TRIGGER_BYTES[i]
            );
            failed = 1;
        }
    }
    for (i = 0; i < sizeof(SMART_TRIGGER_BYTES); i++) {
        unsigned char line[8] = {'x', 'x', 0, '\n', 'y', 'y', 0, 0};
        unsigned char other[8] = {'x', 'x', 0, '\n', 'z', 'z', 0, 0};
        markdown_core_bufsize seam;
        line[2] = SMART_TRIGGER_BYTES[i];
        other[2] = SMART_TRIGGER_BYTES[i];
        seam = markdown_core_inline_seam_prefix(parser, line, 6, other, 6, MARKDOWN_CORE_OPT_SMART);
        if (seam != 0) {
            fprintf(
                stderr,
                "inline_seam_barrier: smart byte '%c' fell inside a seam prefix (seam %d)\n",
                SMART_TRIGGER_BYTES[i],
                (int)seam
            );
            failed = 1;
        }
    }
    /* The two-space hard break is seam-admissible, which is exactly why it
     * must own no record: the prefix a reparse never rescans still equals
     * the fresh parse record-for-record only if breaks stay implicit. */
    {
        static const unsigned char broken_a[] = "ab  \ncd";
        static const unsigned char broken_b[] = "ab  \ncq";
        markdown_core_bufsize seam =
            markdown_core_inline_seam_prefix(parser, broken_a, 7, broken_b, 7, MARKDOWN_CORE_OPT_SMART);
        if (seam != 5) {
            fprintf(stderr, "inline_seam_barrier: hard-break prefix must stay seam-admissible (seam %d)\n", (int)seam);
            failed = 1;
        }
    }
    markdown_core_parser_free(parser);
    return failed ? -1 : 0;
}

/* --- inline_equivalence ------------------------------------------------- */

/* The two incremental paths the inline vectors must survive, each proven to
 * have actually run before its records are compared:
 *
 * - the seam fast-forward: the staged leaf never rescans its inert prefix,
 *   the old prefix children move over physically (pointer identity), and
 *   the staged vector is already complete because the prefix owns no
 *   records;
 * - the dependent rebuild: a definition flip rebuilds a unit the edit never
 *   touched, the stable owner keeps its node (pointer identity) while
 *   {content, children, inline records} swap in — a vector left behind by
 *   the swap is exactly what the fresh-parse comparison catches. */
static int case_inline_equivalence(void) {
    int failed = 0;
    markdown_core_parse_options options = capture_options();

    /* Seam transplant. */
    {
        static const char initial[] = "plain one\nplain two\nedit *here* soon\n";
        static const char replaced[] = "*there* now\n";
        markdown_core_session *session = markdown_core_session_open(&options, NULL);
        const markdown_core_document *view;
        const markdown_core_node *paragraph;
        const markdown_core_node *prefix_text;
        markdown_core_document *fresh;
        static const char final_text[] = "plain one\nplain two\nedit *there* now\n";

        if (!session) {
            return -1;
        }
        if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: seam first commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        paragraph = markdown_core_document_root(view)->first_child;
        prefix_text = paragraph ? paragraph->first_child : NULL;
        if (!paragraph || paragraph->type != MARKDOWN_CORE_NODE_PARAGRAPH || !prefix_text) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: seam fixture lost its paragraph\n");
            return -1;
        }
        if (inline_record_count_of(paragraph) == 0) {
            fprintf(stderr, "inline_equivalence: seam paragraph captured nothing to compare\n");
            failed = 1;
        }
        if (!markdown_core_session_edit(session, 25, 36, (const uint8_t *)replaced, sizeof(replaced) - 1, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: seam second commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        if (!markdown_core_document_root(view)->first_child ||
            markdown_core_document_root(view)->first_child->first_child != prefix_text) {
            fprintf(stderr, "inline_equivalence: seam transplant did not engage (prefix child was reparsed)\n");
            failed = 1;
        }
        if (markdown_core_document_root(view)->first_child &&
            markdown_core_document_root(view)->first_child->user_data != NULL) {
            fprintf(stderr, "inline_equivalence: committed leaf retained its seam user_data\n");
            failed = 1;
        }
        fresh = markdown_core_document_parse((const uint8_t *)final_text, sizeof(final_text) - 1, &options, NULL);
        if (!fresh) {
            markdown_core_session_free(session);
            return -1;
        }
        failed |= compare_tree_records(
            "inline_equivalence: seam",
            markdown_core_document_concrete(view),
            markdown_core_document_concrete(fresh),
            1
        );
        failed |= check_inline_invariants("inline_equivalence: seam", markdown_core_document_root(view));
        markdown_core_document_free(fresh);
        markdown_core_session_free(session);
    }

    /* Dependent rebuild across a definition flip, both directions. */
    if (!failed) {
        static const char initial[] = "alpha [a][x] beta [^n] gamma\n"
                                      "\n"
                                      "filler para\n"
                                      "\n"
                                      "[x]: /u\n"
                                      "\n"
                                      "[^n]: note\n";
        static const char without_def[] = "alpha [a][x] beta [^n] gamma\n"
                                          "\n"
                                          "filler para\n"
                                          "\n"
                                          "\n"
                                          "[^n]: note\n";
        markdown_core_session *session = markdown_core_session_open(&options, NULL);
        const markdown_core_document *view;
        const markdown_core_node *unit;
        markdown_core_document *fresh;

        if (!session) {
            return -1;
        }
        if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: dependent first commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        unit = markdown_core_document_root(view)->first_child;
        if (!unit || inline_record_count_of(unit) == 0) {
            fprintf(stderr, "inline_equivalence: dependent unit captured nothing to compare\n");
            failed = 1;
        }
        /* Delete the `[x]: /u\n` line, bytes 43..50 plus its newline. */
        if (!markdown_core_session_edit(session, 43, 51, (const uint8_t *)"", 0, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: definition removal commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        if (markdown_core_document_root(view)->first_child != unit) {
            fprintf(stderr, "inline_equivalence: dependent rebuild replaced its stable owner\n");
            failed = 1;
        }
        fresh = markdown_core_document_parse((const uint8_t *)without_def, sizeof(without_def) - 1, &options, NULL);
        if (!fresh) {
            markdown_core_session_free(session);
            return -1;
        }
        failed |= compare_tree_records(
            "inline_equivalence: definition removed",
            markdown_core_document_concrete(view),
            markdown_core_document_concrete(fresh),
            2
        );
        markdown_core_document_free(fresh);

        /* Flip it back: the same unit swaps again to the resolved shape. */
        if (!failed) {
            if (!markdown_core_session_edit(session, 43, 43, (const uint8_t *)"[x]: /u\n", 8, NULL) ||
                !markdown_core_session_commit(session, NULL, NULL)) {
                markdown_core_session_free(session);
                fprintf(stderr, "inline_equivalence: definition restore commit failed\n");
                return -1;
            }
            view = markdown_core_session_document(session);
            if (markdown_core_document_root(view)->first_child != unit) {
                fprintf(stderr, "inline_equivalence: restore rebuild replaced its stable owner\n");
                failed = 1;
            }
            fresh = markdown_core_document_parse((const uint8_t *)initial, sizeof(initial) - 1, &options, NULL);
            if (!fresh) {
                markdown_core_session_free(session);
                return -1;
            }
            failed |= compare_tree_records(
                "inline_equivalence: definition restored",
                markdown_core_document_concrete(view),
                markdown_core_document_concrete(fresh),
                3
            );
            failed |= check_inline_invariants("inline_equivalence: dependent", markdown_core_document_root(view));
            markdown_core_document_free(fresh);
        }
        markdown_core_session_free(session);
    }

    /* Dependent rebuild of a table cell: a definition flip converts a
     * reference inside the cell without reparsing the table's block
     * structure — the Table and the cell keep pointer identity (a full
     * reparse would mint new nodes and silently satisfy the dump
     * comparison), the cell's inline records swap with its domain, and
     * its block-side records (the row's pipes, the table's delimiter
     * row) ride the stable nodes unchanged. */
    if (!failed) {
        /* The cell carries a `\|` so it holds a block-side escape record:
         * the dependent rebuild swaps {children, content, inline records}
         * and must leave the cell's block records exactly in place. */
        static const char initial[] = "| pre\\|q [a][x] | b |\n"
                                      "| - | - |\n"
                                      "\n"
                                      "filler para\n"
                                      "\n"
                                      "[x]: /u\n"
                                      "\n"
                                      "tail para\n";
        static const char without_def[] = "| pre\\|q [a][x] | b |\n"
                                          "| - | - |\n"
                                          "\n"
                                          "filler para\n"
                                          "\n"
                                          "\n"
                                          "tail para\n";
        markdown_core_session *session = markdown_core_session_open(&options, NULL);
        const markdown_core_document *view;
        const markdown_core_node *table;
        const markdown_core_node *cell;
        markdown_core_document *fresh;

        if (!session) {
            return -1;
        }
        if (!markdown_core_session_edit(session, 0, 0, (const uint8_t *)initial, sizeof(initial) - 1, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: cell first commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        table = markdown_core_document_root(view)->first_child;
        cell = nth_node_of_type(markdown_core_document_root(view), MARKDOWN_CORE_NODE_TABLE_CELL, 0);
        if (!table || table->type != MARKDOWN_CORE_NODE_TABLE || !cell) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: cell fixture lost its table\n");
            return -1;
        }
        if (record_count_of(cell) == 0) {
            fprintf(stderr, "inline_equivalence: cell fixture holds no block record to keep in place\n");
            failed = 1;
        }
        /* Delete the `[x]: /u\n` definition line. */
        if (!markdown_core_session_edit(session, 46, 54, (const uint8_t *)"", 0, NULL) ||
            !markdown_core_session_commit(session, NULL, NULL)) {
            markdown_core_session_free(session);
            fprintf(stderr, "inline_equivalence: cell definition removal commit failed\n");
            return -1;
        }
        view = markdown_core_session_document(session);
        if (markdown_core_document_root(view)->first_child != table ||
            nth_node_of_type(markdown_core_document_root(view), MARKDOWN_CORE_NODE_TABLE_CELL, 0) != cell) {
            fprintf(stderr, "inline_equivalence: cell dependent rebuild replaced its stable owner\n");
            failed = 1;
        }
        fresh = markdown_core_document_parse((const uint8_t *)without_def, sizeof(without_def) - 1, &options, NULL);
        if (!fresh) {
            markdown_core_session_free(session);
            return -1;
        }
        failed |= compare_tree_records(
            "inline_equivalence: cell definition removed",
            markdown_core_document_concrete(view),
            markdown_core_document_concrete(fresh),
            2
        );
        failed |= check_inline_invariants("inline_equivalence: cell dependent", markdown_core_document_root(view));
        markdown_core_document_free(fresh);
        markdown_core_session_free(session);
    }
    return failed ? -1 : 0;
}

/* --- inline_growth_ceiling ---------------------------------------------- */

/* The inline twin of capture_growth_ceiling: at the doubling wrap point the
 * append refuses and disturbs nothing. */
static int case_inline_growth_ceiling(void) {
    markdown_core_mem *mem = markdown_core_mem_default();
    markdown_core_concrete_capture capture;
    markdown_core_inline_concrete_records *vector =
        (markdown_core_inline_concrete_records *)mem->calloc(mem, 1, sizeof(markdown_core_inline_concrete_records));
    markdown_core_inline_concrete_records *witness;
    int failed = 0;
    if (!vector) {
        return -1;
    }
    markdown_core_concrete_capture_init(&capture, mem);
    vector->capacity = SIZE_MAX / sizeof(markdown_core_inline_concrete_record);
    vector->count = vector->capacity;
    capture.records = vector;
    witness = vector;
    if (markdown_core_concrete_capture_append(&capture, MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE, 0, 1, 1, 0)) {
        fprintf(stderr, "inline_growth_ceiling: append past the wrap point reported success\n");
        failed = 1;
    }
    if (capture.records != witness || vector->count != vector->capacity ||
        vector->capacity != SIZE_MAX / sizeof(markdown_core_inline_concrete_record)) {
        fprintf(stderr, "inline_growth_ceiling: refused append disturbed the vector\n");
        failed = 1;
    }
    /* The retraction-span vector's own wrap point, same drive: a fabricated
     * full capacity at the ceiling must refuse the note untouched. */
    capture.retraction_capacity = SIZE_MAX / sizeof(markdown_core_concrete_retraction);
    capture.retraction_count = capture.retraction_capacity;
    if (markdown_core_concrete_capture_retract_span(&capture, 0, 1)) {
        fprintf(stderr, "inline_growth_ceiling: span note past the wrap point reported success\n");
        failed = 1;
    }
    if (capture.retractions != NULL || capture.retraction_count != capture.retraction_capacity ||
        capture.retraction_capacity != SIZE_MAX / sizeof(markdown_core_concrete_retraction)) {
        fprintf(stderr, "inline_growth_ceiling: refused span note disturbed the capture\n");
        failed = 1;
    }
    mem->free(mem, vector);
    return failed ? -1 : 0;
}

/* --- case table --------------------------------------------------------- */

typedef struct concrete_case {
    const char *name;
    int (*run)(void);
} concrete_case;

/* --- recovery gates (14.1.10) ------------------------------------------- */

/* Nodes of `type` in the subtree rooted at `node`, the node included. */
static size_t count_kind(const markdown_core_node *node, uint16_t type) {
    const markdown_core_node *child;
    size_t total = node->type == type ? 1 : 0;
    for (child = node->first_child; child; child = child->next) {
        total += count_kind(child, type);
    }
    return total;
}

/* Concatenates the subtree's Text literals, a newline per SoftBreak — the
 * literal-content observable: when nothing may be guessed, every authored
 * byte reappears here in order. */
static void concat_inline_text(const markdown_core_node *node, markdown_core_strbuf *out) {
    const markdown_core_node *child;
    if (node->type == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_strbuf_put(out, node->as.literal.data, node->as.literal.len);
    } else if (node->type == MARKDOWN_CORE_NODE_SOFT_BREAK) {
        markdown_core_strbuf_putc(out, '\n');
    }
    for (child = node->first_child; child; child = child->next) {
        concat_inline_text(child, out);
    }
}

/* Every inline record in the subtree must be an unconsumed candidate: a
 * delimiter run or bracket opener whose head and tail are still zero. Any
 * other kind, or any consumed byte, means the parse turned an unmatched
 * candidate into structure. */
static int expect_unconsumed_runs(const char *context, const markdown_core_node *node) {
    const markdown_core_node *child;
    size_t count = 0;
    const markdown_core_inline_concrete_record *records = markdown_core_node_inline_concrete_records(node, &count);
    size_t i;
    int failed = 0;
    for (i = 0; i < count; i++) {
        if ((records[i].kind != MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN &&
             records[i].kind != MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN) ||
            records[i].head != 0 || records[i].tail != 0) {
            fprintf(
                stderr,
                "%s: record %zu {kind %u head %u tail %u} is consumed markup in literal fallback\n",
                context,
                i,
                records[i].kind,
                records[i].head,
                records[i].tail
            );
            failed = 1;
        }
    }
    for (child = node->first_child; child; child = child->next) {
        failed |= expect_unconsumed_runs(context, child);
    }
    return failed;
}

/* 14.1.10 clause (a): unmatched core Markdown and unmatched island openers
 * become literal content — no recovery structure, no guessed pairing, no
 * consumed markup, every authored byte in the Text spine. The engine has no
 * MissingToken/UnexpectedToken/ErrorRegion kinds to count, so the gate pins
 * the observables that adding any of them (or any structure-guessing) would
 * move: forbidden node kinds at zero, byte-exact literal reconstruction,
 * and every surviving inline record an unconsumed candidate. */
static int case_recovery_literal_fallback(void) {
    typedef struct literal_fixture {
        const char *name;
        const char *text;
        const char *expected;
        uint16_t forbidden[4];
    } literal_fixture;
    static const literal_fixture FIXTURES[] = {
        {"emphasis",
         "*a and **b and __c\n",
         "*a and **b and __c",
         {MARKDOWN_CORE_NODE_EMPHASIS, MARKDOWN_CORE_NODE_STRONG, 0, 0}},
        {"brackets",
         "[foo and ![bar and a] end\n",
         "[foo and ![bar and a] end",
         {MARKDOWN_CORE_NODE_LINK, MARKDOWN_CORE_NODE_IMAGE, 0, 0}},
        {"code_span", "`tick and ``run\n", "`tick and ``run", {MARKDOWN_CORE_NODE_CODE, 0, 0, 0}},
        {"angle", "<notag and < loose\n", "<notag and < loose", {MARKDOWN_CORE_NODE_HTML, 0, 0, 0}},
        {"formula_inline",
         "$x and $$y and \\\\(z end\n",
         "$x and $$y and \\\\(z end",
         {MARKDOWN_CORE_NODE_FORMULA, 0, 0, 0}},
        {"cross_link",
         "[[target and ![[embed end\n",
         "[[target and ![[embed end",
         {MARKDOWN_CORE_NODE_CROSS_LINK, MARKDOWN_CORE_NODE_EMBED, 0, 0}},
        {"footnote_ref",
         "[^x unclosed and [^y] undefined\n",
         "[^x unclosed and [^y] undefined",
         {MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE, MARKDOWN_CORE_NODE_LINK, 0, 0}},
        {"strikethrough", "~~open and ~odd\n", "~~open and ~odd", {MARKDOWN_CORE_NODE_STRIKETHROUGH, 0, 0, 0}},
        {"across_lines",
         "*a\nstill *b\n",
         "*a\nstill *b",
         {MARKDOWN_CORE_NODE_EMPHASIS, MARKDOWN_CORE_NODE_STRONG, 0, 0}},
    };
    markdown_core_parse_options options = capture_options();
    int failed = 0;
    size_t f;
    for (f = 0; f < sizeof(FIXTURES) / sizeof(FIXTURES[0]); f++) {
        const literal_fixture *fixture = &FIXTURES[f];
        capture_source source = {fixture->name, fixture->text, strlen(fixture->text), false};
        markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
        size_t k;
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)fixture->text, strlen(fixture->text), &options, NULL);
        if (!document) {
            markdown_core_strbuf_free(&text);
            return -1;
        }
        for (k = 0; k < 4 && fixture->forbidden[k]; k++) {
            if (count_kind(markdown_core_document_root(document), fixture->forbidden[k]) != 0) {
                fprintf(stderr, "%s: literal fallback produced a %u node\n", fixture->name, fixture->forbidden[k]);
                failed = 1;
            }
        }
        concat_inline_text(markdown_core_document_root(document), &text);
        if (text.size != strlen(fixture->expected) || memcmp(text.ptr, fixture->expected, text.size) != 0) {
            fprintf(
                stderr,
                "%s: literal reconstruction is \"%.*s\", expected \"%s\"\n",
                fixture->name,
                (int)text.size,
                text.ptr,
                fixture->expected
            );
            failed = 1;
        }
        failed |= expect_unconsumed_runs(fixture->name, markdown_core_document_root(document));
        if (tree_record_total(markdown_core_document_root(document)) != 0) {
            fprintf(stderr, "%s: literal fallback captured block records\n", fixture->name);
            failed = 1;
        }
        failed |= check_node_records(&source, markdown_core_document_root(document), 0);
        markdown_core_strbuf_free(&text);
        markdown_core_document_free(document);
    }
    return failed ? -1 : 0;
}

/* 14.1.10 clause (b): a committed bounded island recovers only inside its
 * own boundary — its grammar's termination rule, or the parent container
 * that closes it — preserves all authored source, and never consumes an
 * unrelated following region. Eligibility binds to the form's commit
 * point: a malformed opener line never commits and stays a paragraph; the
 * inline directive's name form commits at scan and stands while its
 * unclosed label falls back around it. */
static int case_recovery_island_boundary(void) {
    markdown_core_parse_options options = capture_options();
    int failed = 0;

    /* An unclosed fence absorbs every later line — headings, quotes — as
     * its own literal, to EOF. That is its termination rule, and every
     * authored byte is in the literal. */
    {
        static const char TEXT[] = "```\n# h\n> q\n";
        static const char BODY[] = "# h\n> q\n";
        static const expected_record OPEN_ONLY[] = {{MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3}};
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *code;
        if (!document) {
            return -1;
        }
        code = nth_node_of_type(markdown_core_document_root(document), MARKDOWN_CORE_NODE_CODE_BLOCK, 0);
        if (!code || code->as.code.fence_closed || code->as.code.literal.len != sizeof(BODY) - 1 ||
            memcmp(code->as.code.literal.data, BODY, sizeof(BODY) - 1) != 0) {
            fprintf(stderr, "island_boundary: unclosed fence did not keep its authored body\n");
            failed = 1;
        }
        if (count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_HEADING) != 0 ||
            count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_BLOCK_QUOTE) != 0) {
            fprintf(stderr, "island_boundary: unclosed fence let a later construct escape\n");
            failed = 1;
        }
        failed |= expect_records("island_boundary: unclosed fence records", code, OPEN_ONLY, 1);
        markdown_core_document_free(document);
    }
    /* Same rule for the $$ island, with the leading paragraph untouched. */
    {
        static const char TEXT[] = "before\n\n$$\nx + y\n# not a heading\n";
        static const expected_record OPEN_ONLY[] = {{MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 2}};
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *formula;
        const char *literal;
        if (!document) {
            return -1;
        }
        formula = nth_node_of_type(markdown_core_document_root(document), MARKDOWN_CORE_NODE_FORMULA_BLOCK, 0);
        literal = formula ? markdown_core_extensions_get_formula_literal((markdown_core_node *)formula) : NULL;
        if (!literal || strcmp(literal, "x + y\n# not a heading") != 0) {
            fprintf(stderr, "island_boundary: unclosed formula block did not keep its authored body\n");
            failed = 1;
        }
        if (count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_HEADING) != 0) {
            fprintf(stderr, "island_boundary: unclosed formula let a heading escape\n");
            failed = 1;
        }
        failed |= expect_records("island_boundary: unclosed formula records", formula, OPEN_ONLY, 1);
        markdown_core_document_free(document);
    }
    /* The island's boundary is also its parent's: a quote-nested unclosed
     * $$ ends where the quote ends, and the following unrelated region is
     * not consumed — same node, same literal, same sibling. */
    {
        static const char TEXT[] = "> $$\n> a\n\nafter\n";
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *root;
        const markdown_core_node *quote;
        const markdown_core_node *formula;
        const char *literal;
        markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
        if (!document) {
            return -1;
        }
        root = markdown_core_document_root(document);
        quote = root->first_child;
        formula = quote ? quote->first_child : NULL;
        literal = formula && formula->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK
                      ? markdown_core_extensions_get_formula_literal((markdown_core_node *)formula)
                      : NULL;
        if (!quote || quote->type != MARKDOWN_CORE_NODE_BLOCK_QUOTE || !literal || strcmp(literal, "a") != 0 ||
            formula->next != NULL) {
            fprintf(stderr, "island_boundary: quote-bounded formula did not stop at its container\n");
            failed = 1;
        }
        if (!quote || !quote->next || quote->next->type != MARKDOWN_CORE_NODE_PARAGRAPH || quote->next->next) {
            fprintf(stderr, "island_boundary: the region after the bounded island changed\n");
            failed = 1;
        } else {
            concat_inline_text(quote->next, &text);
            if (text.size != 5 || memcmp(text.ptr, "after", 5) != 0) {
                fprintf(stderr, "island_boundary: the following region's content changed\n");
                failed = 1;
            }
        }
        markdown_core_strbuf_free(&text);
        markdown_core_document_free(document);
    }
    /* An unclosed ::: container re-parents parsed blocks — recovery keeps
     * the children structured, unlike the raw-absorbing fences — and the
     * close-fence record exists exactly when the source spells one. */
    {
        static const char TEXT_UNCLOSED[] = ":::note\nchild para\n## child heading\n";
        static const char TEXT_CLOSED[] = ":::note\nchild para\n## child heading\n:::\nafter\n";
        static const expected_record DIR_UNCLOSED[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 3, 4}
        };
        static const expected_record DIR_CLOSED[] = {
            {MARKDOWN_CORE_CONCRETE_FENCE_OPEN, 0, 0, 3},
            {MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME, 0, 3, 4},
            {MARKDOWN_CORE_CONCRETE_FENCE_CLOSE, 3, 0, 3}
        };
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT_UNCLOSED, sizeof(TEXT_UNCLOSED) - 1, &options, NULL);
        const markdown_core_node *directive;
        if (!document) {
            return -1;
        }
        directive = nth_node_of_type(markdown_core_document_root(document), MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, 0);
        if (!directive || count_kind(directive, MARKDOWN_CORE_NODE_PARAGRAPH) != 1 ||
            count_kind(directive, MARKDOWN_CORE_NODE_HEADING) != 1) {
            fprintf(stderr, "island_boundary: unclosed directive did not re-parent parsed children\n");
            failed = 1;
        }
        failed |= expect_records("island_boundary: unclosed directive records", directive, DIR_UNCLOSED, 2);
        markdown_core_document_free(document);

        document = markdown_core_document_parse((const uint8_t *)TEXT_CLOSED, sizeof(TEXT_CLOSED) - 1, &options, NULL);
        if (!document) {
            return -1;
        }
        directive = nth_node_of_type(markdown_core_document_root(document), MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, 0);
        failed |= expect_records("island_boundary: closed directive records", directive, DIR_CLOSED, 3);
        if (!directive || !directive->next || directive->next->type != MARKDOWN_CORE_NODE_PARAGRAPH) {
            fprintf(stderr, "island_boundary: the closed directive did not release the following region\n");
            failed = 1;
        }
        markdown_core_document_free(document);
    }
    /* Commit-point eligibility, block side: a malformed opener line never
     * commits — no island, no recovery, the line is paragraph text. */
    {
        static const char TEXT[] = ":::note[unclosed\n$$ trailing\n";
        static const char EXPECTED[] = ":::note[unclosed\n$$ trailing";
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
        if (!document) {
            return -1;
        }
        if (count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) != 0 ||
            count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_FORMULA_BLOCK) != 0 ||
            count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_DIRECTIVE) != 0 ||
            count_kind(markdown_core_document_root(document), MARKDOWN_CORE_NODE_FORMULA) != 0) {
            fprintf(stderr, "island_boundary: an uncommitted opener line produced an island\n");
            failed = 1;
        }
        concat_inline_text(markdown_core_document_root(document), &text);
        if (text.size != sizeof(EXPECTED) - 1 || memcmp(text.ptr, EXPECTED, text.size) != 0) {
            fprintf(stderr, "island_boundary: the uncommitted opener lines lost bytes\n");
            failed = 1;
        }
        markdown_core_strbuf_free(&text);
        markdown_core_document_free(document);
    }
    /* Commit-point eligibility, inline side: the directive's name form is
     * the one inline commit before a closer. The committed name stands,
     * the unclosed label falls back to a literal `[`, and the label text
     * parses as ordinary inlines around it. */
    {
        static const char TEXT[] = ":name[unclosed rest *em*\n";
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *root;
        markdown_core_strbuf text = MARKDOWN_CORE_BUF_INIT(markdown_core_mem_default());
        if (!document) {
            return -1;
        }
        root = markdown_core_document_root(document);
        if (count_kind(root, MARKDOWN_CORE_NODE_DIRECTIVE) != 1 ||
            count_kind(root, MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) != 0 ||
            count_kind(root, MARKDOWN_CORE_NODE_EMPHASIS) != 1) {
            fprintf(stderr, "island_boundary: the inline name commit did not hold its boundary\n");
            failed = 1;
        }
        concat_inline_text(root, &text);
        if (text.size != 17 || memcmp(text.ptr, "[unclosed rest em", 17) != 0) {
            fprintf(stderr, "island_boundary: the fallen-back label text changed\n");
            failed = 1;
        }
        markdown_core_strbuf_free(&text);
        markdown_core_document_free(document);
    }
    /* A footnote definition's boundary is the dedent rule: the blank line
     * plus unindented text ends it, and the following region is a sibling
     * paragraph, not a child. */
    {
        static const char TEXT[] = "[^l]: def\n\nout\n";
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *root;
        const markdown_core_node *definition;
        if (!document) {
            return -1;
        }
        root = markdown_core_document_root(document);
        definition = root->first_child;
        if (!definition || definition->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION ||
            count_kind(definition, MARKDOWN_CORE_NODE_PARAGRAPH) != 1 || !definition->next ||
            definition->next->type != MARKDOWN_CORE_NODE_PARAGRAPH) {
            fprintf(stderr, "island_boundary: the footnote definition's dedent boundary moved\n");
            failed = 1;
        }
        markdown_core_document_free(document);
    }
    /* GFM-mandated truncation is the grammar's own rule, not recovery:
     * "If there are greater [cells than the header row], the excess is
     * ignored" (GFM tables, example 204). The excess cells' text gets no
     * node — the spec forbids one — but the island keeps the material
     * addressable: the row's extent spans every authored byte of the
     * line, and every pipe, the excess cells' separators included,
     * records on the row. */
    {
        static const char TEXT[] = "| a | b |\n| - | - |\n| 1 | 2 | 3 |\n";
        static const expected_record WIDE_ROW_PIPES[] = {
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 0, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 4, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 8, 1},
            {MARKDOWN_CORE_CONCRETE_TABLE_PIPE, 0, 12, 1}
        };
        markdown_core_document *document =
            markdown_core_document_parse((const uint8_t *)TEXT, sizeof(TEXT) - 1, &options, NULL);
        const markdown_core_node *row;
        if (!document) {
            return -1;
        }
        row = nth_node_of_type(markdown_core_document_root(document), MARKDOWN_CORE_NODE_TABLE_ROW, 1);
        if (!row || count_kind(row, MARKDOWN_CORE_NODE_TABLE_CELL) != 2) {
            fprintf(stderr, "island_boundary: the wide row's excess cells were not truncated per spec\n");
            failed = 1;
        }
        failed |= expect_records("island_boundary: wide-row pipes span the authored line", row, WIDE_ROW_PIPES, 4);
        if (!row || row->end_column != 13) {
            fprintf(stderr, "island_boundary: the wide row's extent no longer spans its authored bytes\n");
            failed = 1;
        }
        markdown_core_document_free(document);
    }
    return failed ? -1 : 0;
}

static const concrete_case CASES[] = {
    {"region_partition", case_region_partition},
    {"region_of_walk", case_region_of_walk},
    {"capture_shape", case_capture_shape},
    {"capture_document", case_capture_document},
    {"capture_equivalence", case_capture_equivalence},
    {"capture_oom_sweep", case_capture_oom_sweep},
    {"capture_growth_ceiling", case_capture_growth_ceiling},
    {"inline_shape", case_inline_shape},
    {"inline_smart", case_inline_smart},
    {"inline_extension_funnel", case_inline_extension_funnel},
    {"inline_seam_barrier", case_inline_seam_barrier},
    {"inline_equivalence", case_inline_equivalence},
    {"inline_growth_ceiling", case_inline_growth_ceiling},
    {"recovery_literal_fallback", case_recovery_literal_fallback},
    {"recovery_island_boundary", case_recovery_island_boundary},
};

int main(int argc, char **argv) {
    size_t i;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            puts(CASES[i].name);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            if (strcmp(CASES[i].name, argv[2]) == 0) {
                int failed = CASES[i].run();
                printf("%s %s\n", CASES[i].name, failed ? "[FAILED]" : "[PASSED]");
                return failed ? 1 : 0;
            }
        }
        fprintf(stderr, "unknown case: %s\n", argv[2]);
        return 2;
    }
    fputs("usage: concrete_runner --list | --case NAME\n", stderr);
    return 2;
}
