#include "entities.h"
#include <string.h>
#include "services.h"
#include "houdini.h"
#include "inline_internal.h"
#include "markdown_core_ctype.h"

#define advance(inline_state) markdown_core_inline_seek((inline_state), markdown_core_inline_offset(inline_state) + 1)

static bufsize_t S_read(const markdown_core_element_instance *self, markdown_core_strbuf *out, const uint8_t *data,
                        bufsize_t size) {
    (void)self;
    return houdini_unescape_ent(out, data, size);
}

static bool S_decode(const markdown_core_element_instance *self, markdown_core_strbuf *out, const uint8_t *data,
                     bufsize_t size) {
    (void)self;
    return houdini_unescape_html(out, data, size) != 0;
}

/* THE CHARACTER REFERENCE (plan symbols-cleanroom.md 23): `&`, a name or
 * a number, `;`, one token, decoded as compile makes its Text. An `&` no
 * reference follows is data. */
enum { ENTITIES_REFERENCE };
static markdown_core_state S_reference(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    const uint32_t from = tokenizer->position + 1;
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT();
    const bufsize_t len = houdini_unescape_ent(&ent, tokenizer->data + from, (bufsize_t)(tokenizer->size - from));
    const bool oom = ent.oom;
    markdown_core_strbuf_free(&ent);
    if (oom) {
        /* The reading ends: what the reference is was not had. */
        tokenizer->events.failed = true;
        return markdown_core_nok(tokenizer);
    }
    if (len == 0) {
        /* A reference that runs to the end may be one once the input goes
         * on: its name and its `;` are still to come. */
        uint32_t end = from;
        while (end < tokenizer->size &&
               (markdown_core_isalnum((char)tokenizer->data[end]) || tokenizer->data[end] == '#')) {
            end++;
        }
        if (end == tokenizer->size) {
            markdown_core_met_end(tokenizer);
        }
        return markdown_core_nok(tokenizer);
    }
    markdown_core_enter(tokenizer, self->token_base + ENTITIES_REFERENCE);
    markdown_core_consume_to(tokenizer, from + (uint32_t)len);
    markdown_core_exit(tokenizer, self->token_base + ENTITIES_REFERENCE);
    return markdown_core_ok(tokenizer);
}
static const markdown_core_text_construct ENTITIES_CONSTRUCTS[] = {{"&", {.start = S_reference}}};

static void S_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_strbuf ent = MARKDOWN_CORE_BUF_INIT();
    houdini_unescape_ent(&ent, markdown_core_inline_window(inline_state, 0).data + token->start + 1,
                         (bufsize_t)(token->end - token->start - 1));
    if (ent.oom) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    markdown_core_compile_append(compiler, make_str(inline_state, (bufsize_t)token->start, (bufsize_t)token->end - 1,
                                                    markdown_core_chunk_buf_detach(&ent)));
}
static const markdown_core_compile_ops ENTITIES_TOKENS[] = {[ENTITIES_REFERENCE] = {.exit = S_made, .plain = true}};

static const markdown_core_inline_ops ENTITIES_INLINES = {
    .constructs = ENTITIES_CONSTRUCTS,
    .construct_count = 1,
    .tokens = ENTITIES_TOKENS,
    .token_count = 1,
    .precedence = MARKDOWN_CORE_INLINE_FALLBACK,
};

static const markdown_core_decoder ENTITIES_DECODER = {.read = S_read, .decode = S_decode};
static const markdown_core_service ENTITIES_SERVICES[] = {{MARKDOWN_CORE_SERVICE_REFERENCES, &ENTITIES_DECODER}, {0}};

const markdown_core_element MARKDOWN_CORE_ELEMENT_ENTITIES = {
    .name = "entities",
    .provides = ENTITIES_SERVICES,
    .inlines = &ENTITIES_INLINES,
};
