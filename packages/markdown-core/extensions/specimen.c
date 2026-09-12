#include "specimen.h"
#define BLOCK_PEEK(input, at) ((input)->data[(at)])
#include "block_internal.h"

static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                                           bufsize_t pos, markdown_core_specimen_value *value);
static bool markdown_core_specimen_scan(markdown_core_parser *parser, block_start_context *context, block_start *start);
static bufsize_t markdown_core_block_parse_specimen_marker(markdown_core_parser *parser, markdown_core_chunk *input,
                                                           bufsize_t pos, markdown_core_specimen_value *value) {
    bufsize_t begin = pos;
    *value = (markdown_core_specimen_value){0};
    parser->specimen_work++;
    if (BLOCK_PEEK(input, pos++) != '(') {
        return 0;
    }
    int digits = 0;
    while (digits < 9 && markdown_core_isdigit(BLOCK_PEEK(input, pos))) {
        parser->specimen_work++;
        value->start = value->start * 10 + input->data[pos++] - '0';
        digits++;
    }
    value->has_start = digits > 0;
    if ((digits && !value->start) || BLOCK_PEEK(input, pos++) != '@') {
        return 0;
    }
    bufsize_t label = pos;
    bool alnum = false;
    while (pos < input->len) {
        int32_t scalar;
        int width = markdown_core_utf8proc_iterate(input->data + pos, input->len - pos, &scalar);
        parser->specimen_work++;
        if (markdown_core_utf8proc_is_letter(scalar) || markdown_core_utf8proc_is_number(scalar)) {
            alnum = true;
        } else if ((scalar == '_' || scalar == '-') && alnum) {
            alnum = false;
        } else {
            break;
        }
        pos += width;
    }
    if ((pos != label && !alnum) || BLOCK_PEEK(input, pos) != ')' ||
        !markdown_core_isspace(BLOCK_PEEK(input, pos + 1))) {
        return 0;
    }
    if (pos > label) {
        value->id = markdown_core_optional_chunk_present(markdown_core_chunk_dup(input, label, pos - label));
    }
    return pos + 1 - begin;
}

void markdown_core_block_prepare_specimens(markdown_core_parser *parser) {
    markdown_core_definition_collection *collection = &parser->specimens;
    if (!markdown_core_key_index_init(&parser->specimen_ids, parser->mem, collection->count) ||
        (collection->count && !markdown_core_block_order_definitions(parser->mem, collection))) {
        parser->oom = true;
        return;
    }
    for (size_t i = 0; i < collection->count; i++) {
        markdown_core_node *definition = collection->values[i].definition;
        markdown_core_optional_chunk *id = &definition->as.specimen->id;
        if (id->has_value && !markdown_core_key_index_insert(&parser->specimen_ids, id->value.data, id->value.len,
                                                             definition, 0, NULL)) {
            parser->oom = true;
            return;
        }
    }
}

static bool markdown_core_specimen_open(markdown_core_parser *parser, markdown_core_node **container,
                                        markdown_core_chunk *input, block_start *start) {
    bufsize_t matched = start->matched;
    markdown_core_specimen_value specimen = start->specimen;

    if (specimen.id.has_value && !markdown_core_chunk_to_cstr(parser->mem, &specimen.id.value)) {
        parser->oom = true;
        return false;
    }
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_SPECIMEN, parser->first_nonspace + 1);
    if (!*container) {
        markdown_core_optional_chunk_free(parser->mem, &specimen.id);
        return false;
    }
    if ((*container)->prev && (*container)->prev->kind == MARKDOWN_CORE_NODE_SPECIMEN) {
        specimen.has_start = false;
        specimen.start = 0;
    }
    *(*container)->as.specimen = specimen;
    if (!markdown_core_parser_register_definition(parser, &parser->specimens, *container, NULL, NULL)) {
        return false;
    }
    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
    while (markdown_core_block_is_space_or_tab(input->data[parser->offset])) {
        markdown_core_block_advance_offset(parser, input, 1, true);
        parser->specimen_work++;
    }
    return true;
}

static bool markdown_core_specimen_scan(markdown_core_parser *parser, block_start_context *context,
                                        block_start *start) {
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(!context->paragraph &&
          (start->matched = markdown_core_block_parse_specimen_marker(parser, input, first, &start->specimen)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_SPECIMEN;
    start->open = markdown_core_specimen_open;
    return true;
}

bool markdown_core_specimen_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, 4, true);
}

const markdown_core_extension MARKDOWN_CORE_EXTENSION_SPECIMEN = {
    .name = "specimen",
    .block_precedence = MARKDOWN_CORE_BLOCK_MARKER,
    .scan_block_start = markdown_core_specimen_scan,
};

void markdown_core_specimen_finish(markdown_core_parser *parser) {
    markdown_core_block_own_definitions(&parser->specimens, &parser->root->as.document->specimens);
    parser->mem->free(parser->specimens.values);
    parser->specimens = (markdown_core_definition_collection){0};
    markdown_core_key_index_free(&parser->specimen_ids);
}
