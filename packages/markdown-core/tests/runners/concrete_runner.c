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
            if (node->type != MARKDOWN_CORE_NODE_BLOCK_QUOTE || record->length != 1 || line[record->column] != '>') {
                fprintf(stderr, "%s: block-quote marker record does not spell '>'\n", source->name);
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
                (int)record->length != node->as.heading.level || (int)record->column != node->start_column - 1) {
                fprintf(stderr, "%s: ATX opener record disagrees with heading\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_ATX_CLOSER:
            if (node->type != MARKDOWN_CORE_NODE_HEADING || node->as.heading.setext || record->line != 0 ||
                !run_all(line + record->column, record->length) || line[record->column] != '#') {
                fprintf(stderr, "%s: ATX closer record does not spell a '#' run\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE:
            if (node->type != MARKDOWN_CORE_NODE_HEADING || !node->as.heading.setext || record->line == 0 ||
                !run_all(line + record->column, record->length) ||
                line[record->column] != (node->as.heading.level == 1 ? '=' : '-')) {
                fprintf(stderr, "%s: setext underline record disagrees with heading level\n", source->name);
                failed = 1;
            }
            break;
        case MARKDOWN_CORE_CONCRETE_FENCE_OPEN:
            if (node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !node->as.code.fenced || record->line != 0 ||
                record->length < 3 || !run_all(line + record->column, record->length) ||
                (unsigned char)line[record->column] != node->as.code.fence_char ||
                node->as.code.fence_length != (record->length > 255 ? 255 : (uint8_t)record->length) ||
                (int)record->column != node->start_column - 1) {
                fprintf(stderr, "%s: fence-open record disagrees with code block\n", source->name);
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
                decoded.size != (markdown_core_bufsize)node->as.code.info.len ||
                memcmp(decoded.ptr, node->as.code.info.data, decoded.size) != 0) {
                fprintf(stderr, "%s: fence-info record does not decode to as.code.info\n", source->name);
                failed = 1;
            }
            markdown_core_strbuf_free(&decoded);
            break;
        }
        case MARKDOWN_CORE_CONCRETE_FENCE_CLOSE:
            if (node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !node->as.code.fenced || !node->as.code.fence_closed ||
                record->length < node->as.code.fence_length || !run_all(line + record->column, record->length) ||
                (unsigned char)line[record->column] != node->as.code.fence_char ||
                (int)(record->line) !=
                    node->end_line - (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE ? 0 : node->start_line)) {
                fprintf(stderr, "%s: fence-close record disagrees with code block\n", source->name);
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
        default:
            fprintf(stderr, "%s: unknown record kind %u on %s\n", source->name, record->kind, type_name(node->type));
            failed = 1;
            break;
        }
    }

    /* Ownership: records appear on exactly the owners 11.1 names, in the
     * multiplicity the grammar admits. A kind with no marker bytes — every
     * inline, Paragraph, HTMLBlock, ReferenceDefinition, indented code,
     * List, Document, and (until their capture slice) the extension blocks
     * — must hold none. */
    switch (node->type) {
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
        if (count < 1) {
            fprintf(stderr, "%s: BlockQuote holds no marker records\n", source->name);
            failed = 1;
        }
        break;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        if (count != 1 || records[0].kind != MARKDOWN_CORE_CONCRETE_LIST_MARKER) {
            fprintf(stderr, "%s: ListItem holds %zu records\n", source->name, count);
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
 * inside an info string, CRLF, tabs after markers, and the constructs that
 * must stay recordless (indented code, HTML blocks, reference definitions,
 * extension blocks until their slice). */
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
    {"nul_info", "```i\0nfo\nx\n```\n\npara\0text\n", sizeof("```i\0nfo\nx\n```\n\npara\0text\n") - 1},
    {"entity_info", "```&#x26;amp\nx\n```\n\n```&#32;\ny\n```\n", 0},
    {"tabs", ">\tq\n\n-\tt\n", 0},
    {"recordless",
     "    indented code\n"
     "\n"
     "<div>\n"
     "html\n"
     "</div>\n"
     "\n"
     "[rd]: /u \"t\"\n",
     0,
     true},
    {"interrupts",
     "foo\n"
     "***\n"
     "\n"
     "bar\n"
     "---\n",
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
     * per-record scrutiny: extension blocks hold no records yet, and the
     * capture must not disturb any of the 34 kinds. */
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
    {
        markdown_core_document *document = markdown_core_document_parse(
            (const uint8_t *)SHAPE_SOURCES[1].text,
            strlen(SHAPE_SOURCES[1].text),
            &options,
            NULL
        );
        size_t expected[3] = {2, 2, 1};
        size_t depth;
        if (!document) {
            return -1;
        }
        for (depth = 0; depth < 3; depth++) {
            const markdown_core_node *quote = nth_node_of_type(document->root, MARKDOWN_CORE_NODE_BLOCK_QUOTE, depth);
            if (!quote || record_count_of(quote) != expected[depth]) {
                fprintf(
                    stderr,
                    "capture_shape: quote depth %zu captured %zu of %zu '>' markers\n",
                    depth,
                    quote ? record_count_of(quote) : 0,
                    expected[depth]
                );
                failed = 1;
            }
        }
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
        markdown_core_document_free(document);
    }

    /* Incremental: Document.concrete must reach the owner from a session's
     * committed view, across commits — not only from a one-shot parse. */
    if (!failed) {
        markdown_core_parse_options options = capture_options();
        markdown_core_session *session = markdown_core_session_open(&options, NULL);
        static const char first[] = "> quoted\n> more\n\n# head #\n";
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
            tree_record_total(markdown_core_document_concrete(view)) == 0) {
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
 * whose locality the region-relative encoding exists to survive. */
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

static const char SWEEP_TEXT[] = "# head ##\n"
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
                                 "[^n]: def\n";

static int case_capture_oom_sweep(void) {
    markdown_core_node *clean_root;
    markdown_core_parser *clean_parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_FOOTNOTES);
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
        markdown_core_parser *parser = markdown_core_parser_new_with_mem(MARKDOWN_CORE_OPT_FOOTNOTES, &sweep.mem);
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

/* --- case table --------------------------------------------------------- */

typedef struct concrete_case {
    const char *name;
    int (*run)(void);
} concrete_case;

static const concrete_case CASES[] = {
    {"region_partition", case_region_partition},
    {"region_of_walk", case_region_of_walk},
    {"capture_shape", case_capture_shape},
    {"capture_document", case_capture_document},
    {"capture_equivalence", case_capture_equivalence},
    {"capture_oom_sweep", case_capture_oom_sweep},
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
