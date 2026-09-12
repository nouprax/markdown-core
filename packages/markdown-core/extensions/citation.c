#include "citation.h"
#include "inline_internal.h"
#include "block_internal.h"

static bool markdown_core_inline_citation_opener(subject *subj, bufsize_t pos);
static bool markdown_core_inline_citation_key_follows(subject *subj, bufsize_t at);
static citation_tokens *markdown_core_inline_current_citation_tokens(subject *subj);
static markdown_core_node *markdown_core_inline_read_citation_token(subject *subj, bool key);
static bool markdown_core_inline_citation_group_valid(const citation_tokens *tokens, bool tail);
typedef struct {
    citation_token *key, *next;
    bool ordinary, partitioned, first;
} citation_resolution;
static void resolve_citation_tail(subject *subj, citation_token *token, bool ordinary);
void markdown_core_inline_free_citation_tokens(subject *subj, citation_tokens *tokens) {
    while (tokens->first) {
        citation_token *next = tokens->first->next;
        subj->mem->free(tokens->first);
        tokens->first = next;
    }
    tokens->last = NULL;
}

static bool citation_key_char(int32_t scalar) {
    return scalar == '_' || markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar);
}

static bool markdown_core_inline_citation_opener(subject *subj, bufsize_t pos) {
    if (!pos) {
        return true;
    }
    bufsize_t before = pos - 1;
    while (before && (subj->input.data[before] & 0xc0) == 0x80) {
        before--;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(subj->input.data + before, pos - before, &scalar);
    return !citation_key_char(scalar);
}

static bool markdown_core_inline_citation_key_follows(subject *subj, bufsize_t at) {
    if (at >= subj->input.len) {
        return false;
    }
    if (subj->input.data[at] == '{') {
        return true;
    }
    int32_t scalar;
    markdown_core_utf8proc_iterate(subj->input.data + at, subj->input.len - at, &scalar);
    return citation_key_char(scalar);
}

static bool source_escaped(subject *subj, bufsize_t at, bufsize_t begin) {
    bufsize_t escape = at;
    while (escape > begin && subj->input.data[escape - 1] == '\\') {
        escape--;
        subj->owner_parser->citation_work++;
    }
    return (at - escape) % 2 != 0;
}

static void prepare_citation_braces(subject *subj) {
    citation_brace_index *index = &subj->citation_braces;
    index->ready = true;
    size_t capacity = 0;
    bufsize_t top = -1;
    unsigned html_flags = 0;
    for (bufsize_t at = 0; at < subj->input.len;) {
        bufsize_t opaque_end = at;
        unsigned char c = subj->input.data[at];
        bool escaped = (c == '`' || c == '<') && source_escaped(subj, at, 0);
        if (c == '`' && !escaped) {
            bufsize_t run = at;
            while (run < subj->input.len && subj->input.data[run] == '`') {
                run++;
            }
            bufsize_t saved = subj->pos;
            subj->pos = run;
            opaque_end = markdown_core_inline_scan_to_closing_backticks(subj, run - at);
            subj->pos = saved;
            if (!opaque_end) {
                opaque_end = run;
            }
        } else if (c == '<' && !escaped) {
            bufsize_t width = markdown_core_inline_scan_inline_html(subj, at + 1, &html_flags, NULL);
            if (!width) {
                width = scan_autolink_uri(&subj->input, at + 1);
            }
            if (!width) {
                width = scan_autolink_email(&subj->input, at + 1);
            }
            if (width) {
                opaque_end = at + 1 + width;
            }
        }
        if (opaque_end > at) {
            while (at < opaque_end) {
                int32_t scalar;
                int width = markdown_core_utf8proc_iterate(subj->input.data + at, opaque_end - at, &scalar);
                subj->owner_parser->citation_work++;
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
        subj->owner_parser->citation_work++;
        if (c == '{') {
            if (index->count == capacity) {
                if (capacity > SIZE_MAX / sizeof(*index->entries) / 2) {
                    subj->oom = 1;
                    return;
                }
                size_t grown = capacity ? capacity * 2 : 8;
                void *entries = subj->mem->realloc(index->entries, grown * sizeof(*index->entries));
                if (!entries) {
                    subj->oom = 1;
                    return;
                }
                index->entries = entries;
                subj->owner_parser->citation_brace_bytes += (grown - capacity) * sizeof(*index->entries);
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
            int width = markdown_core_utf8proc_iterate(subj->input.data + at, subj->input.len - at, &scalar);
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

static bool scan_citation_key(subject *subj, bufsize_t start, citation_token *token) {
    bufsize_t pos = start;
    *token = (citation_token){.start = start, .key = true};
    if (!markdown_core_inline_citation_opener(subj, start)) {
        return false;
    }
    if (markdown_core_inline_peek_at(subj, pos) == '-') {
        token->suppress = true;
        pos++;
    }
    if (markdown_core_inline_peek_at(subj, pos) != '@') {
        return false;
    }
    pos++;
    if (markdown_core_inline_peek_at(subj, pos) == '{') {
        citation_brace_index *index = &subj->citation_braces;
        if (!index->ready) {
            prepare_citation_braces(subj);
        }
        if (subj->oom) {
            return false;
        }
        while (index->cursor < index->count && index->entries[index->cursor].start < pos) {
            subj->owner_parser->citation_work++;
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
    while (pos < subj->input.len) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(subj->input.data + pos, subj->input.len - pos, &scalar);
        subj->owner_parser->citation_work++;
        if (citation_key_char(scalar)) {
            pos += width;
        } else if (pos > token->key_start && scalar < 128 && strchr(":.#$%&-+?<>~/", scalar) &&
                   pos + width < subj->input.len) {
            int32_t next;
            markdown_core_utf8proc_iterate(subj->input.data + pos + width, subj->input.len - pos - width, &next);
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

static citation_tokens *markdown_core_inline_current_citation_tokens(subject *subj) {
    return subj->last_bracket ? &subj->last_bracket->citations : &subj->citations;
}

static markdown_core_node *markdown_core_inline_read_citation_token(subject *subj, bool key) {
    citation_token value = {.start = subj->pos, .end = subj->pos + 1};
    if (key && !scan_citation_key(subj, subj->pos, &value)) {
        return NULL;
    }
    markdown_core_node *text = make_str(subj, value.start, value.end - 1,
                                        markdown_core_chunk_dup(&subj->input, value.start, value.end - value.start));
    if (!text) {
        return NULL;
    }
    citation_token *token = subj->mem->calloc(1, sizeof(*token));
    delimiter *boundary =
        token ? markdown_core_inline_push_delimiter_entry(subj, DELIMITER_CITATION_TOKEN, value.end) : NULL;
    if (!boundary) {
        subj->mem->free(token);
        markdown_core_node_free(text);
        subj->oom = 1;
        return NULL;
    }
    *token = value;
    token->tail_start = -1;
    if (key) {
        bufsize_t at = value.end;
        unsigned lines = 0;
        while (at < subj->input.len &&
               (markdown_core_inline_peek_at(subj, at) == ' ' || markdown_core_inline_peek_at(subj, at) == '\t' ||
                markdown_core_inline_peek_at(subj, at) == '\n' || markdown_core_inline_peek_at(subj, at) == '\r')) {
            subj->owner_parser->citation_work++;
            if (markdown_core_inline_peek_at(subj, at) == '\n') {
                lines++;
            }
            at++;
        }
        if (lines <= 1 && markdown_core_inline_peek_at(subj, at) == '[' &&
            markdown_core_inline_peek_at(subj, at + 1) != '^') {
            token->tail_start = at;
        }
    }
    token->node = text;
    token->boundary = boundary;
    citation_tokens *tokens = markdown_core_inline_current_citation_tokens(subj);
    if (tokens->last) {
        tokens->last->next = token;
    } else {
        tokens->first = token;
    }
    tokens->last = token;
    subj->pos = value.end;
    return text;
}

markdown_core_node *markdown_core_inline_new_cite(subject *subj) {
    return markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_CITE);
}

markdown_core_node *markdown_core_inline_new_citation(subject *subj, markdown_core_node *cite,
                                                      markdown_core_node *last) {
    markdown_core_node *item = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_CITATION);
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

static markdown_core_node *new_bib_item(subject *subj, markdown_core_node *cite, markdown_core_node *last,
                                        const citation_token *key, bool normal) {
    markdown_core_node *item = markdown_core_inline_new_citation(subj, cite, last);
    if (!item) {
        return NULL;
    }
    item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_BIB;
    item->as.citation->mode = key->suppress ? MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR
                              : normal      ? MARKDOWN_CORE_BIB_MODE_NORMAL
                                            : MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT;
    item->as.citation->value = markdown_core_chunk_dup(&subj->input, key->key_start, key->key_end - key->key_start);
    if (!markdown_core_chunk_to_cstr(subj->mem, &item->as.citation->value)) {
        subj->oom = 1;
        return NULL;
    }
    markdown_core_inline_parser_place(subj, item, key->start, key->end - 1);
    return item;
}

static void citation_boundary(subject *subj, citation_token *token, bool field) {
    if (token->boundary) {
        if (field) {
            token->boundary->kind = DELIMITER_AFFIX_BOUNDARY;
        } else {
            markdown_core_inline_remove_delimiter(subj, token->boundary);
        }
        token->boundary = NULL;
    }
}

static void trim_citation_source(subject *subj, bufsize_t *start, bufsize_t *end) {
    while (*start < *end) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(subj->input.data + *start, *end - *start, &scalar);
        subj->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        *start += width;
    }
    while (*end > *start) {
        bufsize_t at = *end - 1;
        while (at > *start && (subj->input.data[at] & 0xc0) == 0x80) {
            at--;
        }
        int32_t scalar;
        markdown_core_utf8proc_iterate(subj->input.data + at, *end - at, &scalar);
        subj->owner_parser->citation_work++;
        if (!markdown_core_utf8proc_is_space(scalar)) {
            break;
        }
        /* An escaped ASCII space or line ending is an owned inline token,
         * not raw edge whitespace. Preserve its complete source extent. */
        if ((scalar == ' ' || scalar == '\n') && source_escaped(subj, at, *start)) {
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

static bool trim_affix_node(subject *subj, markdown_core_node *node, bufsize_t start, bufsize_t end) {
    int start_line, start_column, end_line, end_column;
    if (start == end) {
        return false;
    }
    markdown_core_parser_content_place(subj->owner_parser, subj->owner, start, &start_line, &start_column);
    markdown_core_parser_content_end_place(subj->owner_parser, subj->owner, end - 1, &end_line, &end_column);
    if (source_compare(node->end_line, node->end_column, start_line, start_column) < 0 ||
        source_compare(node->start_line, node->start_column, end_line, end_column) > 0) {
        return false;
    }
    bool trim_start = source_compare(node->start_line, node->start_column, start_line, start_column) < 0;
    bool trim_end = source_compare(node->end_line, node->end_column, end_line, end_column) > 0;
    if ((trim_start || trim_end) && node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_chunk *text = node->as.literal;
        bufsize_t from = node->content_mark_offset - subj->owner->content_mark_offset;
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
        markdown_core_inline_parser_place(subj, node, from + first, from + first + length - 1);
    }
    return true;
}

static void take_citation_affix(subject *subj, markdown_core_node **slot, markdown_core_node *first,
                                markdown_core_node *after, bufsize_t start, bufsize_t end) {
    trim_citation_source(subj, &start, &end);
    while (first != after && !subj->oom) {
        markdown_core_node *next = first->next;
        subj->owner_parser->citation_work++;
        if (trim_affix_node(subj, first, start, end)) {
            if (!*slot) {
                *slot = markdown_core_inline_make_simple_subj(subj, MARKDOWN_CORE_NODE_PARAGRAPH);
                if (!*slot) {
                    return;
                }
                markdown_core_inline_parser_place(subj, *slot, start, end - 1);
            }
            markdown_core_node_unlink(first);
            markdown_core_inline_append_child(*slot, first);
        } else {
            markdown_core_node_free(first);
        }
        first = next;
    }
}

static void remove_specimen_parenthesis(subject *subj, markdown_core_node *text, bool first) {
    assert(text && text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal->len);
    markdown_core_chunk *literal = text->as.literal;
    if (literal->len == 1) {
        markdown_core_node_free(text);
        return;
    }
    if (first) {
        markdown_core_parser_content_place(subj->owner_parser, text, 1, &text->start_line, &text->start_column);
        markdown_core_parser_adopt_content_marks(subj->owner_parser, text, text, 1, literal->len - 1);
        if (literal->alloc) {
            memmove(literal->data, literal->data + 1, (size_t)literal->len - 1);
        } else {
            literal->data++;
        }
    } else {
        markdown_core_parser_content_end_place(subj->owner_parser, text, literal->len - 2, &text->end_line,
                                               &text->end_column);
    }
    literal->len--;
}

static void materialize_citation_key(subject *subj, citation_token *token) {
    if (!token->key || token->node->kind == MARKDOWN_CORE_NODE_CITE || subj->oom) {
        return;
    }
    if (!markdown_core_node_can_contain_type(token->node->parent, MARKDOWN_CORE_NODE_CITE)) {
        return;
    }
    markdown_core_node *cite = markdown_core_inline_new_cite(subj);
    markdown_core_node *item = cite ? new_bib_item(subj, cite, NULL, token, false) : NULL;
    if (!item) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        return;
    }
    bool specimen = !token->suppress && token->key_start == token->start + 1 &&
                    markdown_core_key_index_lookup(&subj->owner_parser->specimen_ids, item->as.citation->value.data,
                                                   item->as.citation->value.len);
    if (specimen) {
        item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
        item->as.citation->mode = 0;
    }
    bufsize_t start = token->start, end = token->end;
    if (specimen && start && markdown_core_inline_peek_at(subj, start - 1) == '(' &&
        markdown_core_inline_peek_at(subj, end) == ')') {
        if (!source_escaped(subj, start - 1, 0) && token->node->prev && token->node->next &&
            token->node->prev->kind == MARKDOWN_CORE_NODE_TEXT && token->node->next->kind == MARKDOWN_CORE_NODE_TEXT) {
            remove_specimen_parenthesis(subj, token->node->prev, false);
            remove_specimen_parenthesis(subj, token->node->next, true);
            start--;
            end++;
        }
    }
    markdown_core_inline_parser_place(subj, cite, start, end - 1);
    markdown_core_node_insert_before(token->node, cite);
    markdown_core_node_free(token->node);
    token->node = cite;
}

void markdown_core_inline_finish_citation_tokens(subject *subj, citation_tokens *tokens) {
    for (citation_token *token = tokens->first; token && !subj->oom; token = token->next) {
        resolve_citation_tail(subj, token, true);
        citation_boundary(subj, token, false);
        materialize_citation_key(subj, token);
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

bool markdown_core_inline_close_bibliography(markdown_core_parser *parser, subject *subj, bracket *opener) {
    bool tail =
        opener->author && markdown_core_inline_peek_char(subj) != '(' && markdown_core_inline_peek_char(subj) != '[';
    if (markdown_core_inline_peek_at(subj, opener->position) == '^' ||
        !markdown_core_inline_citation_group_valid(&opener->citations, tail) ||
        !markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
        return false;
    }
    bool first = true;
    for (citation_token *token = opener->citations.first; token && !subj->oom; token = token->next) {
        if (!token->key) {
            citation_boundary(subj, token, true);
            first = true;
        } else if (first) {
            resolve_citation_tail(subj, token, false);
            citation_boundary(subj, token, true);
            first = false;
        } else {
            resolve_citation_tail(subj, token, true);
            citation_boundary(subj, token, false);
            materialize_citation_key(subj, token);
        }
    }
    if (subj->oom) {
        markdown_core_inline_pop_bracket(subj);
        return true;
    }
    markdown_core_inline_process_delimiters(parser, subj, opener->position, opener->delim_end);
    markdown_core_node *cite = markdown_core_inline_new_cite(subj);
    if (!cite) {
        markdown_core_inline_pop_bracket(subj);
        return true;
    }
    markdown_core_inline_parser_place(subj, cite, tail ? opener->author->start : opener->position - 1, subj->pos - 1);
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
        markdown_core_inline_replace_bracket_opener(subj, opener, cite);
    }
    bufsize_t item_start = opener->position;
    citation_token *token = opener->citations.first;
    markdown_core_node *last = NULL;
    bool author_item = tail;
    /* A key in the first tail section gives that section to a normal item.
     * Only a key-free section contributes a suffix to the external author. */
    bool tail_starts_item = tail && token && token->key;
    while ((author_item || token) && !subj->oom) {
        citation_token *key = author_item ? opener->author : token;
        assert(key->key);
        citation_token *separator = author_item ? token : key->next;
        while (separator && separator->key) {
            separator = separator->next;
        }
        bufsize_t item_end = separator ? separator->start : subj->pos - 1;
        bufsize_t scope_start = author_item ? key->start : item_start;
        bufsize_t scope_end = author_item && tail_starts_item ? key->end : item_end;
        trim_citation_source(subj, &scope_start, &scope_end);
        markdown_core_node *item = new_bib_item(subj, cite, last, key, !author_item);
        if (!item) {
            break;
        }
        last = item;
        markdown_core_inline_parser_place(subj, item, scope_start, scope_end - 1);
        if (!author_item) {
            take_citation_affix(subj, &item->as.citation->prefix, content, key->node, item_start, key->start);
            content = key->node->next;
            markdown_core_node_free(key->node);
        }
        if (author_item && tail_starts_item) {
            author_item = false;
            continue;
        }
        take_citation_affix(subj, &item->as.citation->suffix, content, separator ? separator->node : opener->close_text,
                            author_item ? item_start : key->end, item_end);
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
        opener->author->end = subj->pos;
    }
    markdown_core_inline_pop_bracket(subj);
    return true;
}

static void resume_citation_tail(subject *subj, citation_token *token, bool ordinary) {
    bracket *pending = token->tail;
    if (!pending || subj->oom || subj->owner_parser->oom) {
        return;
    }
    token->tail = NULL;
    bracket *saved_bracket = subj->last_bracket;
    bufsize_t saved_pos = subj->pos;
    bool saved_no_links = subj->no_link_openers;
    markdown_core_node *close = pending->close_text;
    pending->previous = saved_bracket;
    pending->author = ordinary ? token : NULL;
    subj->last_bracket = pending;
    subj->pos = pending->close_position;
    subj->no_link_openers = pending->pending_no_link_openers;
    markdown_core_node *literal = markdown_core_inline_handle_close_bracket(subj->owner_parser, subj);
    if (literal) {
        markdown_core_node_free(literal);
    } else if (!subj->oom && !subj->owner_parser->oom) {
        markdown_core_node_free(close);
    }
    subj->pos = saved_pos;
    subj->last_bracket = saved_bracket;
    subj->no_link_openers |= saved_no_links;
}

static citation_resolution citation_resolution_for(citation_token *key, bool ordinary) {
    bracket *pending = key->tail;
    bool group = !ordinary && markdown_core_inline_citation_group_valid(&pending->citations, false);
    return (citation_resolution){key, pending->citations.first, ordinary, ordinary || group, true};
}

static void resolve_citation_tail(subject *subj, citation_token *token, bool ordinary) {
    if (!token->tail || subj->oom || subj->owner_parser->oom) {
        return;
    }
    citation_resolution *stack = NULL;
    size_t count = 0, capacity = 0;
    citation_resolution next = citation_resolution_for(token, ordinary);
    for (;;) {
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 8;
            if (grown > SIZE_MAX / sizeof(*stack)) {
                subj->oom = 1;
                break;
            }
            void *values = subj->mem->realloc(stack, grown * sizeof(*stack));
            if (!values) {
                subj->oom = 1;
                break;
            }
            stack = values;
            capacity = grown;
        }
        stack[count++] = next;
        bool descend = false;
        while (count && !subj->oom && !subj->owner_parser->oom) {
            citation_resolution *frame = &stack[count - 1];
            citation_token *child = frame->next;
            if (!child) {
                resume_citation_tail(subj, frame->key, frame->ordinary);
                count--;
                continue;
            }
            frame->next = child->next;
            subj->owner_parser->citation_work++;
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
        if (!descend || subj->oom || subj->owner_parser->oom) {
            break;
        }
    }
    subj->mem->free(stack);
}

bool markdown_core_citation_defer_tail(subject *subj, bracket *opener, markdown_core_node **result) {
    bufsize_t initial_pos = subj->pos;
    if (opener->author && !opener->close_text && markdown_core_inline_peek_char(subj) != '(' &&
        markdown_core_inline_peek_char(subj) != '[' &&
        markdown_core_inline_citation_group_valid(&opener->citations, true)) {
        markdown_core_node *close = make_str(subj, initial_pos - 1, initial_pos - 1, markdown_core_chunk_literal("]"));
        delimiter *end =
            close ? markdown_core_inline_push_delimiter_entry(subj, DELIMITER_CITATION_TOKEN, initial_pos) : NULL;
        if (!end) {
            if (close) {
                markdown_core_node_free(close);
            }
            subj->oom = 1;
            return true;
        }
        opener->close_text = close;
        opener->close_position = initial_pos - 1;
        opener->delim_end = end;
        opener->pending_no_link_openers = subj->no_link_openers;
        opener->author->tail = opener;
        opener->pending_next = subj->pending_brackets;
        if (subj->pending_brackets) {
            subj->pending_brackets->pending_previous = opener;
        }
        subj->pending_brackets = opener;
        subj->last_bracket = opener->previous;
        *result = close;
        return true;
    }
    return false;
}

static bool is_inline_start(markdown_core_inline_parser *subj, bufsize_t at) {
    unsigned char c = subj->input.data[at];
    if (c == '-') {
        return markdown_core_inline_peek_at(subj, at + 1) == '@';
    }
    if (c == ';') {
        return subj->last_bracket != NULL;
    }
    return markdown_core_inline_citation_opener(subj, at) && markdown_core_inline_citation_key_follows(subj, at + 1);
}
static markdown_core_node *match(const markdown_core_extension *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *subj) {
    if (character == '@' || character == '-') {
        return markdown_core_inline_read_citation_token(subj, true);
    }
    if (character == ';' && subj->last_bracket) {
        return markdown_core_inline_read_citation_token(subj, false);
    }
    return NULL;
}
const markdown_core_extension MARKDOWN_CORE_EXTENSION_CITATION = {
    .name = "citation",
    .match_inline = match,
    .is_inline_start = is_inline_start,
    .terminates_text = "@-;",
    .dispatch = "@-;",
};

void markdown_core_citation_open_bracket(subject *subj, bracket *b) {
    citation_token *author = markdown_core_inline_current_citation_tokens(subj)->last;
    if (b->kind == BRACKET_LINK && author && author->key && author->tail_start == subj->pos - 1) {
        b->author = author;
    }
}
