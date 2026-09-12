#ifndef MARKDOWN_CORE_EXT_HEADING_H
#define MARKDOWN_CORE_EXT_HEADING_H
#include "inlines.h"
#include "block_internal.h"
typedef struct {
    markdown_core_key_index index;
    markdown_core_key_index resources;
} anchor_registry;
void markdown_core_block_register_heading(markdown_core_parser *parser, markdown_core_node *node);
void markdown_core_block_reserve_node_anchor(markdown_core_parser *parser, anchor_registry *registry,
                                             markdown_core_node *node);
void markdown_core_block_prepare_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings);
void markdown_core_block_dispose_headings(markdown_core_parser *parser, markdown_core_heading_collection *headings);
void markdown_core_block_finalize_heading_anchors(markdown_core_parser *parser,
                                                  markdown_core_heading_collection *headings,
                                                  anchor_registry *registry);
void markdown_core_heading_begin_inlines(markdown_core_parser *parser, markdown_core_inline_parser *subj,
                                         markdown_core_node *parent);
bool markdown_core_heading_claim_tail(markdown_core_inline_parser *subj, markdown_core_node *parent);
#endif
