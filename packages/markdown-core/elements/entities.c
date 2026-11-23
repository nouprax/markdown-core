#include "entities.h"
#include "houdini.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

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

    len = houdini_unescape_ent(
        &ent, markdown_core_inline_window(inline_state, 0).data + markdown_core_inline_offset(inline_state),
        markdown_core_inline_size(inline_state) - markdown_core_inline_offset(inline_state));

    if (len == 0) {
        /* A reference that runs to the end may be one once the input goes
         * on: its name and its `;` are still to come. */
        bufsize_t end = markdown_core_inline_offset(inline_state);
        while (markdown_core_inline_tracks_end(inline_state) && end < markdown_core_inline_size(inline_state) &&
               (markdown_core_isalnum((char)markdown_core_inline_char_at(inline_state, end)) ||
                markdown_core_inline_char_at(inline_state, end) == '#')) {
            end++;
        }
        if (end == markdown_core_inline_size(inline_state)) {
            markdown_core_inline_saw_end(inline_state);
        }
        markdown_core_node *literal = make_str(
            inline_state, markdown_core_inline_offset(inline_state) - 1, markdown_core_inline_offset(inline_state) - 1,
            markdown_core_inline_take(inline_state, markdown_core_inline_offset(inline_state) - 1, 1));
        /* Not an entity: the `&` IS the literal, so it is content. */
        return literal;
    }

    markdown_core_inline_seek(inline_state, markdown_core_inline_offset(inline_state) + (len));
    if (ent.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return make_str(inline_state, markdown_core_inline_offset(inline_state) - 1 - len,
                    markdown_core_inline_offset(inline_state) - 1, markdown_core_chunk_buf_detach(&ent));
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
