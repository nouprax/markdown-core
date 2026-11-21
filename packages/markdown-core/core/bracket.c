#include "alloc.h"
#include "bracket.h"
#include "dialect.h"
#include "parser.h"

/* Each element of the dialect with a `family` hook is told of `b`. */
static void S_tell(markdown_core_inline_state *inline_state, markdown_core_inline_hook family, bracket *b) {
    const markdown_core_dialect *dialect = inline_state->dialect;
    const markdown_core_element_instance *const *owners = dialect->inline_hooks[family];
    const size_t count = dialect->inline_hook_counts[family];
    for (size_t i = 0; i < count; i++) {
        const markdown_core_inline_ops *inlines = owners[i]->element->inlines;
        if (family == MARKDOWN_CORE_INLINE_HOOK_OPENED) {
            inlines->opened(owners[i], inline_state, b);
        } else if (family == MARKDOWN_CORE_INLINE_HOOK_SETTLED) {
            inlines->settled(owners[i], inline_state, b);
        } else {
            inlines->freed(owners[i], inline_state, b);
        }
    }
}

void markdown_core_inline_push_bracket(const markdown_core_element_instance *owner,
                                       markdown_core_inline_state *inline_state, bracket_kind kind,
                                       markdown_core_node *text) {
    bracket *b = (bracket *)markdown_core_alloc(1, sizeof(bracket) + inline_state->dialect->bracket_size);
    if (!b) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    markdown_core_bracket_scope *brackets = &inline_state->brackets;
    if (brackets->last != NULL) {
        brackets->last->bracket_after = true;
        if (kind != BRACKET_FOOTNOTE) {
            b->in_bracket_image0 = brackets->last->in_bracket_image0;
            b->in_bracket_image1 = brackets->last->in_bracket_image1;
        }
    }
    b->owner = owner;
    b->kind = kind;
    b->outer_no_link_openers = brackets->no_link_openers;
    b->inl_text = text;
    b->previous = brackets->last;
    b->position = inline_state->pos;
    if (kind == BRACKET_IMAGE) {
        b->in_bracket_image1 = true;
    } else if (kind == BRACKET_LINK) {
        b->in_bracket_image0 = true;
    }
    S_tell(inline_state, MARKDOWN_CORE_INLINE_HOOK_OPENED, b);
    brackets->last = b;
    if (kind != BRACKET_IMAGE) {
        brackets->no_link_openers = false;
    }
}

void markdown_core_inline_pop_bracket(markdown_core_inline_state *inline_state) {
    markdown_core_bracket_scope *brackets = &inline_state->brackets;
    bracket *b = brackets->last;
    if (b == NULL) {
        return;
    }
    brackets->last = b->previous;
    if (b->close_text) {
        if (b->pending_previous) {
            b->pending_previous->pending_next = b->pending_next;
        } else {
            brackets->pending = b->pending_next;
        }
        if (b->pending_next) {
            b->pending_next->pending_previous = b->pending_previous;
        }
        markdown_core_inline_remove_delimiter(inline_state, b->delim_end);
    }
    S_tell(inline_state, MARKDOWN_CORE_INLINE_HOOK_FREED, b);
    markdown_core_free(b);
}

void markdown_core_inline_settle_bracket(markdown_core_inline_state *inline_state, bracket *opener) {
    S_tell(inline_state, MARKDOWN_CORE_INLINE_HOOK_SETTLED, opener);
}

void markdown_core_inline_take_bracket_content(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                               bracket *opener, markdown_core_node *owner) {
    (void)inline_state;
    markdown_core_node *child = markdown_core_scaffold_next(opener->inl_text);
    while (child != opener->close_text) {
        markdown_core_node *next = markdown_core_scaffold_next(child);
        markdown_core_scaffold_unlink(child);
        markdown_core_scaffold_append(owner, child);
        parser->bracket_work++;
        child = next;
    }
}

void markdown_core_inline_replace_bracket_opener(markdown_core_inline_state *inline_state, bracket *opener,
                                                 markdown_core_node *replacement) {
    if (opener->kind == BRACKET_IMAGE) {
        opener->inl_text->as.literal->len = 1;
        markdown_core_inline_state_place(inline_state, opener->inl_text, opener->position - 2, opener->position - 2);
        /* The replacement follows the `!`, which stays as text. */
        markdown_core_node *after = markdown_core_scaffold_next(opener->inl_text);
        if (after) {
            markdown_core_scaffold_insert_before(after, replacement);
        } else {
            markdown_core_scaffold_append(markdown_core_scaffold_parent(opener->inl_text), replacement);
        }
    } else {
        markdown_core_scaffold_insert_before(opener->inl_text, replacement);
        markdown_core_parser_release_node(inline_state->owner_parser, opener->inl_text);
    }
}

int markdown_core_inline_state_in_bracket(markdown_core_inline_state *inline_state, int image) {
    const bracket *b = inline_state->brackets.last;
    if (!b) {
        return 0;
    }
    return image != 0 ? b->in_bracket_image1 : b->in_bracket_image0;
}

unsigned char markdown_core_inline_state_closing_bracket(markdown_core_inline_state *inline_state) {
    return inline_state->brackets.last ? ']' : 0;
}

int markdown_core_inline_state_context_start(markdown_core_inline_state *inline_state) {
    const bracket *opener = inline_state->brackets.last;
    return opener && opener->kind == BRACKET_FOOTNOTE ? opener->position : 0;
}

/* The run ends: every bracket it holds, open or suspended, is freed. */
void markdown_core_inline_clear_brackets(markdown_core_inline_state *inline_state) {
    markdown_core_bracket_scope *brackets = &inline_state->brackets;
    while (brackets->last) {
        markdown_core_inline_pop_bracket(inline_state);
    }
    while (brackets->pending) {
        bracket *next = brackets->pending->pending_next;
        S_tell(inline_state, MARKDOWN_CORE_INLINE_HOOK_FREED, brackets->pending);
        markdown_core_free(brackets->pending);
        brackets->pending = next;
    }
}

/* Whether `owner` closes the brackets it opens by rules of its own alone
 * (element.h, markdown_core_close_rule, `mine`). */
static bool S_mine(const markdown_core_dialect *dialect, const markdown_core_element_instance *owner) {
    for (size_t i = 0; i < dialect->closer_count; i++) {
        if (dialect->closers[i].rule->mine && dialect->closers[i].owner == owner) {
            return true;
        }
    }
    return false;
}

markdown_core_node *markdown_core_inline_close_bracket(markdown_core_parser *parser,
                                                       markdown_core_inline_state *inline_state) {
    parser->bracket_work++;
    const bufsize_t after = ++inline_state->pos;
    bracket *opener = inline_state->brackets.last;
    if (opener) {
        const markdown_core_dialect *dialect = inline_state->dialect;
        const bool mine = S_mine(dialect, opener->owner);
        for (size_t i = 0; i < dialect->closer_count; i++) {
            const markdown_core_closer *closer = &dialect->closers[i];
            if (closer->rule->mine != mine || (mine && closer->owner != opener->owner)) {
                continue;
            }
            inline_state->pos = after;
            markdown_core_node *made = NULL;
            const markdown_core_bracket_match match =
                closer->rule->close(closer->owner, parser, inline_state, opener, &made);
            if (match == BRACKET_MATCHED) {
                return made;
            }
            if (match == BRACKET_REJECTED) {
                break;
            }
        }
        /* No construct: the body stays as it was read, and the `]` is
         * text. */
        markdown_core_inline_settle_bracket(inline_state, opener);
        markdown_core_inline_pop_bracket(inline_state);
        inline_state->pos = after;
    }
    return make_str(inline_state, after - 1, after - 1, markdown_core_chunk_dup(&inline_state->input, after - 1, 1));
}

void markdown_core_inline_suspend_bracket(markdown_core_inline_state *inline_state, bracket *opener,
                                          markdown_core_node *close, delimiter *end) {
    markdown_core_bracket_scope *brackets = &inline_state->brackets;
    assert(brackets->last == opener);
    opener->close_text = close;
    opener->close_position = inline_state->pos - 1;
    opener->delim_end = end;
    opener->pending_no_link_openers = brackets->no_link_openers;
    opener->pending_next = brackets->pending;
    if (brackets->pending) {
        brackets->pending->pending_previous = opener;
    }
    brackets->pending = opener;
    brackets->last = opener->previous;
}

void markdown_core_inline_resume_bracket(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         bracket *opener) {
    markdown_core_bracket_scope *brackets = &inline_state->brackets;
    bracket *saved_bracket = brackets->last;
    const bufsize_t saved_pos = inline_state->pos;
    const bool saved_no_links = brackets->no_link_openers;
    markdown_core_node *close = opener->close_text;
    opener->previous = saved_bracket;
    brackets->last = opener;
    inline_state->pos = opener->close_position;
    brackets->no_link_openers = opener->pending_no_link_openers;
    markdown_core_node *literal = markdown_core_inline_close_bracket(parser, inline_state);
    if (literal) {
        markdown_core_parser_release_node(parser, literal);
    } else if (!inline_state->error && !parser->error) {
        markdown_core_parser_release_node(parser, close);
    }
    inline_state->pos = saved_pos;
    brackets->last = saved_bracket;
    brackets->no_link_openers |= saved_no_links;
}
