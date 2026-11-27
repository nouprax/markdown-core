#ifndef MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#define MARKDOWN_CORE_ELEMENT_BLOCK_IDENTIFIER_H
#include "inlines.h"
#include "paragraph.h"
#include "tokenizer.h"
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
/* What an identifier line found: the block it names, and where its
 * identifier stands on the line and how long it is. A block start's found
 * storage (WHAT THE START FOUND, block_internal.h) that asks the line
 * construct begins with it. */
typedef struct {
    markdown_core_node *owner;
    bufsize_t at, length;
} markdown_core_block_identifier_line_found;
/* THE IDENTIFIER LINE, code by code, a check of a line that would open a
 * paragraph: up to three columns of space, `#name#`, a name of letters,
 * digits and hyphens, spaces and tabs, and the line's end. Its context is
 * the paragraph's instance, whose work it counts into. */
extern const markdown_core_construct markdown_core_identifier_line;
/* The line read as one names the block before it in `parent`: true when
 * that block is one an identifier line names, which the line then writes
 * (block_identifier.c, THE IDENTIFIER LINE). */
bool markdown_core_block_identifier_owner(markdown_core_parser *parser, markdown_core_node *parent,
                                          markdown_core_reader *reader,
                                          markdown_core_block_identifier_line_found *found);
/* The identifier line, of the bytes `line`, names its owner: false when it
 * could not. */
bool markdown_core_block_identifier_line_attach(markdown_core_parser *parser, markdown_core_reader *reader,
                                                const unsigned char *line,
                                                const markdown_core_block_identifier_line_found *found);
/* Whether a line as written may be an identifier line, which names the
 * block before it (element.h, the follow's `writes`): one that ends with
 * an identifier, whatever its prefixes. */
bool markdown_core_block_identifier_writes(const markdown_core_element_instance *self, const unsigned char *line,
                                           size_t length);
#endif
