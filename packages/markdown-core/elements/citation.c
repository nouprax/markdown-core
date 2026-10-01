#include "alloc.h"
#include "link.h"
#include "autolink_scanners.h"
#include "code.h"
#include "html.h"
#include "citation.h"
#include "specimen.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { CITATION_LINK, CITATION_CODE, CITATION_SPECIMEN };
static const markdown_core_element *const CITATION_PEERS[] = {[CITATION_LINK] = &MARKDOWN_CORE_ELEMENT_LINK,
                                                              [CITATION_CODE] = &MARKDOWN_CORE_ELEMENT_CODE,
                                                              [CITATION_SPECIMEN] = &MARKDOWN_CORE_ELEMENT_SPECIMEN,
                                                              NULL};

static bool markdown_core_inline_citation_opener(markdown_core_inline_state *inline_state, bufsize_t pos);
static bool markdown_core_inline_citation_key_follows(markdown_core_inline_state *inline_state, bufsize_t at);
static citation_tokens *markdown_core_inline_current_citation_tokens(const markdown_core_element_instance *self,
                                                                     markdown_core_inline_state *inline_state);
static markdown_core_node *markdown_core_inline_read_citation_token(const markdown_core_element_instance *self,
                                                                    markdown_core_inline_state *inline_state, bool key);
static bool markdown_core_inline_citation_group_valid(const citation_tokens *tokens, bool tail);
typedef struct {
    citation_token *key, *next;
    bool ordinary, partitioned, first;
} citation_resolution;
static void resolve_citation_tail(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                  citation_token *token, bool ordinary);

/* The citation element's records: its parse record and its record in this
 * run. */
static markdown_core_citation_work *citation_work(const markdown_core_element_instance *self) { return self->state; }
static markdown_core_citation_run *citation_run(const markdown_core_element_instance *self,
                                                const markdown_core_inline_state *inline_state) {
    return markdown_core_run_state(inline_state, self);
}

void markdown_core_inline_free_citation_tokens(markdown_core_inline_state *inline_state, citation_tokens *tokens) {
    while (tokens->first) {
        citation_token *next = tokens->first->next;
        markdown_core_free(tokens->first);
        tokens->first = next;
    }
    tokens->last = NULL;
}

/* THE WIDTH OF A KEY CHARACTER at `str`, or 0: Pandoc's "letter, digit or
 * `_`", with letter and digit meaning the Unicode categories. A bare key has
 * no delimiter after it, so the class of the next character is the only thing
 * that ends it -- `@张三，如此说` must key `张三`, as Pandoc keys it -- and
 * reading that class off the bytes would run the key into the clause. */
static int citation_key_width(const unsigned char *str, bufsize_t len) {
    if (len > 0 && str[0] == '_') {
        return 1;
    }
    return markdown_core_utf8proc_alnum_width(str, len);
}

/* An opener stands at the start of the input or after a character that is not
 * a key character; that character begins at the last non-continuation byte
 * before `pos`. */
static bool markdown_core_inline_citation_opener(markdown_core_inline_state *inline_state, bufsize_t pos) {
    const unsigned char *data = inline_state->input.data;
    bufsize_t before = pos;
    if (!pos) {
        return true;
    }
    do {
        before--;
    } while (before && (data[before] & 0xc0) == 0x80);
    return !citation_key_width(data + before, pos - before);
}

static bool markdown_core_inline_citation_key_follows(markdown_core_inline_state *inline_state, bufsize_t at) {
    if (at >= inline_state->input.len) {
        return false;
    }
    return inline_state->input.data[at] == '{' ||
           citation_key_width(inline_state->input.data + at, inline_state->input.len - at);
}

static bool source_escaped(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                           bufsize_t at, bufsize_t begin) {
    bufsize_t escape = at;
    while (escape > begin && inline_state->input.data[escape - 1] == '\\') {
        escape--;
    }
    citation_work(self)->work += (size_t)(at - escape);
    return (at - escape) % 2 != 0;
}

static void prepare_citation_braces(const markdown_core_element_instance *self,
                                    markdown_core_inline_state *inline_state) {
    citation_brace_index *index = &citation_run(self, inline_state)->braces;
    markdown_core_citation_work *counts = citation_work(self);
    index->ready = true;
    size_t capacity = 0;
    bufsize_t top = -1;
    unsigned html_flags = 0;
    for (bufsize_t at = 0; at < inline_state->input.len;) {
        bufsize_t opaque_end = at;
        unsigned char c = inline_state->input.data[at];
        bool escaped = (c == '`' || c == '<') && source_escaped(self, inline_state, at, 0);
        /* A backtick run is opaque where the dialect reads code spans. */
        if (c == '`' && !escaped && self->peers[CITATION_CODE]) {
            bufsize_t run = at;
            while (run < inline_state->input.len && inline_state->input.data[run] == '`') {
                run++;
            }
            bufsize_t saved = inline_state->pos;
            inline_state->pos = run;
            opaque_end =
                markdown_core_inline_scan_to_closing_backticks(self->peers[CITATION_CODE], inline_state, run - at);
            inline_state->pos = saved;
            if (!opaque_end) {
                opaque_end = run;
            }
        } else if (c == '<' && !escaped) {
            bufsize_t width = markdown_core_inline_scan_inline_html(inline_state, at + 1, &html_flags, NULL);
            if (!width) {
                width = scan_autolink_uri(inline_state->input.data, inline_state->input.len, at + 1);
            }
            if (!width) {
                width = scan_autolink_email(inline_state->input.data, inline_state->input.len, at + 1);
            }
            if (width) {
                opaque_end = at + 1 + width;
            }
        }
        if (opaque_end > at) {
            for (; at < opaque_end; at++) {
                counts->work++;
                if (top >= 0) {
                    index->entries[top].content = true;
                    if (markdown_core_is_whitespace(inline_state->input.data[at])) {
                        index->entries[top].valid = false;
                    }
                }
            }
            continue;
        }
        counts->work++;
        if (c == '{') {
            if (index->count == capacity) {
                if (capacity > SIZE_MAX / sizeof(*index->entries) / 2) {
                    inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                    return;
                }
                size_t grown = capacity ? capacity * 2 : 8;
                void *entries = markdown_core_realloc(index->entries, grown * sizeof(*index->entries));
                if (!entries) {
                    inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                    return;
                }
                index->entries = entries;
                counts->brace_bytes += (grown - capacity) * sizeof(*index->entries);
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
            if (top >= 0) {
                index->entries[top].content = true;
                if (markdown_core_is_whitespace(c)) {
                    index->entries[top].valid = false;
                }
            }
            at++;
        }
    }
}

static bool scan_citation_key(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                              bufsize_t start, citation_token *token) {
    markdown_core_citation_work *counts = citation_work(self);
    bufsize_t pos = start;
    *token = (citation_token){.start = start, .key = true};
    if (!markdown_core_inline_citation_opener(inline_state, start)) {
        return false;
    }
    if (markdown_core_inline_peek_at(inline_state, pos) == '-') {
        token->suppress = true;
        pos++;
    }
    if (markdown_core_inline_peek_at(inline_state, pos) != '@') {
        return false;
    }
    pos++;
    if (markdown_core_inline_peek_at(inline_state, pos) == '{') {
        citation_brace_index *index = &citation_run(self, inline_state)->braces;
        if (!index->ready) {
            prepare_citation_braces(self, inline_state);
        }
        if (inline_state->error) {
            return false;
        }
        while (index->cursor < index->count && index->entries[index->cursor].start < pos) {
            counts->work++;
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
    /* Internal punctuation is single and must be followed by a key
     * character, as Pandoc has it: `@Foo_bar.baz.` keeps `Foo_bar.baz` and
     * `@Foo_bar--baz` stops at `Foo_bar`. */
    const unsigned char *data = inline_state->input.data;
    bufsize_t len = inline_state->input.len;
    token->key_start = pos;
    while (pos < len) {
        unsigned char c = data[pos];
        int width = citation_key_width(data + pos, len - pos);
        counts->work++;
        if (width) {
            pos += width;
        } else if (pos > token->key_start && c < 128 && c && strchr(":.#$%&-+?<>~/", c) &&
                   citation_key_width(data + pos + 1, len - pos - 1)) {
            pos++;
        } else {
            break;
        }
    }
    token->key_end = token->end = pos;
    return pos > token->key_start;
}

static citation_tokens *markdown_core_inline_current_citation_tokens(const markdown_core_element_instance *self,
                                                                     markdown_core_inline_state *inline_state) {
    bracket *open = markdown_core_open_bracket(self->peers[CITATION_LINK], inline_state);
    return open ? &open->citations : &citation_run(self, inline_state)->tokens;
}

static markdown_core_node *markdown_core_inline_read_citation_token(const markdown_core_element_instance *self,
                                                                    markdown_core_inline_state *inline_state,
                                                                    bool key) {
    citation_token value = {.start = inline_state->pos, .end = inline_state->pos + 1};
    if (key && !scan_citation_key(self, inline_state, inline_state->pos, &value)) {
        return NULL;
    }
    markdown_core_node *text =
        make_str(inline_state, value.start, value.end - 1,
                 markdown_core_chunk_dup(&inline_state->input, value.start, value.end - value.start));
    markdown_core_inline_item *item = text ? markdown_core_inline_add(inline_state, text) : NULL;
    if (!item) {
        return NULL;
    }
    citation_token *token = markdown_core_alloc(1, sizeof(*token));
    delimiter *boundary =
        token ? markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_CITATION_TOKEN, value.end) : NULL;
    if (!boundary) {
        markdown_core_free(token);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    *token = value;
    token->tail_start = -1;
    if (key) {
        bufsize_t at =
            markdown_core_skip_spaces_and_line_end(inline_state->input.data, value.end, inline_state->input.len);
        citation_work(self)->work += (size_t)(at - value.end);
        if (markdown_core_inline_peek_at(inline_state, at) == '[' &&
            markdown_core_inline_peek_at(inline_state, at + 1) != '^') {
            token->tail_start = at;
        }
    }
    token->item = item;
    token->boundary = boundary;
    citation_tokens *tokens = markdown_core_inline_current_citation_tokens(self, inline_state);
    if (tokens->last) {
        tokens->last->next = token;
    } else {
        tokens->first = token;
    }
    tokens->last = token;
    inline_state->pos = value.end;
    return NULL;
}

markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITE);
}

markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_state *inline_state,
                                                      markdown_core_node *cite) {
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_node *item = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITATION);
    if (item && !markdown_core_children_append(parser->pool, &cite->children, item)) {
        markdown_core_parser_release_node(parser, item);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    return item;
}

static markdown_core_node *new_bib_item(markdown_core_inline_state *inline_state, markdown_core_node *cite,
                                        const citation_token *key, bool normal) {
    markdown_core_node *item = markdown_core_inline_new_citation(inline_state, cite);
    if (!item) {
        return NULL;
    }
    item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_BIB;
    item->as.citation->mode = key->suppress ? MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR
                              : normal      ? MARKDOWN_CORE_BIB_MODE_NORMAL
                                            : MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT;
    item->as.citation->value =
        markdown_core_chunk_dup(&inline_state->input, key->key_start, key->key_end - key->key_start);
    if (!markdown_core_chunk_to_cstr(&item->as.citation->value)) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    markdown_core_inline_state_place(inline_state, item, key->start, key->end - 1);
    return item;
}

static void citation_boundary(markdown_core_inline_state *inline_state, citation_token *token, bool field) {
    if (token->boundary) {
        if (field) {
            token->boundary->kind = DELIMITER_AFFIX_BOUNDARY;
        } else {
            markdown_core_inline_remove_delimiter(inline_state, token->boundary);
        }
        token->boundary = NULL;
    }
}

/* Whitespace at either edge of a citation's source is separation, not
 * content. Whitespace is a space, a tab or a line ending, each one byte, so
 * the edges are trimmed byte by byte. */
static void trim_citation_source(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                 bufsize_t *start, bufsize_t *end) {
    const unsigned char *data = inline_state->input.data;
    markdown_core_citation_work *counts = citation_work(self);
    while (*start < *end && markdown_core_is_whitespace(data[*start])) {
        counts->work++;
        (*start)++;
    }
    while (*end > *start && markdown_core_is_whitespace(data[*end - 1])) {
        counts->work++;
        /* An escaped space or line ending is an owned inline token, not raw
         * edge whitespace. Preserve its complete source extent. */
        if ((data[*end - 1] == ' ' || markdown_core_is_line_end(data[*end - 1])) &&
            source_escaped(self, inline_state, *end - 1, *start)) {
            break;
        }
        (*end)--;
    }
}

static bool trim_affix_node(markdown_core_inline_state *inline_state, markdown_core_node *node, bufsize_t start,
                            bufsize_t end) {
    int line;
    bufsize_t first_byte, end_byte;
    if (start == end) {
        return false;
    }
    markdown_core_parser_content_place(inline_state->owner_parser, &inline_state->owner->content_map, start, &line,
                                       &first_byte);
    markdown_core_parser_content_end_place(inline_state->owner_parser, &inline_state->owner->content_map, end - 1,
                                           &line, &end_byte);
    const markdown_core_place place = node->where.place;
    if (place.end <= (uint32_t)first_byte || place.start >= (uint32_t)end_byte) {
        return false;
    }
    bool trim_start = place.start < (uint32_t)first_byte;
    bool trim_end = place.end > (uint32_t)end_byte;
    if ((trim_start || trim_end) && node->kind == MARKDOWN_CORE_NODE_TEXT) {
        markdown_core_chunk *text = node->as.literal;
        bufsize_t from = node->content_map.offset - inline_state->owner->content_map.offset;
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
        markdown_core_inline_state_place(inline_state, node, from + first, from + first + length - 1);
    }
    return true;
}

static void take_citation_affix(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                markdown_core_node **slot, markdown_core_inline_item *first,
                                markdown_core_inline_item *after, bufsize_t start, bufsize_t end) {
    trim_citation_source(self, inline_state, &start, &end);
    markdown_core_citation_work *counts = citation_work(self);
    markdown_core_parser *parser = inline_state->owner_parser;
    while (first != after && !inline_state->error) {
        markdown_core_inline_item *next = first->next;
        counts->work++;
        if (trim_affix_node(inline_state, first->node, start, end)) {
            if (!*slot) {
                *slot = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_PARAGRAPH);
                if (!*slot) {
                    return;
                }
                markdown_core_inline_state_place(inline_state, *slot, start, end - 1);
            }
            markdown_core_node *node = markdown_core_inline_take(inline_state, first);
            if (!markdown_core_children_append(parser->pool, &(*slot)->children, node)) {
                markdown_core_parser_release_node(parser, node);
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                return;
            }
        } else {
            markdown_core_inline_release(inline_state, first);
        }
        first = next;
    }
}

static void remove_specimen_parenthesis(markdown_core_inline_state *inline_state, markdown_core_inline_item *item,
                                        bool first) {
    markdown_core_node *text = item->node;
    assert(text && text->kind == MARKDOWN_CORE_NODE_TEXT && text->as.literal->len);
    markdown_core_chunk *literal = text->as.literal;
    if (literal->len == 1) {
        markdown_core_inline_release(inline_state, item);
        return;
    }
    if (first) {
        int line;
        bufsize_t start;
        markdown_core_parser_content_place(inline_state->owner_parser, &text->content_map, 1, &line, &start);
        text->where.place.start = (uint32_t)start;
        markdown_core_parser_adopt_content_marks(inline_state->owner_parser, &text->content_map, &text->content_map, 1,
                                                 literal->len - 1);
        if (literal->alloc) {
            memmove(literal->data, literal->data + 1, (size_t)literal->len - 1);
        } else {
            literal->data++;
        }
    } else {
        int line;
        bufsize_t end;
        markdown_core_parser_content_end_place(inline_state->owner_parser, &text->content_map, literal->len - 2, &line,
                                               &end);
        text->where.place.end = (uint32_t)end;
    }
    literal->len--;
}

static void materialize_citation_key(const markdown_core_element_instance *self,
                                     markdown_core_inline_state *inline_state, citation_token *token) {
    if (!token->key || token->item->node->kind == MARKDOWN_CORE_NODE_CITE || inline_state->error) {
        return;
    }
    if (!markdown_core_node_can_contain_type(inline_state->owner, MARKDOWN_CORE_NODE_CITE)) {
        return;
    }
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_state);
    markdown_core_node *item = cite ? new_bib_item(inline_state, cite, token, false) : NULL;
    if (!item) {
        if (cite) {
            markdown_core_parser_release_node(inline_state->owner_parser, cite);
        }
        return;
    }
    const markdown_core_element_instance *specimen_element = self->peers[CITATION_SPECIMEN];
    const markdown_core_specimen_state *specimens = specimen_element ? specimen_element->state : NULL;
    bool specimen =
        !token->suppress && token->key_start == token->start + 1 && specimens &&
        markdown_core_key_index_lookup(&specimens->ids, item->as.citation->value.data, item->as.citation->value.len);
    if (specimen) {
        item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
        item->as.citation->mode = 0;
    }
    bufsize_t start = token->start, end = token->end;
    if (specimen && start && markdown_core_inline_peek_at(inline_state, start - 1) == '(' &&
        markdown_core_inline_peek_at(inline_state, end) == ')') {
        markdown_core_inline_item *before = token->item->prev, *after = token->item->next;
        if (!source_escaped(self, inline_state, start - 1, 0) && before && after &&
            before->node->kind == MARKDOWN_CORE_NODE_TEXT && after->node->kind == MARKDOWN_CORE_NODE_TEXT) {
            remove_specimen_parenthesis(inline_state, before, false);
            remove_specimen_parenthesis(inline_state, after, true);
            start--;
            end++;
        }
    }
    markdown_core_inline_state_place(inline_state, cite, start, end - 1);
    markdown_core_inline_item *placed = markdown_core_inline_put(inline_state, token->item, cite);
    if (placed) {
        markdown_core_inline_release(inline_state, token->item);
        token->item = placed;
    }
}

void markdown_core_inline_finish_citation_tokens(const markdown_core_element_instance *self,
                                                 markdown_core_inline_state *inline_state, citation_tokens *tokens) {
    for (citation_token *token = tokens->first; token && !inline_state->error; token = token->next) {
        resolve_citation_tail(self, inline_state, token, true);
        citation_boundary(inline_state, token, false);
        materialize_citation_key(self, inline_state, token);
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

bool markdown_core_inline_close_bibliography(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                             markdown_core_inline_state *inline_state, bracket *opener) {
    bool tail = opener->author && markdown_core_inline_peek_char(inline_state) != '(' &&
                markdown_core_inline_peek_char(inline_state) != '[';
    if (markdown_core_inline_peek_at(inline_state, opener->position) == '^' ||
        !markdown_core_inline_citation_group_valid(&opener->citations, tail) ||
        !markdown_core_node_can_contain_type(inline_state->owner, MARKDOWN_CORE_NODE_CITE)) {
        return false;
    }
    bool first = true;
    for (citation_token *token = opener->citations.first; token && !inline_state->error; token = token->next) {
        if (!token->key) {
            citation_boundary(inline_state, token, true);
            first = true;
        } else if (first) {
            resolve_citation_tail(self, inline_state, token, false);
            citation_boundary(inline_state, token, true);
            first = false;
        } else {
            resolve_citation_tail(self, inline_state, token, true);
            citation_boundary(inline_state, token, false);
            materialize_citation_key(self, inline_state, token);
        }
    }
    if (inline_state->error) {
        markdown_core_inline_pop_bracket(self->peers[CITATION_LINK], inline_state);
        return true;
    }
    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_state);
    if (!cite) {
        markdown_core_inline_pop_bracket(self->peers[CITATION_LINK], inline_state);
        return true;
    }
    markdown_core_inline_state_place(inline_state, cite, tail ? opener->author->start : opener->position - 1,
                                     inline_state->pos - 1);
    markdown_core_inline_item *content = opener->inl_text->next;
    if (tail) {
        markdown_core_inline_item *old = opener->author->item;
        markdown_core_inline_item *placed = markdown_core_inline_put(inline_state, old, cite);
        if (!placed) {
            markdown_core_inline_pop_bracket(self->peers[CITATION_LINK], inline_state);
            return true;
        }
        while (old != content) {
            markdown_core_inline_item *next = old->next;
            markdown_core_inline_release(inline_state, old);
            old = next;
        }
        opener->author->item = placed;
    } else {
        markdown_core_inline_replace_bracket_opener(inline_state, opener, cite);
    }
    bufsize_t item_start = opener->position;
    citation_token *token = opener->citations.first;
    bool author_item = tail;
    /* A key in the first tail section gives that section to a normal item.
     * Only a key-free section contributes a suffix to the external author. */
    bool tail_starts_item = tail && token && token->key;
    while ((author_item || token) && !inline_state->error) {
        citation_token *key = author_item ? opener->author : token;
        assert(key->key);
        citation_token *separator = author_item ? token : key->next;
        while (separator && separator->key) {
            separator = separator->next;
        }
        bufsize_t item_end = separator ? separator->start : inline_state->pos - 1;
        bufsize_t scope_start = author_item ? key->start : item_start;
        bufsize_t scope_end = author_item && tail_starts_item ? key->end : item_end;
        trim_citation_source(self, inline_state, &scope_start, &scope_end);
        markdown_core_node *item = new_bib_item(inline_state, cite, key, !author_item);
        if (!item) {
            break;
        }
        markdown_core_inline_state_place(inline_state, item, scope_start, scope_end - 1);
        if (!author_item) {
            take_citation_affix(self, inline_state, &item->as.citation->prefix, content, key->item, item_start,
                                key->start);
            content = key->item->next;
            markdown_core_inline_release(inline_state, key->item);
        }
        if (author_item && tail_starts_item) {
            author_item = false;
            continue;
        }
        take_citation_affix(self, inline_state, &item->as.citation->suffix, content,
                            separator ? separator->item : opener->close_text, author_item ? item_start : key->end,
                            item_end);
        author_item = false;
        if (separator) {
            content = separator->item->next;
            markdown_core_inline_release(inline_state, separator->item);
            item_start = separator->end;
            token = separator->next;
        } else {
            token = NULL;
        }
    }
    if (tail) {
        opener->author->end = inline_state->pos;
    }
    markdown_core_inline_pop_bracket(self->peers[CITATION_LINK], inline_state);
    return true;
}

static void resume_citation_tail(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                 citation_token *token, bool ordinary) {
    bracket *pending = token->tail;
    if (!pending || inline_state->error || inline_state->owner_parser->error) {
        return;
    }
    token->tail = NULL;
    markdown_core_bracket_scope *brackets = markdown_core_brackets(self->peers[CITATION_LINK], inline_state);
    bracket *saved_bracket = brackets->last;
    bufsize_t saved_pos = inline_state->pos;
    bool saved_no_links = brackets->no_link_openers;
    markdown_core_inline_item *close = pending->close_text;
    pending->previous = saved_bracket;
    pending->author = ordinary ? token : NULL;
    brackets->last = pending;
    inline_state->pos = pending->close_position;
    brackets->no_link_openers = pending->pending_no_link_openers;
    markdown_core_node *literal =
        markdown_core_inline_handle_close_bracket(self->peers[CITATION_LINK], inline_state->owner_parser, inline_state);
    if (literal) {
        markdown_core_parser_release_node(inline_state->owner_parser, literal);
    } else if (!inline_state->error && !inline_state->owner_parser->error) {
        markdown_core_inline_release(inline_state, close);
    }
    inline_state->pos = saved_pos;
    brackets->last = saved_bracket;
    brackets->no_link_openers |= saved_no_links;
}

static citation_resolution citation_resolution_for(citation_token *key, bool ordinary) {
    bracket *pending = key->tail;
    bool group = !ordinary && markdown_core_inline_citation_group_valid(&pending->citations, false);
    return (citation_resolution){key, pending->citations.first, ordinary, ordinary || group, true};
}

static void resolve_citation_tail(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                  citation_token *token, bool ordinary) {
    if (!token->tail || inline_state->error || inline_state->owner_parser->error) {
        return;
    }
    markdown_core_citation_work *counts = citation_work(self);
    citation_resolution *stack = NULL;
    size_t count = 0, capacity = 0;
    citation_resolution next = citation_resolution_for(token, ordinary);
    for (;;) {
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 8;
            if (grown > SIZE_MAX / sizeof(*stack)) {
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                break;
            }
            void *values = markdown_core_realloc(stack, grown * sizeof(*stack));
            if (!values) {
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                break;
            }
            stack = values;
            capacity = grown;
        }
        stack[count++] = next;
        bool descend = false;
        while (count && !inline_state->error && !inline_state->owner_parser->error) {
            citation_resolution *frame = &stack[count - 1];
            citation_token *child = frame->next;
            if (!child) {
                resume_citation_tail(self, inline_state, frame->key, frame->ordinary);
                count--;
                continue;
            }
            frame->next = child->next;
            counts->work++;
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
        if (!descend || inline_state->error || inline_state->owner_parser->error) {
            break;
        }
    }
    markdown_core_free(stack);
}

bool markdown_core_citation_defer_tail(const markdown_core_element_instance *self,
                                       markdown_core_inline_state *inline_state, bracket *opener,
                                       markdown_core_node **result) {
    bufsize_t initial_pos = inline_state->pos;
    if (opener->author && !opener->close_text && markdown_core_inline_peek_char(inline_state) != '(' &&
        markdown_core_inline_peek_char(inline_state) != '[' &&
        markdown_core_inline_citation_group_valid(&opener->citations, true)) {
        markdown_core_node *text = make_str(inline_state, initial_pos - 1, initial_pos - 1,
                                            markdown_core_chunk_dup(&inline_state->input, initial_pos - 1, 1));
        markdown_core_inline_item *close = text ? markdown_core_inline_add(inline_state, text) : NULL;
        delimiter *end =
            close ? markdown_core_inline_push_delimiter_entry(inline_state, DELIMITER_CITATION_TOKEN, initial_pos)
                  : NULL;
        if (!end) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            return true;
        }
        markdown_core_bracket_scope *brackets = markdown_core_brackets(self->peers[CITATION_LINK], inline_state);
        opener->close_text = close;
        opener->close_position = initial_pos - 1;
        opener->delim_end = end;
        opener->pending_no_link_openers = brackets->no_link_openers;
        opener->author->tail = opener;
        opener->pending_next = brackets->pending;
        if (brackets->pending) {
            brackets->pending->pending_previous = opener;
        }
        brackets->pending = opener;
        brackets->last = opener->previous;
        *result = NULL;
        return true;
    }
    return false;
}

static bool is_inline_start(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                            bufsize_t at) {
    unsigned char c = inline_state->input.data[at];
    if (c == '-') {
        return markdown_core_inline_peek_at(inline_state, at + 1) == '@';
    }
    if (c == ';') {
        return markdown_core_open_bracket(self->peers[CITATION_LINK], inline_state) != NULL;
    }
    return markdown_core_inline_citation_opener(inline_state, at) &&
           markdown_core_inline_citation_key_follows(inline_state, at + 1);
}
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character == '@' || character == '-') {
        return markdown_core_inline_read_citation_token(self, inline_state, true);
    }
    if (character == ';' && markdown_core_open_bracket(self->peers[CITATION_LINK], inline_state)) {
        return markdown_core_inline_read_citation_token(self, inline_state, false);
    }
    return NULL;
}
void markdown_core_citation_finish_run_tokens(const markdown_core_element_instance *self,
                                              markdown_core_inline_state *inline_state) {
    markdown_core_inline_finish_citation_tokens(self, inline_state, &citation_run(self, inline_state)->tokens);
}
static void finish_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    for (bracket *open = markdown_core_open_bracket(self->peers[CITATION_LINK], inline_state); open;
         open = open->previous) {
        markdown_core_inline_finish_citation_tokens(self, inline_state, &open->citations);
    }
    markdown_core_inline_finish_citation_tokens(
        self, inline_state, &((markdown_core_citation_run *)markdown_core_run_state(inline_state, self))->tokens);
}
static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_citation_run *run = markdown_core_run_state(inline_state, self);
    markdown_core_inline_free_citation_tokens(inline_state, &run->tokens);
    markdown_core_free(run->braces.entries);
    run->braces = (citation_brace_index){0};
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_CITATION = {
    .peers = CITATION_PEERS,
    .finish_inline = finish_inline,
    .dispose_inline = dispose_inline,
    .state_size = sizeof(markdown_core_citation_work),
    .run_state_size = sizeof(markdown_core_citation_run),

    .name = "citation",
    .match_inline = match,
    .is_inline_start = is_inline_start,
    .terminates_text = "@-;",
    .dispatch = "@-;",
};

void markdown_core_citation_open_bracket(const markdown_core_element_instance *self,
                                         markdown_core_inline_state *inline_state, bracket *b) {
    citation_token *author = markdown_core_inline_current_citation_tokens(self, inline_state)->last;
    if (b->kind == BRACKET_LINK && author && author->key && author->tail_start == inline_state->pos - 1) {
        b->author = author;
    }
}
