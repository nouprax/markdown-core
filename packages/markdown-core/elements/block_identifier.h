#ifndef MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#define MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#include "inlines.h"
#include "paragraph.h"
/* THE BLOCK-IDENTIFIER SUFFIX SCANNER'S WORK: the bytes it examined, for its
 * complexity gate. The scanner is the paragraph's, which attaches every
 * identifier, so this is the paragraph element's parse record; the calls
 * below count into the one they are given. */
typedef struct {
    size_t scan;
} markdown_core_block_identifier_work;

/* `paragraph` closes inside `parent`, which it is not yet a child of: an
 * identifier at its end names it, or the list item it opens. */
void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *paragraph,
                                                     markdown_core_node *parent, markdown_core_reader *reader);
/* What an identifier line found: the block it names and its identifier. */
typedef struct {
    markdown_core_node *owner;
    markdown_core_chunk identifier;
    bufsize_t content_end;
} markdown_core_block_identifier_line_found;
/* A line that would open a paragraph in `parent` may be an identifier line
 * naming the block before it (block_identifier.c, THE IDENTIFIER LINE):
 * true when it is one the boundary after it decides, already read when
 * the input ends after it (`*bounded`); then it names its owner, false
 * when it could not. */
bool markdown_core_block_identifier_line(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_node *parent, markdown_core_reader *reader,
                                         markdown_core_block_identifier_line_found *found, bool *bounded);
bool markdown_core_block_identifier_line_attach(markdown_core_parser *parser, markdown_core_reader *reader,
                                                const markdown_core_block_identifier_line_found *found);
/* Whether a line as written may be an identifier line, which names the
 * block before it (element.h, the follow's `writes`): one that ends with
 * an identifier, whatever its prefixes. */
bool markdown_core_block_identifier_writes(const markdown_core_element_instance *self, const unsigned char *line,
                                           size_t length);
#endif
