#include "attributes.h"
#include "citation.h"
#include "link.h"
#include "media.h"
#include "inline_internal.h"
#include "block_internal.h"

static bufsize_t markdown_core_inline_manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset,
                                                           markdown_core_chunk *output);
bool markdown_core_block_resolve_reference_link_definitions(markdown_core_parser *parser, markdown_core_node *b) {
    bufsize_t pos;
    markdown_core_strbuf *node_content = &b->content;
    markdown_core_chunk chunk = {node_content->ptr, node_content->size, 0};
    markdown_core_attribute_parser attributes = {.mem = parser->mem, .data = chunk.data, .length = chunk.len};
    while (chunk.len && chunk.data[0] == '[') {
        int line = b->start_line, column = b->start_column;
        markdown_core_parser_content_place(parser, b, (bufsize_t)(chunk.data - node_content->ptr), &line, &column);
        uint64_t source_key = ((uint64_t)(uint32_t)line << 32) | (uint32_t)column;
        pos = markdown_core_parse_reference_inline(parser->mem, &chunk, parser->refmap, &attributes, source_key);
        if (!pos) {
            break;
        }
        chunk.data += pos;
        chunk.len -= pos;
    }
    if (attributes.oom) {
        parser->oom = true;
    }
    parser->attribute_work += attributes.work;
    markdown_core_attribute_parser_free(&attributes);
    // The definitions are dropped off the FRONT of the block's content, so what
    // is left starts further down the source than the block was told it did.
    // Without this a paragraph whose leading definitions were consumed keeps the
    // DEFINITION's position, and so does every inline in it, because
    // markdown_core_parse_inlines seeds the subject from b->start_line and
    // b->start_column.
    //
    // D18 corrected the LINE here by counting the line endings in the prefix
    // that goes away, and left the column alone with the note that it was
    // right wherever the remaining first line has the same stripped prefix as
    // the definition's line. Requirement 10 removes both the count and the
    // caveat: the map says where the surviving first byte was written, so the
    // column is answered rather than assumed, and the marks are rebased so the
    // inline phase reads the same map against the shortened buffer.
    bufsize_t dropped = node_content->size - chunk.len;
    int line, column;
    markdown_core_block_rebase_content_marks(parser, b, dropped, chunk.len);
    markdown_core_strbuf_drop(node_content, dropped);
    /* The block now begins where its FIRST SURVIVING line was written, and
     * that is asked of the map rather than derived: this function can be
     * reached twice on one paragraph -- once at the setext-underline check and
     * again at markdown_core_block_finalize -- and the first call can consume everything recorded
     * so far, leaving the line that carries what is left still unread. Taking
     * the answer from the surviving run rather than from the size of the cut
     * is what makes both arrivals give the same result. On a block with no
     * definitions in front of it this is what the block already said. */
    if (markdown_core_parser_content_place(parser, b, 0, &line, &column)) {
        b->start_line = line;
        b->start_column = column;
    }
    return !markdown_core_block_is_blank(&b->content, 0);
}

markdown_core_chunk markdown_core_clean_url(markdown_core_mem *mem, markdown_core_chunk *url, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);

    markdown_core_chunk_trim(url);

    if (url->len == 0) {
        return markdown_core_chunk_literal("");
    }

    houdini_unescape_html_f(&buf, url->data, url->len);

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_chunk_buf_detach(&buf);
}

markdown_core_optional_chunk markdown_core_clean_title(markdown_core_mem *mem, markdown_core_chunk *title, int *lost) {
    markdown_core_strbuf buf = MARKDOWN_CORE_BUF_INIT(mem);
    unsigned char first, last;

    if (title->len == 0) {
        return markdown_core_optional_chunk_absent();
    }

    first = title->data[0];
    last = title->data[title->len - 1];

    // remove surrounding quotes if any:
    if ((first == '\'' && last == '\'') || (first == '(' && last == ')') || (first == '"' && last == '"')) {
        houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
    } else {
        houdini_unescape_html_f(&buf, title->data, title->len);
    }

    markdown_core_strbuf_unescape(&buf);
    if (buf.oom && lost) {
        *lost = 1;
    }
    return markdown_core_optional_chunk_present(markdown_core_chunk_buf_detach(&buf));
}

bufsize_t markdown_core_inline_reference_label_length(const unsigned char *data, bufsize_t length) {
    bufsize_t at = 0;
    while (at < length && at <= MAX_LINK_LABEL_LENGTH && data[at] != '[' && data[at] != ']') {
        if (data[at] == '\\' && at + 1 < length && markdown_core_ispunct(data[at + 1])) {
            at++;
        }
        at++;
    }
    return at;
}

int markdown_core_inline_link_label(subject *subj, markdown_core_chunk *raw_label) {
    bufsize_t startpos = subj->pos;
    int length = 0;
    unsigned char c;

    // advance past [
    if (markdown_core_inline_peek_char(subj) == '[') {
        subj->pos++;
    } else {
        return 0;
    }

    length = markdown_core_inline_reference_label_length(subj->input.data + subj->pos, subj->input.len - subj->pos);
    subj->pos += length;
    if (length > MAX_LINK_LABEL_LENGTH) {
        goto noMatch;
    }
    c = markdown_core_inline_peek_char(subj);

    if (c == ']') { // match found
        *raw_label = markdown_core_chunk_dup(&subj->input, startpos + 1, subj->pos - (startpos + 1));
        markdown_core_chunk_trim(raw_label);
        subj->pos++; // advance past ]
        return 1;
    }

noMatch:
    subj->pos = startpos; // rewind
    return 0;
}

static bufsize_t manual_scan_link_url_2(markdown_core_chunk *input, bufsize_t offset, markdown_core_chunk *output) {
    bufsize_t i = offset;
    size_t nb_p = 0;

    while (i < input->len) {
        if (input->data[i] == '\\' && i + 1 < input->len && markdown_core_ispunct(input->data[i + 1])) {
            i += 2;
        } else if (input->data[i] == '(') {
            ++nb_p;
            ++i;
            if (nb_p > 32) {
                return -1;
            }
        } else if (input->data[i] == ')') {
            if (nb_p == 0) {
                break;
            }
            --nb_p;
            ++i;
        } else if (markdown_core_isspace(input->data[i])) {
            if (i == offset) {
                return -1;
            }
            break;
        } else {
            ++i;
        }
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset, i - offset, 0};
        *output = result;
    }
    return i - offset;
}

static bufsize_t markdown_core_inline_manual_scan_link_url(markdown_core_chunk *input, bufsize_t offset,
                                                           markdown_core_chunk *output) {
    bufsize_t i = offset;

    if (i < input->len && input->data[i] == '<') {
        ++i;
        while (i < input->len) {
            if (input->data[i] == '>') {
                ++i;
                break;
            } else if (input->data[i] == '\\') {
                i += 2;
            } else if (input->data[i] == '\n' || input->data[i] == '<') {
                return -1;
            } else {
                ++i;
            }
        }
    } else {
        return manual_scan_link_url_2(input, offset, output);
    }

    if (i >= input->len) {
        return -1;
    }

    {
        markdown_core_chunk result = {input->data + offset + 1, i - 2 - offset, 0};
        *output = result;
    }
    return i - offset;
}

static void spnl(subject *subj) {
    markdown_core_inline_skip_spaces(subj);
    if (markdown_core_inline_skip_line_end(subj)) {
        markdown_core_inline_skip_spaces(subj);
    }
}

static bool reference_tail(subject *subj, markdown_core_attribute_parser *attributes, markdown_core_attributes *value) {
    bufsize_t before = subj->pos;
    spnl(subj);
    bufsize_t base = (bufsize_t)(subj->input.data - attributes->data);
    bufsize_t start = base + subj->pos;
    bufsize_t end = markdown_core_attributes_end(attributes, start);
    if (end) {
        subj->pos = end - base;
        markdown_core_inline_skip_spaces(subj);
        if (markdown_core_inline_skip_line_end(subj)) {
            return markdown_core_attributes_parse(attributes, start, value, &end) != 0;
        }
    }
    subj->pos = before;
    markdown_core_inline_skip_spaces(subj);
    return markdown_core_inline_skip_line_end(subj);
}

bufsize_t markdown_core_parse_reference_inline(markdown_core_mem *mem, markdown_core_chunk *input,
                                               markdown_core_map *refmap, markdown_core_attribute_parser *attributes,
                                               uint64_t source_key) {
    subject subj;
    markdown_core_resource *resource;
    int lost = 0;
    markdown_core_attributes value = {0};

    markdown_core_chunk lab;
    markdown_core_chunk url;
    markdown_core_chunk title;
    const markdown_core_chunk absent_title = MARKDOWN_CORE_CHUNK_EMPTY;

    bufsize_t matchlen = 0;
    bufsize_t beforetitle;

    markdown_core_inline_subject_from_buf(NULL, mem, -1, &subj, input, NULL);

    // parse label:
    if (!markdown_core_inline_link_label(&subj, &lab) || lab.len == 0) {
        return 0;
    }
    // colon:
    if (markdown_core_inline_peek_char(&subj) == ':') {
        subj.pos++;
    } else {
        return 0;
    }

    // parse link url:
    spnl(&subj);
    if ((matchlen = markdown_core_inline_manual_scan_link_url(&subj.input, subj.pos, &url)) > -1) {
        subj.pos += matchlen;
    } else {
        return 0;
    }

    // parse optional link_title
    beforetitle = subj.pos;
    spnl(&subj);
    matchlen = subj.pos == beforetitle ? 0 : scan_link_title(&subj.input, subj.pos);
    if (matchlen) {
        title = markdown_core_chunk_dup(&subj.input, subj.pos, matchlen);
        subj.pos += matchlen;
    } else {
        subj.pos = beforetitle;
        // No title was written, so record that rather than an empty one.
        title = absent_title;
    }

    // parse final spaces and newline:
    if (!reference_tail(&subj, attributes, &value)) {
        if (matchlen) { // try rewinding before title
            subj.pos = beforetitle;
            if (!reference_tail(&subj, attributes, &value)) {
                return 0;
            }
            // The title candidate is un-read here: its bytes stay paragraph
            // text, and the definition has no title. `title` still held the
            // scanned chunk, which then went into the reference map -- so a
            // reference to this label resolved with a title the definition does
            // not have, and the same bytes were stated twice, once as prose and
            // once as a title.
            title = absent_title;
        } else {
            return 0;
        }
    }
    if (!refmap) {
        markdown_core_attributes_free(mem, &value);
        return subj.pos;
    }
    // The definition is consumed into the map, which owns its resource ONCE
    // and lends it to every occurrence that resolves to the label (M2). The
    // destination and title are cleaned here, the way a direct link's are, so
    // a resolved occurrence and a direct one state the same values.
    {
        markdown_core_chunk clean_url = markdown_core_clean_url(mem, &url, &lost);
        markdown_core_optional_chunk clean_title = markdown_core_clean_title(mem, &title, &lost);
        resource = lost ? NULL : markdown_core_resource_new(mem, clean_url, clean_title);
        if (!resource) {
            markdown_core_chunk_free(mem, &clean_url);
            markdown_core_optional_chunk_free(mem, &clean_title);
            lost = 1;
        }
    }
    if (resource) {
        resource->attributes = value;
        markdown_core_map_record *record = markdown_core_reference_create(mem, refmap, &lab, resource);
        if (record) {
            record->source_key = source_key;
        }
    } else {
        markdown_core_attributes_free(mem, &value);
    }
    if ((subj.oom || lost) && refmap) {
        refmap->oom = 1;
    }
    return subj.pos;
}

markdown_core_link_match markdown_core_link_recognize(subject *subj, bracket *opener,
                                                      markdown_core_link_candidate *candidate) {
    bufsize_t initial_pos = subj->pos, endurl, starttitle, endtitle, endall, sps, n;
    markdown_core_chunk url_chunk, title_chunk, raw_label;
    markdown_core_chunk url = MARKDOWN_CORE_CHUNK_EMPTY;
    markdown_core_optional_chunk title = {MARKDOWN_CORE_CHUNK_EMPTY, false};
    markdown_core_map_record *record = NULL;
    int found_label;
    bool explicit_tail = false;
    // If we got here, we matched a potential link/image text.
    // Now we check to see if it's a link/image.
    bool is_image = opener->kind == BRACKET_IMAGE;

    bool link_allowed = is_image || !subj->no_link_openers;

    bufsize_t after_link_text_pos = subj->pos;

    // First, look for an inline link.
    if (link_allowed && markdown_core_inline_peek_char(subj) == '(' &&
        ((sps = scan_spacechars(&subj->input, subj->pos + 1)) > -1) &&
        ((n = markdown_core_inline_manual_scan_link_url(&subj->input, subj->pos + 1 + sps, &url_chunk)) > -1)) {

        // try to parse an explicit link:
        endurl = subj->pos + 1 + sps + n;
        starttitle = endurl + scan_spacechars(&subj->input, endurl);

        // ensure there are spaces btw url and title
        endtitle = (starttitle == endurl) ? starttitle : starttitle + scan_link_title(&subj->input, starttitle);

        endall = endtitle + scan_spacechars(&subj->input, endtitle);

        if (markdown_core_inline_peek_at(subj, endall) == ')') {
            explicit_tail = true;
            subj->pos = endall + 1;

            title_chunk = markdown_core_chunk_dup(&subj->input, starttitle, endtitle - starttitle);
            {
                int lost = 0;
                url = markdown_core_clean_url(subj->mem, &url_chunk, &lost);
                title = markdown_core_clean_title(subj->mem, &title_chunk, &lost);
                if (lost) {
                    subj->oom = 1;
                }
            }
            markdown_core_chunk_free(subj->mem, &url_chunk);
            markdown_core_chunk_free(subj->mem, &title_chunk);
            candidate->url = url;
            candidate->title = title;
            candidate->explicit_tail = true;
            return LINK_EXPLICIT;

        } else {
            // it could still be a shortcut reference link
            subj->pos = after_link_text_pos;
        }
    }

    // Next, look for a following [link label] that matches in refmap.
    // skip spaces
    raw_label = markdown_core_chunk_literal("");
    found_label = markdown_core_inline_link_label(subj, &raw_label);
    explicit_tail = found_label;
    if (!found_label) {
        // If we have a shortcut reference link, back up
        // to before the spacse we skipped.
        subj->pos = initial_pos;
    }

    if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
        markdown_core_chunk_free(subj->mem, &raw_label);
        raw_label = markdown_core_chunk_dup(&subj->input, opener->position, initial_pos - opener->position - 1);
        found_label = true;
    }

    /* `[t][l]`, `[l][]` and `[l]` resolve identically and to the same node: the
     * `Link` or `Media` the definition names (M2). Nothing records which of the
     * three spellings the author wrote, and nothing downstream can recover it
     * -- the module states one node for every successful form. */
    if (link_allowed && found_label) {
        record = markdown_core_map_lookup(subj->refmap, &raw_label);
    }
    markdown_core_chunk_free(subj->mem, &raw_label);
    candidate->record = record;
    candidate->explicit_tail = explicit_tail;
    return record ? (explicit_tail ? LINK_EXPLICIT : LINK_SHORTCUT) : LINK_UNMATCHED;
}

bool markdown_core_link_commit(markdown_core_parser *parser, subject *subj, bracket *opener,
                               markdown_core_link_candidate *candidate, bufsize_t initial_pos) {
    bool is_image = opener->kind == BRACKET_IMAGE;
    bool explicit_tail = candidate->explicit_tail;
    markdown_core_map_record *record = candidate->record;
    markdown_core_chunk url = candidate->url;
    markdown_core_optional_chunk title = candidate->title;
    markdown_core_node *inl;
    markdown_core_inline_finish_citation_tokens(subj, &opener->citations);
    if (!markdown_core_node_can_contain_type(opener->inl_text->parent,
                                             is_image ? MARKDOWN_CORE_NODE_MEDIA : MARKDOWN_CORE_NODE_LINK)) {
        markdown_core_chunk_free(subj->mem, &url);
        markdown_core_optional_chunk_free(subj->mem, &title);
        return false;
    }
    inl = markdown_core_inline_make_simple(subj->mem, is_image ? MARKDOWN_CORE_NODE_MEDIA : MARKDOWN_CORE_NODE_LINK);
    if (inl && record) {
        /* A RESOLVED REFERENCE IS THE LINK OR MEDIA IT NAMES (M2), and it reads
         * its destination and title through the definition's resource, which
         * the map owns once and every occurrence shares. Nothing is copied, so
         * there is nothing to charge and no budget can make whether a reference
         * resolves depend on how many resolved before it (D9). The occurrence
         * keeps its own scope, below: the definition's range is never copied,
         * unioned or substituted into it. */
        assert(record->resource != NULL);
        markdown_core_resource_retain(record->resource);
        inl->as.link->resource = record->resource;
    } else if (inl) {
        inl->as.link->resource = markdown_core_resource_new(subj->mem, url, title);
        if (!inl->as.link->resource) {
            markdown_core_node_free(inl);
            inl = NULL;
        }
    }
    if (!inl) {
        subj->oom = 1;
        if (!record) {
            markdown_core_chunk_free(subj->mem, &url);
            markdown_core_optional_chunk_free(subj->mem, &title);
        }
        return false;
    }
    /* REQUIREMENT 11b: the brackets, and whatever follows the closing one --
     * `(...)` with the destination and title, or `[label]` -- are the link's
     * markers. They were claimed CONTENT as they were read, because an
     * unmatched `[` is its own literal; these claims are later and win. The
     * children keep the claims they made for themselves. */
    // A link starts at its own '[' and ends at its closing ')' or ']', and the
    // two need not be on the same line. Taking BOTH from subj->line made a link
    // start where it ENDED: `[a\nb](/u)` reported Link 2:1..2:6 around a child
    // Text at 1:2 -- a node that begins after its own first child.
    inl->start_line = opener->inl_text->start_line;
    inl->start_column = opener->inl_text->start_column;
    if (explicit_tail) {
        markdown_core_inline_attach_inline_attributes(subj, inl, opener->position - 1);
        inl->start_column = opener->inl_text->start_column;
    }
    markdown_core_inline_parser_place(subj, inl, opener->position - 1, subj->pos - 1);
    inl->start_line = opener->inl_text->start_line;
    inl->start_column = opener->inl_text->start_column;
    // And the destination and title are scanned by markdown_core_inline_manual_scan_link_url and
    // scan_link_title, which move subj->pos without ever passing through
    // handle_newline -- so a line ending inside `(...)` is invisible to the
    // subject, and every later node in the paragraph inherits the error.
    // The extent is projected from the two offsets, which is the repair inline
    // code and raw HTML already
    // use; it walks only [initial_pos, subj->pos), which is what the bracket
    // handler consumed for itself. Counting from the OPENING bracket instead
    // would count the label's own newlines a second time -- measured,
    // `[a\nb](/u) tail` then reports line 3 of a two-line document.
    markdown_core_node_insert_before(opener->inl_text, inl);
    markdown_core_inline_take_bracket_content(parser, opener, inl);

    if (is_image) {
        markdown_core_inline_apply_image_dimensions(subj, opener, inl, initial_pos - 1);
    }

    // Free the bracket [:
    markdown_core_node_free(opener->inl_text);

    markdown_core_inline_process_delimiters(parser, subj, opener->position, opener->delim_end);
    markdown_core_inline_pop_bracket(subj);

    // Now, if we have a link, we also want to deactivate links until
    // we get a new opener. (This code can be removed if we decide to allow links
    // inside links.)
    if (!is_image) {
        subj->no_link_openers = true;
    }

    return true;
}
