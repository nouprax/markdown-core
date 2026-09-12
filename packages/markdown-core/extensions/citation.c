#include "link.h"
#include "autolink_scanners.h"
#include "code.h"
#include "html.h"
#include "citation.h"
#include "inline_internal.h"
#include "block_internal.h"

static bool markdown_core_inline_citation_opener(subject *inline_parser, bufsize_t pos);
static bool markdown_core_inline_citation_key_follows(subject *inline_parser, bufsize_t at);
static citation_tokens *markdown_core_inline_current_citation_tokens(subject *inline_parser);
static markdown_core_node *markdown_core_inline_read_citation_token(subject *inline_parser, bool key);
static bool markdown_core_inline_citation_group_valid(const citation_tokens *tokens, bool tail);
typedef struct {
    citation_token *key, *next;
    bool ordinary, partitioned, first;
} citation_resolution;
static void resolve_citation_tail(subject *inline_parser, citation_token *token, bool ordinary);
void markdown_core_inline_free_citation_tokens(subject *inline_parser, citation_tokens *tokens) {
    while (tokens->first) {
        citation_token *next = tokens->first->next;
        inline_parser->mem->free(tokens->first);
        tokens->first = next;
    }
    tokens->last = NULL;
}

static bool citation_key_char(int32_t scalar) {
    return scalar == '_' || markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar);
}

static bool markdown_core_inline_citation_opener(subject *inline_parser, bufsize_t pos) {
    if (!pos) {
        return true;
    }
    bufsize_t before = pos - 1;
    while (before && (inline_parser->input.data[before] & 0xc0) == 0x80) {
        before--;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(inline_parser->input.data + before, pos - before, &scalar);
    return !citation_key_char(scalar);
}

static bool markdown_core_inline_citation_key_follows(subject *inline_parser, bufsize_t at) {
    if (at >= inline_parser->input.len) {
        return false;
    }
    if (inline_parser->input.data[at] == '{') {
        return true;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(inline_parser->input.data + at, inline_parser->input.len - at, &scalar);
    return citation_key_char(scalar);
}

static bool source_escaped(subject *inline_parser, bufsize_t at, bufsize_t begin) {
    bufsize_t escape = at;
    while (escape > begin && inline_parser->input.data[escape - 1] == '\\') {
        escape--;
        inline_parser->owner_parser->citation_work++;
    }
    return (at - escape) % 2 != 0;
}

static void prepare_citation_braces(subject *inline_parser) {
    citation_brace_index *index = &inline_parser->citation_braces;
    index->ready = true;
    size_t capacity = 0;
    bufsize_t top = -1;
    unsigned html_flags = 0;
    for (bufsize_t at = 0; at < inline_parser->input.len;) {
        bufsize_t opaque_end = at;
        unsigned char c = inline_parser->input.data[at];
        bool escaped = (c == '`' || c == '<') && source_escaped(inline_parser, at, 0);
        if (c == '`' && !escaped) {
            bufsize_t run = at;
            while (run < inline_parser->input.len && inline_parser->input.data[run] == '`') {
                run++;
            }
            bufsize_t saved = inline_parser->pos;
            inline_parser->pos = run;
            opaque_end = markdown_core_inline_scan_to_closing_backticks(inline_parser, run - at);
            inline_parser->pos = saved;
            if (!opaque_end) {
                opaque_end = run;
            }
        } else if (c == '<' && !escaped) {
            bufsize_t width = markdown_core_inline_scan_inline_html(inline_parser, at + 1, &html_flags, NULL);
            if (!width) {
                width = scan_autolink_uri(&inline_parser->input, at + 1);
            }
            if (!width) {
                width = scan_autolink_email(&inline_parser->input, at + 1);
            }
            if (width) {
                opaque_end = at + 1 + width;
            }
        }
        if (opaque_end > at) {
            while (at < opaque_end) {
                int32_t scalar;
                int width = markdown_core_utf8proc_iterate(inline_parser->input.data + at, opaque_end - at, &scalar);
                inline_parser->owner_parser->citation_work++;
                if (top >= 0) {
                    index->entries[top].content = true;
                    if (markdown_core_utf8proc_is_space(scalar)) {
                        index->entries[top].valid = false;
                    }
                }
                at += width > 0 ? width : 1;
            }
            continue;
        }
        inline_parser->owner_parser->citation_work++;
        if (c == '{') {
            if (index->count == capacity) {
                if (capacity > SIZE_MAX / sizeof(*index->entries) / 2) {
                    inline_parser->oom = 1;
                    return;
                }
                size_t grown = capacity ? capacity * 2 : 8;
                void *entries = inline_parser->mem->realloc(index->entries, grown * sizeof(*index->entries));
                if (!entries) {
                    inline_parser->oom = 1;
                    return;
                }
                index->entries = entries;
                inline_parser->owner_parser->citation_brace_bytes += (grown - capacity) * sizeof(*index->entries);
                capacity = grown;
            }
            index->entries[index->count] = (citation_brace){.start = at++, .previous = top, .valid = true};
            top = (bufsize_t)index->count++;
        } else if (c == '}' && top >= 0) {
            citation_brace *brace = &index->entries[top];
            bool valid = brace->valid && brace->content;
            brace->end = valid ? at + 1 : 0;
            top = brace->previous;
            if (top >= 0) {
                index->entries[top].valid &= valid;
                index->entries[top].content = true;
            }
            at++;
        } else {
            int32_t scalar;
            int width =
                markdown_core_utf8proc_iterate(inline_parser->input.data + at, inline_parser->input.len - at, &scalar);
            if (top >= 0) {
                index->entries[top].content = true;
                if (markdown_core_utf8proc_is_space(scalar)) {
                    index->entries[top].valid = false;
                }
            }
            at += width > 0 ? width : 1;
        }
    }
}

static bool scan_citation_key(subject *inline_parser, bufsize_t start, citation_token *token) {
    bufsize_t pos = start;
    *token = (citation_token){.start = start, .key = true};
    if (!markdown_core_inline_citation_opener(inline_parser, start)) {
        return false;
    }
    if (markdown_core_inline_peek_at(inline_parser, pos) == '-') {
        token->suppress = true;
        pos++;
    }
    if (markdown_core_inline_peek_at(inline_parser, pos) != '@') {
        return false;
    }
    pos++;
    if (markdown_core_inline_peek_at(inline_parser, pos) == '{') {
        citation_brace_index *index = &inline_parser->citation_braces;
        if (!index->ready) {
            prepare_citation_braces(inline_parser);
        }
        if (inline_parser->oom) {
            return false;
        }
        while (index->cursor < index->count && index->entries[index->cursor].start < pos) {
            inline_parser->owner_parser->citation_work++;
            index->cursor++;
        }
        if (index->cursor == index->count || index->entries[index->cursor].start != pos ||
            !index->entries[index->cursor].end) {
            return false;
        }
        token->key_start = pos + 1;
        token->end = index->entries[index->cursor].end;
        token->key_end = token->end - 1;
        return true;
    }
    token->key_start = pos;
    while (pos < inline_parser->input.len) {
        int32_t scalar;
        int width =
            markdown_core_utf8proc_iterate(inline_parser->input.data + pos, inline_parser->input.len - pos, &scalar);
        inline_parser->owner_parser->citation_work++;
        if (citation_key_char(scalar)) {
            pos += width;
        } else if (pos > token->key_start && scalar < 128 && strchr(":.#$%&-+?<>~/", scalar) &&
                   pos + width < inline_parser->input.len) {
            int32_t next;
            markdown_core_utf8proc_iterate(inline_parser->input.data + pos + width,
                                           inline_parser->input.len - pos - width, &next);
            if (!citation_key_char(next)) {
                break;
            }
            pos += width;
        } else {
            break;
        }
    }
    token->key_end = token->end = pos;
    return pos > token->key_start;
}

static citation_tokens *markdown_core_inline_current_citation_tokens(subject *inline_parser) {
    return inline_parser->last_bracket ? &inline_parser->last_bracket->citations : &inline_parser->citations;
}

static markdown_core_node *markdown_core_inline_read_citation_token(subject *inline_parser, bool key) {
    citation_token value = {.start = inline_parser->pos, .end = inline_parser->pos + 1};
    if (key && !scan_citation_key(inline_parser, inline_parser->pos, &value)) {
        return NULL;
    }
    markdown_core_node *text =
        make_str(inline_parser, value.start, value.end - 1,
                 markdown_core_chunk_dup(&inline_parser->input, value.start, value.end - value.start));
    if (!text) {
        return NULL;
    }
    citation_token *token = inline_parser->mem->calloc(1, sizeof(*token));
    delimiter *boundary =
        token ? markdown_core_inline_push_delimiter_entry(inline_parser, DELIMITER_CITATION_TOKEN, value.end) : NULL;
    if (!boundary) {
        inline_parser->mem->free(token);
        markdown_core_node_free(text);
        inline_parser->oom = 1;
        return NULL;
    }
    *token = value;
    token->tail_start = -1;
    if (key) {
        bufsize_t at = value.end;
        unsigned lines = 0;
        while (at < inline_parser->input.len && (markdown_core_inline_peek_at(inline_parser, at) == ' ' ||
                                                 markdown_core_inline_peek_at(inline_parser, at) == '\t' ||
                                                 markdown_core_inline_peek_at(inline_parser, at) == '\n' ||
                                                 markdown_core_inline_peek_at(inline_parser, at) == '\r')) {
            inline_parser->owner_parser->citation_work++;
            if (markdown_core_inline_peek_at(inline_parser, at) == '\n') {
                lines++;
            }
            at++;
        }
        if (lines <= 1 && markdown_core_inline_peek_at(inline_parser, at) == '[' &&
            markdown_core_inline_peek_at(inline_parser, at + 1) != '^') {
            token->tail_start = at;
        }
    }
    token->node = text;
    token->boundary = boundary;
    citation_tokens *tokens = markdown_core_inline_current_citation_tokens(inline_parser);
    if (tokens->last) {
        tokens->last->next = token;
    } else {
        tokens->first = token;
    }
    tokens->last = token;
    inline_parser->pos = value.end;
    return text;
}

markdown_core_node *markdown_core_inline_new_cite(subject *inline_parser) {
    return markdown_core_inline_make_simple_subj(inline_parser, MARKDOWN_CORE_NODE_CITE);
}

markdown_core_node *markdown_core_inline_new_citation(subject *inline_parser, markdown_core_node *cite,
                                                      markdown_core_node *last) {
    markdown_core_node *item = markdown_core_inline_make_simple_subj(inline_parser, MARKDOWN_CORE_NODE_CITATION);
    if (item) {
        item->prev = last;
        if (last) {
            last->next = item;
        } else {
            cite->as.cite->citations = item;
        }
    }
    return item;
}

static markdown_core_node *new_bib_item(subject *inline_parser, markdown_core_node *cite, markdown_core_node *last,
                                        const citation_token *key, bool normal) {
    markdown_core_node *item = markdown_core_inline_new_citation(inline_parser, cite, last);
    if (!item) {
        return NULL;
    }
    item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_BIB;
    item->as.citation->mode = key->suppress ? MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR
                              : normal      ? MARKDOWN_CORE_BIB_MODE_NORMAL
                                            : MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT;
    item->as.citation->value =
        markdown_core_chunk_dup(&inline_parser->input, key->key_start, key->key_end - key->key_start);
    if (!markdown_core_chunk_to_cstr(inline_parser->mem, &item->as.citation->value)) {
        inline_parser->oom = 1;
        return NULL;
    }
    markdown_core_inline_parser_place(inline_parser, item, key->start, key->end - 1);
    return item;
}

static void citation_boundary(subject *inline_parser, citation_token *token, bool field) {
    if (token->boundary) {
        if (field) {
            token->boundary->kind = DELIMITER_AFFIX_BOUNDARY;
        } else {
            markdown_core_inline_remove_delimiter(inline_parser, token->boundary);
        }
        token->boundary = NULL;
    }
}

static void trim_citation_source(subject *inline_parser, bufsize_t *start, bufsize_t *end) {
    while (*start < *end) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(inline_parser->input.data + *start, *end - *start, &scalar);
        inline_parser->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        *start += width;
    }
    while (*end > *start) {
        bufsize_t at = *end - 1;
        while (at > *start && (inline_parser->input.data[at] & 0xc0) == 0x80) {
            at--;
        }
        int32_t scalar;
        markdown_core_utf8proc_iterate(inline_parser->input.data + at, *end - at, &scalar);
        inline_parser->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        /* An escaped ASCII space or line ending is an owned inline token,
         * not raw edge whitespace. Preserve its complete source extent. */
        if ((scalar == ' ' || scalar == '\n') && source_escaped(inline_parser, at, *start)) {
            break;
        }
        *end = at;
    }
}

static int source_compare(int line, int column, int other_line, int other_column) {
    if (line != other_line) {
        return line < other_line ? -1 : 1;
    }
    return (column > other_column) - (column < other_column);
}

static bool trim_affix_node(subject *inline_parser, markdown_core_node *node, bufsize_t start, bufsize_t end) {
    int start_line, start_column, end_line, end_column;
    if (start == end) {
        return false;
    }
    markdown_core_parser_content_place(inline_parser->owner_parser, inline_parser->owner, start, &start_line,
                                       &start_column);
    markdown_core_parser_content_end_place(inline_parser->owner_parser, inline_parser->owner, end - 1, &end_line,
                                           &end_column);
    if (source_compare(node->end_line, node->end_column, start_line, start_column) < 0 ||
        source_compare(node->start_line, node->start_column, end_line, end_column) > 0) {
        return false;
    }
    bool trim_start = source_compare(node->start_line, node->start_column, start_line, start_column) < 0;
    bool trim_end = source_compare(node->end_line, node->end_column, end_line, end_column) > 0;
    if ((trim_start || trim_end) && node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_chunk *text = node->as.literal;
        bufsize_t from = node->content_mark_offset - inline_parser->owner->content_mark_offset;
        bufsize_t first = trim_start ? start - from : 0;
        bufsize_t length = trim_end && end - from < text->len ? end - from : text->len;
        if (length <= first) {
            return false;
        }
        length -= first;
        if (text->alloc) {
            memmove(text->data, text->data + first, (size_t)length);
        } else {
            text->data += first;
        }
        text->len = length;
        markdown_core_inline_parser_place(inline_parser, node, from + first, from + first + length - 1);
    }
    return true;
}

static void take_citation_affix(subject *inline_parser, markdown_core_node **slot, markdown_core_node *first,
                                markdown_core_node *after, bufsize_t start, bufsize_t end) {
    trim_citation_source(inline_parser, &start, &end);
    while (first != after && !inline_parser->oom) {
        markdown_core_node *next = first->next;
        inline_parser->owner_parser->citation_work++;
        if (trim_affix_node(inline_parser, first, start, end)) {
            if (!*slot) {
                *slot = markdown_core_inline_make_simple_subj(inline_parser, MARKDOWN_CORE_NODE_PARAGRAPH);
                if (!*slot) {
                    return;
                }
                markdown_core_inline_parser_place(inline_parser, *slot, start, end - 1);
            }
            markdown_core_node_unlink(first);
            markdown_core_inline_append_child(*slot, first);
        } else {
            markdown_core_node_free(first);
        }
        first = next;
    }
}

static void remove_specimen_parenthesis(subject *inline_parser, markdown_core_node *text, bool first) {
    assert(text && text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal->len);
    markdown_core_chunk *literal = text->as.literal;
    if (literal->len == 1) {
        markdown_core_node_free(text);
        return;
    }
    if (first) {
        markdown_core_parser_content_place(inline_parser->owner_parser, text, 1, &text->start_line,
                                           &text->start_column);
        markdown_core_parser_adopt_content_marks(inline_parser->owner_parser, text, text, 1, literal->len - 1);
        if (literal->alloc) {
            memmove(literal->data, literal->data + 1, (size_t)literal->len - 1);
        } else {
            literal->data++;
        }
    } else {
        markdown_core_parser_content_end_place(inline_parser->owner_parser, text, literal->len - 2, &text->end_line,
                                               &text->end_column);
    }
    literal->len--;
}

static void materialize_citation_key(subject *inline_parser, citation_token *token) {
    if (!token->key || token->node->kind == MARKDOWN_CORE_NODE_CITE || inline_parser->oom) {
        return;
    }
    if (!markdown_core_node_can_contain_type(token->node->parent, MARKDOWN_CORE_NODE_CITE)) {
        return;
    }
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_parser);
    markdown_core_node *item = cite ? new_bib_item(inline_parser, cite, NULL, token, false) : NULL;
    if (!item) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        return;
    }
    bool specimen = !token->suppress && token->key_start == token->start + 1 &&
                    markdown_core_key_index_lookup(&inline_parser->owner_parser->specimen_ids,
                                                   item->as.citation->value.data, item->as.citation->value.len);
    if (specimen) {
        item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
        item->as.citation->mode = 0;
    }
    bufsize_t start = token->start, end = token->end;
    if (specimen && start && markdown_core_inline_peek_at(inline_parser, start - 1) == '(' &&
        markdown_core_inline_peek_at(inline_parser, end) == ')') {
        if (!source_escaped(inline_parser, start - 1, 0) && token->node->prev && token->node->next &&
            token->node->prev->kind == MARKDOWN_CORE_NODE_TEXT && token->node->next->kind == MARKDOWN_CORE_NODE_TEXT) {
            remove_specimen_parenthesis(inline_parser, token->node->prev, false);
            remove_specimen_parenthesis(inline_parser, token->node->next, true);
            start--;
            end++;
        }
    }
    markdown_core_inline_parser_place(inline_parser, cite, start, end - 1);
    markdown_core_node_insert_before(token->node, cite);
    markdown_core_node_free(token->node);
    token->node = cite;
}

void markdown_core_inline_finish_citation_tokens(subject *inline_parser, citation_tokens *tokens) {
    for (citation_token *token = tokens->first; token && !inline_parser->oom; token = token->next) {
        resolve_citation_tail(inline_parser, token, true);
        citation_boundary(inline_parser, token, false);
        materialize_citation_key(inline_parser, token);
    }
}

static bool markdown_core_inline_citation_group_valid(const citation_tokens *tokens, bool tail) {
    bool key = tail;
    for (citation_token *token = tokens->first; token; token = token->next) {
        if (token->key) {
            key = true;
        } else {
            if (!key) {
                return false;
            }
            key = false;
        }
    }
    return key;
}

bool markdown_core_inline_close_bibliography(markdown_core_parser *parser, subject *inline_parser, bracket *opener) {
    bool tail = opener->author && markdown_core_inline_peek_char(inline_parser) != '(' &&
                markdown_core_inline_peek_char(inline_parser) != '[';
    if (markdown_core_inline_peek_at(inline_parser, opener->position) == '^' ||
        !markdown_core_inline_citation_group_valid(&opener->citations, tail) ||
        !markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
        return false;
    }
    bool first = true;
    for (citation_token *token = opener->citations.first; token && !inline_parser->oom; token = token->next) {
        if (!token->key) {
            citation_boundary(inline_parser, token, true);
            first = true;
        } else if (first) {
            resolve_citation_tail(inline_parser, token, false);
            citation_boundary(inline_parser, token, true);
            first = false;
        } else {
            resolve_citation_tail(inline_parser, token, true);
            citation_boundary(inline_parser, token, false);
            materialize_citation_key(inline_parser, token);
        }
    }
    if (inline_parser->oom) {
        markdown_core_inline_pop_bracket(inline_parser);
        return true;
    }
    markdown_core_inline_process_delimiters(parser, inline_parser, opener->position, opener->delim_end);
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_parser);
    if (!cite) {
        markdown_core_inline_pop_bracket(inline_parser);
        return true;
    }
    markdown_core_inline_parser_place(inline_parser, cite, tail ? opener->author->start : opener->position - 1,
                                      inline_parser->pos - 1);
    markdown_core_node *content = opener->inl_text->next;
    if (tail) {
        markdown_core_node *old = opener->author->node;
        markdown_core_node_insert_before(old, cite);
        while (old != content) {
            markdown_core_node *next = old->next;
            markdown_core_node_free(old);
            old = next;
        }
        opener->author->node = cite;
    } else {
        markdown_core_inline_replace_bracket_opener(inline_parser, opener, cite);
    }
    bufsize_t item_start = opener->position;
    citation_token *token = opener->citations.first;
    markdown_core_node *last = NULL;
    bool author_item = tail;
    /* A key in the first tail section gives that section to a normal item.
     * Only a key-free section contributes a suffix to the external author. */
    bool tail_starts_item = tail && token && token->key;
    while ((author_item || token) && !inline_parser->oom) {
        citation_token *key = author_item ? opener->author : token;
        assert(key->key);
        citation_token *separator = author_item ? token : key->next;
        while (separator && separator->key) {
            separator = separator->next;
        }
        bufsize_t item_end = separator ? separator->start : inline_parser->pos - 1;
        bufsize_t scope_start = author_item ? key->start : item_start;
        bufsize_t scope_end = author_item && tail_starts_item ? key->end : item_end;
        trim_citation_source(inline_parser, &scope_start, &scope_end);
        markdown_core_node *item = new_bib_item(inline_parser, cite, last, key, !author_item);
        if (!item) {
            break;
        }
        last = item;
        markdown_core_inline_parser_place(inline_parser, item, scope_start, scope_end - 1);
        if (!author_item) {
            take_citation_affix(inline_parser, &item->as.citation->prefix, content, key->node, item_start, key->start);
            content = key->node->next;
            markdown_core_node_free(key->node);
        }
        if (author_item && tail_starts_item) {
            author_item = false;
            continue;
        }
        take_citation_affix(inline_parser, &item->as.citation->suffix, content,
                            separator ? separator->node : opener->close_text, author_item ? item_start : key->end,
                            item_end);
        author_item = false;
        if (separator) {
            content = separator->node->next;
            markdown_core_node_free(separator->node);
            item_start = separator->end;
            token = separator->next;
        } else {
            token = NULL;
        }
    }
    if (tail) {
        opener->author->end = inline_parser->pos;
    }
    markdown_core_inline_pop_bracket(inline_parser);
    return true;
}

static void resume_citation_tail(subject *inline_parser, citation_token *token, bool ordinary) {
    bracket *pending = token->tail;
    if (!pending || inline_parser->oom || inline_parser->owner_parser->oom) {
        return;
    }
    token->tail = NULL;
    bracket *saved_bracket = inline_parser->last_bracket;
    bufsize_t saved_pos = inline_parser->pos;
    bool saved_no_links = inline_parser->no_link_openers;
    markdown_core_node *close = pending->close_text;
    pending->previous = saved_bracket;
    pending->author = ordinary ? token : NULL;
    inline_parser->last_bracket = pending;
    inline_parser->pos = pending->close_position;
    inline_parser->no_link_openers = pending->pending_no_link_openers;
    markdown_core_node *literal = markdown_core_inline_handle_close_bracket(inline_parser->owner_parser, inline_parser);
    if (literal) {
        markdown_core_node_free(literal);
    } else if (!inline_parser->oom && !inline_parser->owner_parser->oom) {
        markdown_core_node_free(close);
    }
    inline_parser->pos = saved_pos;
    inline_parser->last_bracket = saved_bracket;
    inline_parser->no_link_openers |= saved_no_links;
}

static citation_resolution citation_resolution_for(citation_token *key, bool ordinary) {
    bracket *pending = key->tail;
    bool group = !ordinary && markdown_core_inline_citation_group_valid(&pending->citations, false);
    return (citation_resolution){key, pending->citations.first, ordinary, ordinary || group, true};
}

static void resolve_citation_tail(subject *inline_parser, citation_token *token, bool ordinary) {
    if (!token->tail || inline_parser->oom || inline_parser->owner_parser->oom) {
        return;
    }
    citation_resolution *stack = NULL;
    size_t count = 0, capacity = 0;
    citation_resolution next = citation_resolution_for(token, ordinary);
    for (;;) {
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 8;
            if (grown > SIZE_MAX / sizeof(*stack)) {
                inline_parser->oom = 1;
                break;
            }
            void *values = inline_parser->mem->realloc(stack, grown * sizeof(*stack));
            if (!values) {
                inline_parser->oom = 1;
                break;
            }
            stack = values;
            capacity = grown;
        }
        stack[count++] = next;
        bool descend = false;
        while (count && !inline_parser->oom && !inline_parser->owner_parser->oom) {
            citation_resolution *frame = &stack[count - 1];
            citation_token *child = frame->next;
            if (!child) {
                resume_citation_tail(inline_parser, frame->key, frame->ordinary);
                count--;
                continue;
            }
            frame->next = child->next;
            inline_parser->owner_parser->citation_work++;
            bool child_ordinary;
            if (frame->partitioned) {
                child_ordinary = !frame->first;
                frame->first = !child->key;
            } else {
                child_ordinary = true;
            }
            if (child->tail) {
                next = citation_resolution_for(child, child_ordinary);
                descend = true;
                break;
            }
        }
        if (!descend || inline_parser->oom || inline_parser->owner_parser->oom) {
            break;
        }
    }
    inline_parser->mem->free(stack);
}

bool markdown_core_citation_defer_tail(subject *inline_parser, bracket *opener, markdown_core_node **result) {
    bufsize_t initial_pos = inline_parser->pos;
    if (opener->author && !opener->close_text && markdown_core_inline_peek_char(inline_parser) != '(' &&
        markdown_core_inline_peek_char(inline_parser) != '[' &&
        markdown_core_inline_citation_group_valid(&opener->citations, true)) {
        markdown_core_node *close =
            make_str(inline_parser, initial_pos - 1, initial_pos - 1, markdown_core_chunk_literal("]"));
        delimiter *end =
            close ? markdown_core_inline_push_delimiter_entry(inline_parser, DELIMITER_CITATION_TOKEN, initial_pos)
                  : NULL;
        if (!end) {
            if (close) {
                markdown_core_node_free(close);
            }
            inline_parser->oom = 1;
            return true;
        }
        opener->close_text = close;
        opener->close_position = initial_pos - 1;
        opener->delim_end = end;
        opener->pending_no_link_openers = inline_parser->no_link_openers;
        opener->author->tail = opener;
        opener->pending_next = inline_parser->pending_brackets;
        if (inline_parser->pending_brackets) {
            inline_parser->pending_brackets->pending_previous = opener;
        }
        inline_parser->pending_brackets = opener;
        inline_parser->last_bracket = opener->previous;
        *result = close;
        return true;
    }
    return false;
}

static bool is_inline_start(markdown_core_inline_parser *inline_parser, bufsize_t at) {
    unsigned char c = inline_parser->input.data[at];
    if (c == '-') {
        return markdown_core_inline_peek_at(inline_parser, at + 1) == '@';
    }
    if (c == ';') {
        return inline_parser->last_bracket != NULL;
    }
    return markdown_core_inline_citation_opener(inline_parser, at) &&
           markdown_core_inline_citation_key_follows(inline_parser, at + 1);
}
static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    if (character == '@' || character == '-') {
        return markdown_core_inline_read_citation_token(inline_parser, true);
    }
    if (character == ';' && inline_parser->last_bracket) {
        return markdown_core_inline_read_citation_token(inline_parser, false);
    }
    return NULL;
}
static void finish_inline(subject *inline_parser) {
    for (bracket *open = inline_parser->last_bracket; open; open = open->previous) {
        markdown_core_inline_finish_citation_tokens(inline_parser, &open->citations);
    }
    markdown_core_inline_finish_citation_tokens(inline_parser, &inline_parser->citations);
}
static void dispose_inline(subject *inline_parser) {
    markdown_core_inline_free_citation_tokens(inline_parser, &inline_parser->citations);
    inline_parser->mem->free(inline_parser->citation_braces.entries);
    inline_parser->citation_braces = (citation_brace_index){0};
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_CITATION = {
    .finish_inline = finish_inline,
    .dispose_inline = dispose_inline,

    .name = "citation",
    .match_inline = match,
    .is_inline_start = is_inline_start,
    .terminates_text = "@-;",
    .dispatch = "@-;",
};

void markdown_core_citation_open_bracket(subject *inline_parser, bracket *b) {
    citation_token *author = markdown_core_inline_current_citation_tokens(inline_parser)->last;
    if (b->kind == BRACKET_LINK && author && author->key && author->tail_start == inline_parser->pos - 1) {
        b->author = author;
    }
}
