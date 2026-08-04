/* Ownership-region gates (M2.5, incremental-canonical-ast.md 11.1 and 0).
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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "ast_internal.h"
#include "concrete.h"
#include "cross_reference.h"
#include "directive.h"
#include "formula.h"
#include "strikethrough.h"
#include "table.h"

/* The complete engine node inventory: 16 block types and 18 inline types,
 * mirroring the 34 canonical kinds. The partition gate requires every one
 * observed, so this table is the fixture's completeness contract. */
typedef struct expected_type {
    markdown_core_node_type type;
    const char *name;
} expected_type;

static const expected_type EXPECTED_TYPES[] = {
    {MARKDOWN_CORE_NODE_DOCUMENT, "Document"},
    {MARKDOWN_CORE_NODE_BLOCK_QUOTE, "BlockQuote"},
    {MARKDOWN_CORE_NODE_LIST, "List"},
    {MARKDOWN_CORE_NODE_LIST_ITEM, "ListItem"},
    {MARKDOWN_CORE_NODE_CODE_BLOCK, "CodeBlock"},
    {MARKDOWN_CORE_NODE_HTML_BLOCK, "HTMLBlock"},
    {MARKDOWN_CORE_NODE_PARAGRAPH, "Paragraph"},
    {MARKDOWN_CORE_NODE_HEADING, "Heading"},
    {MARKDOWN_CORE_NODE_THEMATIC_BREAK, "ThematicBreak"},
    {MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION, "FootnoteDefinition"},
    {MARKDOWN_CORE_NODE_REFERENCE_DEFINITION, "ReferenceDefinition"},
    {MARKDOWN_CORE_NODE_TABLE, "Table"},
    {MARKDOWN_CORE_NODE_TABLE_ROW, "TableRow"},
    {MARKDOWN_CORE_NODE_TABLE_CELL, "TableCell"},
    {MARKDOWN_CORE_NODE_FORMULA_BLOCK, "FormulaBlock"},
    {MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, "DirectiveBlock"},
    {MARKDOWN_CORE_NODE_TEXT, "Text"},
    {MARKDOWN_CORE_NODE_SOFT_BREAK, "SoftBreak"},
    {MARKDOWN_CORE_NODE_LINE_BREAK, "LineBreak"},
    {MARKDOWN_CORE_NODE_CODE, "Code"},
    {MARKDOWN_CORE_NODE_HTML, "HTML"},
    {MARKDOWN_CORE_NODE_EMPHASIS, "Emphasis"},
    {MARKDOWN_CORE_NODE_STRONG, "Strong"},
    {MARKDOWN_CORE_NODE_LINK, "Link"},
    {MARKDOWN_CORE_NODE_IMAGE, "Image"},
    {MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE, "FootnoteReference"},
    {MARKDOWN_CORE_NODE_LINK_REFERENCE, "LinkReference"},
    {MARKDOWN_CORE_NODE_IMAGE_REFERENCE, "ImageReference"},
    {MARKDOWN_CORE_NODE_STRIKETHROUGH, "Strikethrough"},
    {MARKDOWN_CORE_NODE_FORMULA, "Formula"},
    {MARKDOWN_CORE_NODE_DIRECTIVE, "Directive"},
    {MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, "DirectiveLabel"},
    {MARKDOWN_CORE_NODE_CROSS_LINK, "CrossLink"},
    {MARKDOWN_CORE_NODE_EMBED, "Embed"},
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

static markdown_core_document *parse_fixture(void) {
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
        }

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

/* --- case table --------------------------------------------------------- */

typedef struct concrete_case {
    const char *name;
    int (*run)(void);
} concrete_case;

static const concrete_case CASES[] = {
    {"region_partition", case_region_partition},
    {"region_of_walk", case_region_of_walk},
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
