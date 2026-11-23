#include "entities.h"
#include "houdini.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

#define advance(inline_state) ((inline_state)->pos += 1)

bufsize_t markdown_core_entities_read(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                      const uint8_t *src, bufsize_t size) {
    return entities ? houdini_unescape_ent(out, src, size) : 0;
}

int markdown_core_entities_decode(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                  const uint8_t *src, bufsize_t size) {
    return entities ? houdini_unescape_html(out, src, size) : 0;
}

void markdown_core_entities_copy(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                 const uint8_t *src, bufsize_t size) {
    if (!markdown_core_entities_decode(entities, out, src, size)) {
        markdown_core_strbuf_put(out, src, size);
    }
}

static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    (void)self;
    (void)parser;
    (void)parent;
    if (character != '&') {
        return NULL;
    }
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT();
    bufsize_t len;

    advance(inline_state);

    len = houdini_unescape_ent(&ent, inline_state->input.data + inline_state->pos,
                               inline_state->input.len - inline_state->pos);

    if (len == 0) {
        /* A reference that runs to the end may be one once the input goes
         * on: its name and its `;` are still to come. */
        bufsize_t end = inline_state->pos;
        while (markdown_core_inline_tracks_end(inline_state) && end < inline_state->input.len &&
               (markdown_core_isalnum((char)inline_state->input.data[end]) || inline_state->input.data[end] == '#')) {
            end++;
        }
        if (end == inline_state->input.len) {
            markdown_core_inline_saw_end(inline_state);
        }
        markdown_core_node *literal = make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                                               markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1));
        /* Not an entity: the `&` IS the literal, so it is content. */
        return literal;
    }

    inline_state->pos += len;
    if (ent.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return make_str(inline_state, inline_state->pos - 1 - len, inline_state->pos - 1,
                    markdown_core_chunk_buf_detach(&ent));
}

static const markdown_core_inline_ops ENTITIES_INLINES = {
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
    .match = match,
    .terminates = "&",
    .dispatch = "&",
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ENTITIES = {
    .name = "entities",
    .inlines = &ENTITIES_INLINES,
};
