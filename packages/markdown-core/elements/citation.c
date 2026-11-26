#include <string.h>
#include "alloc.h"
#include "label.h"
#include "autolink_scanners.h"
#include "citation.h"
#include "services.h"
#include "inline_internal.h"
#include "block_internal.h"
#include "label.h"
#include "attention.h"
#include "tokenizer.h"
#include "compile.h"

/* The services this element asks, as `self->needs` holds them: the spans a
 * citation's brace steps over whole. */
enum { CITATION_CODE_SPANS, CITATION_HTML_TAGS };
static const char *const CITATION_NEEDS[] = {[CITATION_CODE_SPANS] = MARKDOWN_CORE_SERVICE_CODE_SPANS,
                                             [CITATION_HTML_TAGS] = MARKDOWN_CORE_SERVICE_HTML_TAGS,
                                             NULL};
/* The spaces this element looks labels up in, as `self->spaces` numbers
 * them: the specimens' (a space is its name, element.h). */
enum { CITATION_SPECIMENS };
static const markdown_core_space CITATION_SPECIMEN_SPACE = {.name = "specimen"};
static const markdown_core_space *const CITATION_SPACES[] = {[CITATION_SPECIMENS] = &CITATION_SPECIMEN_SPACE, NULL};

static bool markdown_core_inline_citation_opener(markdown_core_inline_state *inline_state, bufsize_t pos);
/* The citation element's records: its parse record and its record in this
 * run. */
static markdown_core_citation_work *citation_work(const markdown_core_element_instance *self) { return self->state; }
static markdown_core_citation_run *citation_run(const markdown_core_element_instance *self,
                                                const markdown_core_inline_state *inline_state) {
    return markdown_core_run_state(inline_state, self);
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
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    bufsize_t before = pos;
    if (!pos) {
        return true;
    }
    do {
        before--;
    } while (before && (data[before] & 0xc0) == 0x80);
    return !citation_key_width(data + before, pos - before);
}

static bool source_escaped(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                           bufsize_t at, bufsize_t begin) {
    bufsize_t escape = at;
    while (escape > begin && markdown_core_inline_char_at(inline_state, escape - 1) == '\\') {
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
    markdown_core_skip_memo html = {0};
    /* The index reads the run from where it starts: a run from a seam
     * reads none of the content before it, which its view may not hold
     * (blocks.c, THE BACK OF A PARAGRAPH), and no citation it reads starts
     * there. */
    for (bufsize_t at = inline_state->settle ? inline_state->settle->from : 0;
         at < markdown_core_inline_size(inline_state);) {
        bufsize_t opaque_end = at;
        unsigned char c = markdown_core_inline_char_at(inline_state, at);
        bool escaped = (c == '`' || c == '<') && source_escaped(self, inline_state, at, 0);
        /* A backtick run is opaque where the dialect reads code spans. */
        bufsize_t open;
        if (c == '`' && !escaped && self->needs[CITATION_CODE_SPANS].ops) {
            /* A backtick run no closer closes is text, read past whole. */
            opaque_end = markdown_core_skip(self->needs[CITATION_CODE_SPANS], inline_state, at, &index->ticks, &open);
            if (!opaque_end) {
                opaque_end = open;
                if (top >= 0) {
                    index->entries[top].cut = true;
                }
            }
        } else if (c == '<' && !escaped) {
            const bufsize_t end = markdown_core_skip(self->needs[CITATION_HTML_TAGS], inline_state, at, &html, &open);
            bufsize_t width = end ? end - at - 1 : 0;
            if (!width) {
                width = scan_autolink_uri(markdown_core_inline_window(inline_state, 0).data,
                                          markdown_core_inline_window(inline_state, 0).len, at + 1);
            }
            if (!width) {
                width = scan_autolink_email(markdown_core_inline_window(inline_state, 0).data,
                                            markdown_core_inline_window(inline_state, 0).len, at + 1);
            }
            if (width) {
                opaque_end = at + 1 + width;
            } else if (top >= 0 && open > at) {
                index->entries[top].cut = true;
            }
        }
        if (opaque_end > at) {
            for (; at < opaque_end; at++) {
                counts->work++;
                if (top >= 0) {
                    index->entries[top].content = true;
                    if (markdown_core_is_whitespace(markdown_core_inline_char_at(inline_state, at))) {
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

/* Where the bare key read from `data` stops: key characters, with single
 * internal punctuation each followed by a key character, as Pandoc has it:
 * `@Foo_bar.baz.` keeps `Foo_bar.baz` and `@Foo_bar--baz` stops at
 * `Foo_bar`. */
static bufsize_t key_end(const unsigned char *data, bufsize_t length) {
    bufsize_t pos = 0;
    while (pos < length) {
        const unsigned char c = data[pos];
        const int width = citation_key_width(data + pos, length - pos);
        if (width) {
            pos += width;
        } else if (pos && c < 128 && c && strchr(":.#$%&-+?<>~/", c) &&
                   citation_key_width(data + pos + 1, length - pos - 1)) {
            pos++;
        } else {
            break;
        }
    }
    return pos;
}

markdown_core_node *markdown_core_inline_new_cite(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITE);
}

markdown_core_node *markdown_core_inline_new_citation(markdown_core_inline_state *inline_state,
                                                      markdown_core_node *cite) {
    markdown_core_node *item = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITATION);
    if (item) {
        markdown_core_scaffold_append(cite, item);
    }
    return item;
}

/* The key of the citation `item`, its referent settled, as the field of
 * that referent's slot kind. */
static void S_key_field(markdown_core_inline_state *inline_state, markdown_core_node *item, const citation_token *key) {
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, item);
    markdown_core_field_put(&fields,
                            item->as.citation->referent == MARKDOWN_CORE_NODE_REFERENT_SPECIMEN
                                ? MARKDOWN_CORE_SLOT_SPECIMEN
                                : MARKDOWN_CORE_SLOT_BIB,
                            MARKDOWN_CORE_FIELD, key->key_start, key->key_end, false);
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
    item->as.citation->value = markdown_core_inline_take(inline_state, key->key_start, key->key_end - key->key_start);
    if (!markdown_core_chunk_to_cstr(&item->as.citation->value)) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    markdown_core_inline_state_place(inline_state, item, key->start, key->end - 1);
    return item;
}

/* Whitespace at either edge of a citation's source is separation, not
 * content. Whitespace is a space, a tab or a line ending, each one byte, so
 * the edges are trimmed byte by byte. */
static void trim_citation_source(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                                 bufsize_t *start, bufsize_t *end) {
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
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
    markdown_core_place span = {0, 0};
    if (start == end) {
        return false;
    }
    markdown_core_text_place(inline_state->owner_parser, &inline_state->owner->content_map, start, end, &span);
    const bufsize_t first_byte = (bufsize_t)span.start, end_byte = (bufsize_t)span.end;
    const markdown_core_place place = node->place;
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
        if ((bufsize_t)(place.end - place.start) != text->len) {
            /* A literal shorter than its scope (the spaces before a line's
             * end left out) maps its bytes to the whole scope, at no offset
             * of the content: it is cut where its source is, the line it
             * lies on holding its bytes one for one from its start. */
            first = trim_start ? first_byte - (bufsize_t)place.start : 0;
            length = trim_end && end_byte - (bufsize_t)place.start < text->len ? end_byte - (bufsize_t)place.start
                                                                               : text->len;
            from = start - (first_byte - (bufsize_t)place.start);
        }
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

/* What compile cuts a bibliography's body at: an item's key or a
 * separator, or the bibliography itself (the base below it). */
struct markdown_core_citation_entry {
    uint16_t kind;
    bufsize_t start, end;
    /* The Cite's last child as it was read, or NULL; for the bibliography
     * itself, the base below it. */
    markdown_core_node *after;
    size_t at;
    /* For the bibliography, what compiles in it until it closes: with its
     * Cite, the bibliography's own until then, let go with the run if a
     * failed compile never closes it. */
    markdown_core_node *held;
};

static void dispose_inline(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state) {
    markdown_core_citation_run *run = markdown_core_run_state(inline_state, self);
    markdown_core_free(run->braces.entries);
    markdown_core_skip_release(self->needs[CITATION_CODE_SPANS], &run->braces.ticks);
    run->braces = (citation_brace_index){0};
    for (size_t i = 0; i < run->entry_count; i++) {
        if (run->entries[i].held) {
            markdown_core_parser_release_node(inline_state->owner_parser, run->entries[i].held);
            markdown_core_parser_release_node(inline_state->owner_parser, run->entries[i].after);
        }
    }
    markdown_core_free(run->entries);
    run->entries = NULL;
    run->entry_count = run->entry_room = 0;
}

/* THE CITATION'S TEXT (plan symbols-cleanroom.md 23.10): a key (`@key`,
 * `-@key`, `@{key}`) and, in a label, a separator are tokens; a label's
 * end asks the citation's rules (S_end_tail, S_end_group) what its body
 * is. A key a `[` follows is that label's author: a label whose body may
 * be the author's tail waits (WAIT, its author's kind) until the list the
 * author is in is decided, and then closes again (markdown_core_label_again)
 * with the author or without. A bibliography is a group over the label
 * (and its author): its item keys and separators are walls no attention
 * pairs across, and compile cuts its body into the items' affixes. The
 * keys no bibliography takes are cites of their own (CITE), decided as
 * the reading ends. */
enum {
    CITATION_KEY,
    CITATION_WAIT,
    CITATION_SEP,
    CITATION_ITEM,
    CITATION_SPLIT,
    CITATION_AUTHOR,
    CITATION_CITE,
    CITATION_GROUP,
    CITATION_GAP,
    CITATION_KINDS
};
/* What a CITE token's `data` says: its key names a specimen, and the
 * parentheses around it are its own. */
enum { CITE_SPECIMEN = 1, CITE_PARENS = 2 };

static uint16_t S_kind(const markdown_core_element_instance *self, const markdown_core_token *token) {
    return token->kind >= self->token_base && token->kind < self->token_base + CITATION_KINDS
               ? (uint16_t)(token->kind - self->token_base)
               : CITATION_KINDS;
}

/* The key a key token's bytes spell (scan_citation_key). */
static citation_token S_key_of(const unsigned char *data, bufsize_t start, bufsize_t end) {
    citation_token key = {.start = start, .end = end, .key = true};
    bufsize_t at = start;
    if (data[at] == '-') {
        key.suppress = true;
        at++;
    }
    if (data[at + 1] == '{') {
        key.key_start = at + 2;
        key.key_end = end - 1;
    } else {
        key.key_start = at + 1;
        key.key_end = end;
    }
    return key;
}

/* THE LIST OF A LABEL (the old bracket's citations): the citation tokens
 * of its body at its own depth, from event `at` up to `to`; the next one
 * from `*at`, which moves past it, or UINT32_MAX. */
static uint32_t S_next(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, size_t *at,
                       size_t to) {
    const markdown_core_events *events = &tokenizer->events;
    while (*at < to) {
        const size_t here = *at;
        *at = markdown_core_label_step(tokenizer, here, to);
        if (events->items[here].phase == MARKDOWN_CORE_ENTER) {
            const uint16_t kind = S_kind(self, markdown_core_event_token(events, here));
            if (kind <= CITATION_AUTHOR) {
                return events->items[here].token;
            }
        }
    }
    return UINT32_MAX;
}

static size_t S_body_end(const markdown_core_tokenizer *tokenizer, const markdown_core_opener *label) {
    return label->close == UINT32_MAX ? tokenizer->events.count : label->close;
}

static bool S_is_key(const markdown_core_element_instance *self, const markdown_core_token *token) {
    return S_kind(self, token) != CITATION_SEP && S_kind(self, token) != CITATION_SPLIT;
}

/* (markdown_core_inline_citation_group_valid) */
static bool S_valid(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                    const markdown_core_opener *label, bool tail) {
    bool key = tail;
    size_t at = (size_t)label->enter + 2;
    const size_t to = S_body_end(tokenizer, label);
    for (uint32_t token; (token = S_next(self, tokenizer, &at, to)) != UINT32_MAX;) {
        if (S_is_key(self, &tokenizer->events.tokens[token])) {
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

/* The key `label` is the tail of, a `[` right after it: its token, or
 * UINT32_MAX. Only white space is between them. */
static uint32_t S_author(const markdown_core_element_instance *self, const markdown_core_tokenizer *tokenizer,
                         const markdown_core_opener *label) {
    if (label->shape->kind != MARKDOWN_CORE_KIND_LINK) {
        return UINT32_MAX;
    }
    const markdown_core_events *events = &tokenizer->events;
    for (uint32_t at = label->token; at-- > 0;) {
        const markdown_core_token *token = &events->tokens[at];
        if (S_kind(self, token) == CITATION_KEY) {
            return token->data == label->start ? at : UINT32_MAX;
        }
        for (uint32_t i = token->start; i < token->end; i++) {
            if (!markdown_core_is_whitespace(tokenizer->data[i])) {
                return UINT32_MAX;
            }
        }
        if (token->end > label->start) {
            return UINT32_MAX;
        }
    }
    return UINT32_MAX;
}

/* THE TAILS (resolve_citation_tail): the tail `key` waits as closes again,
 * with `key` its author when `ordinary`, the tails of the keys in it
 * first, each as ordinary as its place in a list that may be a
 * bibliography makes it. */
typedef struct {
    uint32_t key, label;
    size_t at, to;
    bool ordinary, partitioned, first;
} S_frame;

static S_frame S_frame_of(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, uint32_t key,
                          bool ordinary) {
    const uint32_t offset = tokenizer->events.tokens[key].data;
    const markdown_core_opener *label = markdown_core_opener_at(tokenizer, offset);
    const bool group = !ordinary && S_valid(self, tokenizer, label, false);
    return (S_frame){key, offset, (size_t)label->enter + 2, S_body_end(tokenizer, label), ordinary, ordinary || group,
                     true};
}

static void S_again(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                    const S_frame *frame) {
    markdown_core_citation_run *run = citation_run(self, tokenizer->reader);
    markdown_core_token *key = &tokenizer->events.tokens[frame->key];
    key->kind = (uint16_t)(self->token_base + CITATION_KEY);
    key->data = UINT32_MAX;
    const bool again = run->again;
    const uint32_t label = run->label, author = run->author;
    run->again = true;
    run->label = frame->label;
    run->author = frame->ordinary ? frame->key : UINT32_MAX;
    markdown_core_label_again(tokenizer, frame->label);
    run = citation_run(self, tokenizer->reader);
    run->again = again;
    run->label = label;
    run->author = author;
}

static void S_tail(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, uint32_t key,
                   bool ordinary) {
    if (S_kind(self, &tokenizer->events.tokens[key]) != CITATION_WAIT) {
        return;
    }
    S_frame local[8];
    S_frame *stack = local;
    size_t count = 0, room = 8;
    stack[count++] = S_frame_of(self, tokenizer, key, ordinary);
    while (count && !tokenizer->events.failed) {
        S_frame *frame = &stack[count - 1];
        const uint32_t child = S_next(self, tokenizer, &frame->at, frame->to);
        if (child == UINT32_MAX) {
            const S_frame done = *frame;
            count--;
            S_again(self, tokenizer, &done);
            continue;
        }
        citation_work(self)->work++;
        bool child_ordinary = true;
        if (frame->partitioned) {
            child_ordinary = !frame->first;
            frame->first = !S_is_key(self, &tokenizer->events.tokens[child]);
        }
        if (S_kind(self, &tokenizer->events.tokens[child]) == CITATION_WAIT) {
            if (count == room) {
                S_frame *wider = markdown_core_alloc(room * 2, sizeof(*wider));
                if (!wider) {
                    tokenizer->events.failed = true;
                    break;
                }
                memcpy(wider, stack, count * sizeof(*stack));
                if (stack != local) {
                    markdown_core_free(stack);
                }
                stack = wider;
                room *= 2;
            }
            stack[count++] = S_frame_of(self, tokenizer, child, child_ordinary);
        }
    }
    if (stack != local) {
        markdown_core_free(stack);
    }
}

/* The tails of the list from event `at` to `to` close again, each with its
 * author (markdown_core_inline_finish_citation_tokens). */
static void S_tails(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, size_t at,
                    size_t to) {
    for (uint32_t token; !tokenizer->events.failed && (token = S_next(self, tokenizer, &at, to)) != UINT32_MAX;) {
        S_tail(self, tokenizer, token, true);
    }
}

/* A key the reading gave up, kept with its fields on the run's node. */
static void S_give_key(markdown_core_tokenizer *tokenizer, bufsize_t start, bufsize_t at, bufsize_t field,
                       bufsize_t stop, bool braced) {
    markdown_core_field fields[2];
    size_t count = 0;
    fields[count++] = (markdown_core_field){MARKDOWN_CORE_SLOT_BIB, MARKDOWN_CORE_KIND_CITATION, field, stop};
    if (!braced && !markdown_core_opener_last(tokenizer)) {
        fields[count++] = (markdown_core_field){MARKDOWN_CORE_SLOT_SPECIMEN, MARKDOWN_CORE_KIND_CITATION, field, stop};
    }
    markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_CITE, start, at, fields, count);
}

/* The key at `start` as the reading reads it (scan_citation_key): where
 * it ends, or 0. */
static bufsize_t S_scan(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                        bufsize_t start) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    markdown_core_citation_work *counts = citation_work(self);
    const unsigned char *data = tokenizer->data;
    const bufsize_t len = (bufsize_t)tokenizer->size;
    bufsize_t pos = start;
    if (!markdown_core_inline_citation_opener(inline_state, start)) {
        return 0;
    }
    if (data[pos] == '-') {
        pos++;
    }
    if (data[pos] != '@') {
        return 0;
    }
    pos++;
    if (data[pos] == '{') {
        citation_brace_index *index = &citation_run(self, inline_state)->braces;
        if (!index->ready) {
            prepare_citation_braces(self, inline_state);
        }
        if (inline_state->error) {
            return 0;
        }
        while (index->cursor < index->count && index->entries[index->cursor].start < pos) {
            counts->work++;
            index->cursor++;
        }
        if (index->cursor == index->count || index->entries[index->cursor].start != pos ||
            !index->entries[index->cursor].end) {
            markdown_core_met_end(tokenizer);
            const bool open = index->cursor < index->count && index->entries[index->cursor].start == pos &&
                              index->entries[index->cursor].valid;
            if (open) {
                const bool empty = data[pos + 1] == '}';
                S_give_key(tokenizer, start, pos, pos + 1, empty ? pos + 1 : len, true);
            }
            return 0;
        }
        return index->entries[index->cursor].end;
    }
    const bufsize_t key_start = pos;
    pos += key_end(data + pos, len - pos);
    counts->work += (size_t)(pos - key_start) + 1;
    return pos > key_start ? pos : 0;
}

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_inline_state *inline_state = tokenizer->reader;
    markdown_core_citation_run *run = citation_run(self, inline_state);
    const unsigned char *data = tokenizer->data;
    const bufsize_t size = (bufsize_t)tokenizer->size;
    const bufsize_t at = (bufsize_t)tokenizer->position;
    const bool open = markdown_core_opener_last(tokenizer) != NULL;
    if (code == ';') {
        if (!open) {
            return markdown_core_nok(tokenizer);
        }
        markdown_core_enter(tokenizer, (uint16_t)(self->token_base + CITATION_SEP));
        markdown_core_consume_to(tokenizer, (uint32_t)at + 1);
        markdown_core_exit(tokenizer, (uint16_t)(self->token_base + CITATION_SEP));
        return markdown_core_ok(tokenizer);
    }
    if (code == '-') {
        if (at + 1 >= size) {
            markdown_core_met_end(tokenizer);
            return markdown_core_nok(tokenizer);
        }
        if (data[at + 1] != '@') {
            return markdown_core_nok(tokenizer);
        }
    } else {
        if (!markdown_core_inline_citation_opener(inline_state, at)) {
            return markdown_core_nok(tokenizer);
        }
        if (at + 1 >= size) {
            markdown_core_met_end(tokenizer);
            S_give_key(tokenizer, at, at + 1, at + 1, at + 1, false);
            return markdown_core_nok(tokenizer);
        }
        if (data[at + 1] != '{' && !citation_key_width(data + at + 1, size - at - 1)) {
            S_give_key(tokenizer, at, at + 1, at + 1, at + 1, false);
            return markdown_core_nok(tokenizer);
        }
    }
    const bufsize_t end = S_scan(self, tokenizer, at);
    if (!end) {
        return markdown_core_nok(tokenizer);
    }
    /* A `[` after the key, past white space, may be its tail. */
    const bufsize_t after = markdown_core_skip_spaces_and_line_end(data, end, size);
    citation_work(self)->work += (size_t)(after - end);
    const uint32_t tail = data[after] == '[' && data[after + 1] != '^' ? (uint32_t)after : UINT32_MAX;
    const uint32_t token = markdown_core_enter(tokenizer, (uint16_t)(self->token_base + CITATION_KEY));
    markdown_core_consume_to(tokenizer, (uint32_t)end);
    markdown_core_exit(tokenizer, (uint16_t)(self->token_base + CITATION_KEY));
    if (token != UINT32_MAX) {
        tokenizer->events.tokens[token].data = tail;
    }
    if (!open) {
        run->held = true;
    }
    return markdown_core_ok(tokenizer);
}

/* A token read outside every label, which the reading's end decides. */
static bool S_holds(const markdown_core_tokenizer *tokenizer, const void *context) {
    return citation_run(context, tokenizer->reader)->held;
}

/* A label's body is decided: the tails in it close again. */
static void S_decided(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer,
                      markdown_core_opener *label) {
    S_tails(self, tokenizer, (size_t)label->enter + 2, S_body_end(tokenizer, label));
}

/* A key outside every bibliography is a cite of its own, its key a
 * specimen's when a specimen definition declares it
 * (materialize_citation_key). */
/* Whether `token` is the text of the bytes it covers (compile.h, `verbatim`). */
static bool S_verbatim(const markdown_core_tokenizer *tokenizer, const markdown_core_token *token) {
    const markdown_core_dialect *dialect = ((const markdown_core_inline_state *)tokenizer->reader)->dialect;
    return token->kind < dialect->token_kinds && dialect->compiles[token->kind] &&
           dialect->compiles[token->kind]->verbatim;
}

static void S_cite(const markdown_core_element_instance *self, markdown_core_tokenizer *tokenizer, size_t at) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    markdown_core_events *events = &tokenizer->events;
    const uint32_t index = events->items[at].token;
    markdown_core_token *token = &events->tokens[index];
    if (!markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE, MARKDOWN_CORE_NODE_CITE)) {
        return;
    }
    const citation_token key = S_key_of(tokenizer->data, (bufsize_t)token->start, (bufsize_t)token->end);
    bool failed = false, specimen = false;
    if (!key.suppress && key.key_start == key.start + 1) {
        const markdown_core_chunk value = {(unsigned char *)tokenizer->data + key.key_start,
                                           key.key_end - key.key_start, 0};
        specimen = markdown_core_parser_find_definition(
                       inline_state->owner_parser, self->spaces[CITATION_SPECIMENS], &value,
                       markdown_core_inline_state_root_offset(inline_state, key.start), &failed) != NULL;
    }
    if (failed) {
        events->failed = true;
        return;
    }
    uint32_t data = specimen ? CITE_SPECIMEN : 0;
    /* `(@key)` of a specimen: the parentheses are the cite's. */
    if (specimen && key.start && tokenizer->data[key.start - 1] == '(' && tokenizer->data[key.end] == ')' && at > 0 &&
        at + 2 < events->count && !source_escaped(self, inline_state, key.start - 1, 0)) {
        markdown_core_token *before = markdown_core_event_token(events, at - 1);
        markdown_core_token *after = markdown_core_event_token(events, at + 2);
        if (events->items[at - 1].phase == MARKDOWN_CORE_EXIT && events->items[at + 2].phase == MARKDOWN_CORE_ENTER &&
            S_verbatim(tokenizer, before) && S_verbatim(tokenizer, after) && before->end == (uint32_t)key.start &&
            after->start == (uint32_t)key.end) {
            if (--before->end == before->start) {
                before->kind = MARKDOWN_CORE_TOKEN_NONE;
            }
            if (++after->start == after->end) {
                after->kind = MARKDOWN_CORE_TOKEN_NONE;
            }
            token->start--;
            token->end++;
            data |= CITE_PARENS;
        }
    }
    token->kind = (uint16_t)(self->token_base + CITATION_CITE);
    token->data = data;
}

/* THE READING ENDS (finish_inline): the tails of the labels still open,
 * innermost first, then of the reading's own list, close again; then every
 * key no bibliography took is a cite of its own. */
static void S_resolve(markdown_core_tokenizer *tokenizer) {
    const markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_dialect *dialect = inline_state->dialect;
    const markdown_core_element_instance *self = NULL;
    for (size_t i = 0; i < dialect->element_count && !self; i++) {
        if (dialect->instances[i].element == &MARKDOWN_CORE_ELEMENT_CITATION) {
            self = &dialect->instances[i];
        }
    }
    if (!self) {
        return;
    }
    for (markdown_core_opener *label = markdown_core_opener_last(tokenizer); label && !tokenizer->events.failed;
         label = markdown_core_opener_below(tokenizer, label)) {
        S_tails(self, tokenizer, (size_t)label->enter + 2, tokenizer->events.count);
    }
    S_tails(self, tokenizer, 0, tokenizer->events.count);
    const markdown_core_events *events = &tokenizer->events;
    for (size_t at = 0; at < events->count && !tokenizer->events.failed; at++) {
        if (events->items[at].phase == MARKDOWN_CORE_ENTER &&
            S_kind(self, markdown_core_event_token(events, at)) == CITATION_KEY) {
            S_cite(self, tokenizer, at);
        }
    }
}

/* THE CITATION'S RULES FOR A `]`, as the core's label end asks them. A
 * label whose author's list is still to be decided waits, before a group
 * may take it. */
static markdown_core_bracket_match S_end_tail(const markdown_core_element_instance *self,
                                              markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                              markdown_core_label_made *made) {
    const markdown_core_citation_run *run = citation_run(self, tokenizer->reader);
    const uint32_t offset = tokenizer->events.tokens[label->token].data;
    if (run->again && run->label == offset) {
        return BRACKET_UNMATCHED;
    }
    const uint32_t author = S_author(self, tokenizer, label);
    const unsigned char next = tokenizer->data[made->past];
    if (author == UINT32_MAX || next == '(' || next == '[' || !S_valid(self, tokenizer, label, true)) {
        return BRACKET_UNMATCHED;
    }
    markdown_core_token *key = &tokenizer->events.tokens[author];
    key->kind = (uint16_t)(self->token_base + CITATION_WAIT);
    key->data = offset;
    made->kind = MARKDOWN_CORE_TOKEN_NONE;
    made->keeps = true;
    return BRACKET_MATCHED;
}

static markdown_core_bracket_match S_end_group(const markdown_core_element_instance *self,
                                               markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                               markdown_core_label_made *made) {
    const markdown_core_citation_run *run = citation_run(self, tokenizer->reader);
    const uint32_t offset = tokenizer->events.tokens[label->token].data;
    const uint32_t author = run->again && run->label == offset ? run->author : S_author(self, tokenizer, label);
    const unsigned char next = tokenizer->data[made->past];
    const bool tail = author != UINT32_MAX && next != '(' && next != '[';
    if (tokenizer->data[label->position] == '^' || !S_valid(self, tokenizer, label, tail) ||
        !markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE, MARKDOWN_CORE_NODE_CITE)) {
        return BRACKET_UNMATCHED;
    }
    /* The first key of each item is its key; the tails of the others close
     * again as ordinary ones, and they are cites of their own. */
    bool first = true;
    size_t at = (size_t)label->enter + 2;
    for (uint32_t token; !tokenizer->events.failed &&
                         (token = S_next(self, tokenizer, &at, S_body_end(tokenizer, label))) != UINT32_MAX;) {
        if (!S_is_key(self, &tokenizer->events.tokens[token])) {
            tokenizer->events.tokens[token].kind = (uint16_t)(self->token_base + CITATION_SPLIT);
            first = true;
        } else if (first) {
            S_tail(self, tokenizer, token, false);
            tokenizer->events.tokens[token].kind = (uint16_t)(self->token_base + CITATION_ITEM);
            first = false;
        } else {
            S_tail(self, tokenizer, token, true);
        }
        label = markdown_core_opener_at(tokenizer, offset);
    }
    if (tokenizer->events.failed) {
        return BRACKET_REJECTED;
    }
    made->kind = (uint16_t)(self->token_base + CITATION_GROUP);
    made->data = label->position;
    made->from = label->start + label->shape->kept;
    made->holder = MARKDOWN_CORE_NODE_NONE;
    made->keeps = true;
    if (tail) {
        /* The author is the cite's first item, the space before the `[`
         * nobody's text, though a word body pairs across it no more. */
        markdown_core_events *events = &tokenizer->events;
        events->tokens[author].kind = (uint16_t)(self->token_base + CITATION_AUTHOR);
        for (uint32_t between = author + 1; between < label->token; between++) {
            events->tokens[between].kind = (uint16_t)(self->token_base + CITATION_GAP);
        }
        uint32_t enter = label->enter;
        while (enter > 0 &&
               !(events->items[enter].token == author && events->items[enter].phase == MARKDOWN_CORE_ENTER)) {
            enter--;
        }
        made->from = events->tokens[author].start;
        made->before = enter;
    }
    return BRACKET_MATCHED;
}

/* COMPILE. A cite of its own (materialize_citation_key). */
static void S_cite_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const bool parens = (token->data & CITE_PARENS) != 0;
    const citation_token key = S_key_of(markdown_core_inline_window(inline_state, 0).data,
                                        (bufsize_t)token->start + parens, (bufsize_t)token->end - parens);
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_state);
    markdown_core_node *item = cite ? new_bib_item(inline_state, cite, &key, false) : NULL;
    if (!item) {
        if (cite) {
            markdown_core_parser_release_node(inline_state->owner_parser, cite);
        }
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    if (token->data & CITE_SPECIMEN) {
        item->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_SPECIMEN;
        item->as.citation->mode = 0;
    }
    S_key_field(inline_state, item, &key);
    markdown_core_inline_state_place(inline_state, cite, (int)token->start, (int)token->end - 1);
    markdown_core_compile_append(compiler, cite);
}

static void S_text_of(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_compile_text(compiler, (bufsize_t)token->start, (bufsize_t)token->end);
}

static bool S_entry(markdown_core_citation_run *run, markdown_core_citation_entry entry) {
    markdown_core_citation_entry *entries =
        markdown_core_reserve(run->entries, &run->entry_room, run->entry_count + 1, sizeof(*entries));
    if (!entries) {
        return false;
    }
    run->entries = entries;
    entries[run->entry_count++] = entry;
    return true;
}

/* A bibliography opens: what compiles in it is held, as text is, until it
 * closes and its Cite takes it (the sentinel entry keeps the Cite). */
static void S_group_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_citation_run *run = citation_run(self, inline_state);
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_state);
    markdown_core_node *held =
        cite ? markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_PARAGRAPH) : NULL;
    if (!held || !markdown_core_compile_push(compiler, held)) {
        if (cite) {
            markdown_core_parser_release_node(inline_state->owner_parser, cite);
        }
        if (held) {
            markdown_core_parser_release_node(inline_state->owner_parser, held);
        }
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    if (!S_entry(run, (markdown_core_citation_entry){CITATION_GROUP, 0, 0, cite, run->base, held})) {
        markdown_core_parser_release_node(inline_state->owner_parser, cite);
        markdown_core_parser_release_node(inline_state->owner_parser, markdown_core_compile_pop(compiler));
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    run->base = run->entry_count;
    markdown_core_inline_state_place(inline_state, cite, (int)token->start, (int)token->end - 1);
}

/* An item's key, a separator or the author, where the Cite stands. */
static void S_cut(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_citation_run *run = citation_run(self, inline_state);
    const markdown_core_node *held = markdown_core_compile_top(compiler);
    if (!S_entry(run, (markdown_core_citation_entry){(uint16_t)(token->kind - self->token_base),
                                                     (bufsize_t)token->start, (bufsize_t)token->end,
                                                     held ? markdown_core_scaffold_last(held) : NULL, 0, NULL})) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
    }
}

/* What compiled from `from` up to `to` of `kids`, within the bytes from
 * `start` up to `end`, is the affix at `slot` (take_citation_affix). */
static void S_affix(const markdown_core_element_instance *self, markdown_core_inline_state *inline_state,
                    markdown_core_node **slot, markdown_core_node **kids, size_t from, size_t to, bufsize_t start,
                    bufsize_t end) {
    trim_citation_source(self, inline_state, &start, &end);
    markdown_core_citation_work *counts = citation_work(self);
    for (size_t i = from; i < to && !inline_state->error; i++) {
        markdown_core_node *node = kids[i];
        counts->work++;
        if (trim_affix_node(inline_state, node, start, end)) {
            if (!*slot) {
                *slot = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_PARAGRAPH);
                if (!*slot) {
                    return;
                }
                markdown_core_inline_state_place(inline_state, *slot, start, end - 1);
            }
            markdown_core_scaffold_unlink(node);
            markdown_core_scaffold_append(*slot, node);
        } else {
            markdown_core_scaffold_unlink(node);
            markdown_core_parser_release_node(inline_state->owner_parser, node);
        }
    }
}

/* A bibliography closes: its items are cut from what compiled in it
 * (markdown_core_inline_close_bibliography). */
static void S_group_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_citation_run *run = citation_run(self, inline_state);
    markdown_core_node *held = markdown_core_compile_pop(compiler);
    markdown_core_node *cite = run->entries[run->base - 1].after;
    markdown_core_citation_entry *e = run->entries + run->base;
    const size_t m = run->entry_count - run->base;
    size_t n = 0;
    for (const markdown_core_node *kid = markdown_core_scaffold_first(held); kid;
         kid = markdown_core_scaffold_next(kid)) {
        n++;
    }
    markdown_core_node **kids = markdown_core_alloc(n ? n : 1, sizeof(*kids));
    if (!kids) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    n = 0;
    for (markdown_core_node *kid = markdown_core_scaffold_first(held); kid; kid = markdown_core_scaffold_next(kid)) {
        kids[n++] = kid;
    }
    /* Each cut is where among the children it was read. */
    for (size_t i = 0, j = 0; i < m; i++) {
        if (e[i].after) {
            while (j < n && kids[j] != e[i].after) {
                j++;
            }
            e[i].at = j + 1;
        } else {
            e[i].at = 0;
        }
    }
    const unsigned char *data = markdown_core_inline_window(inline_state, 0).data;
    const bufsize_t close = (bufsize_t)token->end - 1;
    const bool tail = m > 0 && e[0].kind == CITATION_AUTHOR;
    size_t next = tail ? 1 : 0, content = tail ? e[0].at : 0;
    bool author_item = tail;
    const bool tail_starts_item = tail && next < m && e[next].kind == CITATION_ITEM;
    bufsize_t item_start = (bufsize_t)token->data;
    while ((author_item || next < m) && !inline_state->error) {
        const markdown_core_citation_entry *k = author_item ? &e[0] : &e[next];
        size_t sep = author_item ? next : next + 1;
        while (sep < m && e[sep].kind == CITATION_ITEM) {
            sep++;
        }
        const citation_token key = S_key_of(data, k->start, k->end);
        const bufsize_t item_end = sep < m ? e[sep].start : close;
        bufsize_t scope_start = author_item ? k->start : item_start;
        bufsize_t scope_end = author_item && tail_starts_item ? k->end : item_end;
        trim_citation_source(self, inline_state, &scope_start, &scope_end);
        markdown_core_node *item = new_bib_item(inline_state, cite, &key, !author_item);
        if (!item) {
            break;
        }
        S_key_field(inline_state, item, &key);
        markdown_core_inline_state_place(inline_state, item, scope_start, scope_end - 1);
        if (!author_item) {
            S_affix(self, inline_state, &item->as.citation->prefix, kids, content, k->at, item_start, k->start);
            content = k->at;
        }
        if (author_item && tail_starts_item) {
            author_item = false;
            continue;
        }
        S_affix(self, inline_state, &item->as.citation->suffix, kids, content, sep < m ? e[sep].at : n,
                author_item ? item_start : k->end, item_end);
        author_item = false;
        if (sep < m) {
            content = e[sep].at;
            item_start = e[sep].end;
            next = sep + 1;
        } else {
            next = m;
        }
    }
    markdown_core_free(kids);
    markdown_core_parser_release_node(inline_state->owner_parser, held);
    run->entries[run->base - 1].held = NULL;
    run->entry_count = run->base - 1;
    run->base = run->entries[run->entry_count].at;
    markdown_core_compile_append(compiler, cite);
}

static const markdown_core_text_construct CITATION_CONSTRUCTS[] = {
    {"@-;", {.start = S_start, .resolve_all = S_resolve, .holds = S_holds}}};
static const markdown_core_compile_ops CITATION_TOKENS[CITATION_KINDS] = {
    [CITATION_KEY] = {.exit = S_text_of, .plain = true},
    [CITATION_WAIT] = {.exit = S_text_of, .plain = true},
    [CITATION_SEP] = {.exit = S_text_of, .plain = true},
    [CITATION_ITEM] = {.exit = S_cut, .wall = true},
    [CITATION_SPLIT] = {.exit = S_cut, .wall = true},
    [CITATION_AUTHOR] = {.exit = S_cut},
    [CITATION_CITE] = {.exit = S_cite_made, .asked = true},
    [CITATION_GROUP] = {.enter = S_group_open, .exit = S_group_close, .asked = true},
    [CITATION_GAP] = {.boundary = true}};

static const markdown_core_close_rule CITATION_CLOSES[] = {
    {.precedence = 0, .end = S_end_tail},
    {.precedence = 5, .end = S_end_group},
};

static const markdown_core_inline_ops CITATION_INLINES = {
    .constructs = CITATION_CONSTRUCTS,
    .construct_count = sizeof(CITATION_CONSTRUCTS) / sizeof(CITATION_CONSTRUCTS[0]),
    .tokens = CITATION_TOKENS,
    .token_count = CITATION_KINDS,
    .decided = S_decided,
    .dispose = dispose_inline,
    .closes = CITATION_CLOSES,
    .close_count = sizeof(CITATION_CLOSES) / sizeof(CITATION_CLOSES[0]),
    .closer = ']',
};

/* A citation reads in a name as its prefix, its `@` key and its suffix; a
 * specimen's as its key alone; a note's as nothing. */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    const markdown_core_citation_item *item = node->as.citation;
    const bool bib = item->referent == MARKDOWN_CORE_NODE_REFERENT_BIB;
    if (!bib && item->referent != MARKDOWN_CORE_NODE_REFERENT_SPECIMEN) {
        return true;
    }
    return (!bib || !item->prefix || markdown_core_words_children(item->prefix, words)) &&
           words->text(words->context, (const unsigned char *)"@", 1) &&
           words->text(words->context, item->value.data, item->value.len) &&
           (!bib || !item->suffix || markdown_core_words_children(item->suffix, words));
}

const markdown_core_facet MARKDOWN_CORE_CITATION_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_CITATION, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_CITE, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_CITATION = {
    .facets = MARKDOWN_CORE_CITATION_FACETS,
    .needs = CITATION_NEEDS,
    .spaces = CITATION_SPACES,
    .state_size = sizeof(markdown_core_citation_work),
    .run_state_size = sizeof(markdown_core_citation_run),
    .name = "citation",
    .inlines = &CITATION_INLINES,
};

/* A citation's record (markdown_core_record_ops): its referent's bytes,
 * which view its text until it settles, and its note, prefix and suffix,
 * field roots the walk frees beside its children. */
static void free_citation(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    markdown_core_chunk_free(&node->as.citation->value);
}

static bool viewed_citation(markdown_core_node *node) {
    return markdown_core_chunk_own_viewed(&node->as.citation->value);
}

static int visit_citation(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    markdown_core_citation_item *citation = node->as.citation;
    return (!citation->note || visitor(&citation->note, context)) &&
           (!citation->prefix || visitor(&citation->prefix, context)) &&
           (!citation->suffix || visitor(&citation->suffix, context));
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_CITATION = {
    .size = sizeof(markdown_core_citation_item),
    .free = free_citation,
    .viewed = viewed_citation,
    .visit = visit_citation,
};
