#include <string.h>

#include "../include/markdown_core.h"

#include "ast_internal.h"

/* A NODE'S SCALARS, compared field by field: everything a binding builds
 * into the node's value (the MCB3 record, wire/markdown_core_wire.c) except
 * its id, its extent and its node-valued fields, which publishing compares
 * itself. Each comparison reads the node through the same accessor the
 * record is written from, so the two cannot disagree about what a value is. */

static bool string_equal(markdown_core_string a, markdown_core_string b) {
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, a.length));
}

static bool optional_string_equal(markdown_core_optional_string a, markdown_core_optional_string b) {
    return a.has_value == b.has_value && (!a.has_value || string_equal(a.value, b.value));
}

static bool optional_int_equal(markdown_core_optional_i64 a, markdown_core_optional_i64 b) {
    return a.has_value == b.has_value && (!a.has_value || a.value == b.value);
}

static bool chunk_equal(markdown_core_chunk a, markdown_core_chunk b) {
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, (size_t)a.len));
}

/* An attribute value is compared from its representation, which its
 * accessors (`markdown_core_attribute_value_*`) present member for member:
 * the anchor, whose empty bytes are no anchor, then the classes and the
 * records in order. */
static bool attributes_equal(const markdown_core_attributes *a, const markdown_core_attributes *b) {
    if (a->class_count != b->class_count || a->record_count != b->record_count || !chunk_equal(a->anchor, b->anchor)) {
        return false;
    }
    for (uint32_t i = 0; i < a->class_count; i++) {
        if (!chunk_equal(a->classes[i], b->classes[i])) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a->record_count; i++) {
        if (!chunk_equal(a->records[i].name, b->records[i].name) ||
            !chunk_equal(a->records[i].value, b->records[i].value)) {
            return false;
        }
    }
    return true;
}

static bool destination_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_destination x, y;
    markdown_core_node_destination(a, &x);
    markdown_core_node_destination(b, &y);
    if (x.kind != y.kind) {
        return false;
    }
    return x.kind == MARKDOWN_CORE_DESTINATION_URL
               ? string_equal(x.url, y.url)
               : string_equal(x.path, y.path) && optional_string_equal(x.anchor, y.anchor);
}

static bool dimensions_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_dimensions *x, *y;
    markdown_core_node_dimensions(a, &x);
    markdown_core_node_dimensions(b, &y);
    return x == y || (x && y && x->width == y->width && optional_int_equal(x->height, y->height));
}

/* Whether two `Link` or `Embedded` nodes read equal resources. */
static bool resources_equal(const markdown_core_node *a, const markdown_core_node *b) {
    const markdown_core_resource *x, *y;
    markdown_core_node_resource(a, &x);
    markdown_core_node_resource(b, &y);
    if (x == y) {
        return true;
    }
    markdown_core_optional_string x_title, y_title;
    const markdown_core_attribute_value *x_inherited, *y_inherited;
    markdown_core_node_title(a, &x_title);
    markdown_core_node_title(b, &y_title);
    markdown_core_node_inherited_attributes(a, &x_inherited);
    markdown_core_node_inherited_attributes(b, &y_inherited);
    return destination_equal(a, b) && optional_string_equal(x_title, y_title) &&
           attributes_equal(x_inherited, y_inherited);
}

static bool metadata_value_equal(const markdown_core_metadata_value *a, const markdown_core_metadata_value *b) {
    if (!a || !b) {
        return a == b;
    }
    markdown_core_metadata_value_kind kind = markdown_core_metadata_value_get_kind(a);
    if (kind != markdown_core_metadata_value_get_kind(b)) {
        return false;
    }
    if (kind == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar x, y;
        markdown_core_metadata_value_scalar(a, &x);
        markdown_core_metadata_value_scalar(b, &y);
        return x.kind == y.kind &&
               (x.kind == MARKDOWN_CORE_METADATA_NULL ||
                (x.kind == MARKDOWN_CORE_METADATA_BOOL ? x.value.boolean == y.value.boolean
                                                       : string_equal(x.value.string, y.value.string)));
    }
    size_t count, other;
    markdown_core_metadata_value_item_count(a, &count);
    markdown_core_metadata_value_item_count(b, &other);
    if (count != other) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        markdown_core_metadata_list_item x, y;
        markdown_core_metadata_value_item_at(a, i, &x);
        markdown_core_metadata_value_item_at(b, i, &y);
        if (x.kind != y.kind || !string_equal(x.value, y.value)) {
            return false;
        }
    }
    return true;
}

typedef markdown_core_status (*metadata_field)(const markdown_core_node *, const markdown_core_metadata_value **);

static bool metadata_equal(const markdown_core_node *a, const markdown_core_node *b) {
    static const metadata_field fields[] = {
        markdown_core_metadata_name,     markdown_core_metadata_title,    markdown_core_metadata_subtitle,
        markdown_core_metadata_time,     markdown_core_metadata_date,     markdown_core_metadata_authors,
        markdown_core_metadata_keywords, markdown_core_metadata_abstract, markdown_core_metadata_state,
        markdown_core_metadata_comment,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        const markdown_core_metadata_value *x, *y;
        fields[i](a, &x);
        fields[i](b, &y);
        if (!metadata_value_equal(x, y)) {
            return false;
        }
    }
    return true;
}

static bool list_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_list_flavor x_flavor, y_flavor;
    markdown_core_optional_i64 x_start, y_start;
    markdown_core_ordered_list_variant x_variant, y_variant;
    markdown_core_ordered_list_delimiter x_delimiter, y_delimiter;
    bool x_tight, y_tight;
    markdown_core_node_list_properties(a, &x_flavor, &x_start, &x_variant, &x_delimiter, &x_tight);
    markdown_core_node_list_properties(b, &y_flavor, &y_start, &y_variant, &y_delimiter, &y_tight);
    if (x_flavor != y_flavor || x_tight != y_tight || !optional_int_equal(x_start, y_start)) {
        return false;
    }
    /* The ordered-marker facts exist exactly when the list has a start. */
    if (!x_start.has_value) {
        return true;
    }
    bool cased = x_variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA ||
                 x_variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN;
    bool closable = x_delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS;
    return x_variant.kind == y_variant.kind && (!cased || x_variant.lowercased == y_variant.lowercased) &&
           x_delimiter.kind == y_delimiter.kind && (!closable || x_delimiter.closed == y_delimiter.closed);
}

static bool table_equal(const markdown_core_node *a, const markdown_core_node *b) {
    size_t columns, head, content, foot, other;
    markdown_core_node_table_properties(a, &columns, &head, &content, &foot);
    markdown_core_node_table_properties(b, &other, &head, &content, &foot);
    if (columns != other) {
        return false;
    }
    for (size_t i = 0; i < columns; i++) {
        markdown_core_table_column x, y;
        markdown_core_node_table_column_at(a, i, &x);
        markdown_core_node_table_column_at(b, i, &y);
        if (x.flow != y.flow || x.relative.has_value != y.relative.has_value ||
            (x.relative.has_value && x.relative.value != y.relative.value)) {
            return false;
        }
    }
    return true;
}

static bool referent_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_referent x, y;
    markdown_core_citation_referent(a, &x);
    markdown_core_citation_referent(b, &y);
    if (x.kind != y.kind) {
        return false;
    }
    switch (x.kind) {
    case MARKDOWN_CORE_REFERENT_BIB:
        return x.mode == y.mode && string_equal(x.key, y.key);
    case MARKDOWN_CORE_REFERENT_FOOTNOTE:
        /* An inline note is a node-valued field; a label is a scalar. */
        return (x.note != NULL) == (y.note != NULL) && (x.note || string_equal(x.label, y.label));
    case MARKDOWN_CORE_REFERENT_SPECIMEN:
        return string_equal(x.label, y.label);
    }
    return false;
}

bool markdown_core_scalars_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(a);
    markdown_core_optional_string x, y, x_second, y_second;
    markdown_core_string x_literal, y_literal;
    if (!attributes_equal(&a->attributes, &b->attributes)) {
        return false;
    }
    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT: {
        markdown_core_optional_bool x_collapsed, y_collapsed;
        markdown_core_node_callout_properties(a, &x, &x_collapsed);
        markdown_core_node_callout_properties(b, &y, &y_collapsed);
        return optional_string_equal(x, y) && x_collapsed.has_value == y_collapsed.has_value &&
               (!x_collapsed.has_value || x_collapsed.value == y_collapsed.value);
    }
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t x_level, y_level;
        markdown_core_node_heading_level(a, &x_level);
        markdown_core_node_heading_level(b, &y_level);
        return x_level == y_level;
    }
    case MARKDOWN_CORE_KIND_LIST:
        return list_equal(a, b);
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_marker(a, &x);
        markdown_core_node_list_item_marker(b, &y);
        return optional_string_equal(x, y);
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool x_fenced, y_fenced, x_closed, y_closed;
        markdown_core_node_code_block_properties(a, &x, &x_second, &x_literal, &x_fenced, &x_closed);
        markdown_core_node_code_block_properties(b, &y, &y_second, &y_literal, &y_fenced, &y_closed);
        return x_fenced == y_fenced && x_closed == y_closed && optional_string_equal(x, y) &&
               optional_string_equal(x_second, y_second) && string_equal(x_literal, y_literal);
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        markdown_core_node_literal(a, &x_literal);
        markdown_core_node_literal(b, &y_literal);
        return string_equal(x_literal, y_literal);
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement x_mode, y_mode;
        markdown_core_node_formula_properties(a, &x_mode, &x_literal);
        markdown_core_node_formula_properties(b, &y_mode, &y_literal);
        return x_mode == y_mode && string_equal(x_literal, y_literal);
    }
    case MARKDOWN_CORE_KIND_TABLE:
        return table_equal(a, b);
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        int64_t x_rows, x_columns, y_rows, y_columns;
        markdown_core_node_table_cell_spans(a, &x_rows, &x_columns);
        markdown_core_node_table_cell_spans(b, &y_rows, &y_columns);
        return x_rows == y_rows && x_columns == y_columns;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        markdown_core_node_directive_properties(a, &x);
        markdown_core_node_directive_properties(b, &y);
        return optional_string_equal(x, y);
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        markdown_core_node_cross_label(a, &x);
        markdown_core_node_cross_label(b, &y);
        return destination_equal(a, b) && optional_string_equal(x, y) &&
               (kind == MARKDOWN_CORE_KIND_CROSS_LINK || dimensions_equal(a, b));
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED:
        return resources_equal(a, b) && (kind == MARKDOWN_CORE_KIND_LINK || dimensions_equal(a, b));
    case MARKDOWN_CORE_KIND_DEFINITION: {
        bool x_compact, y_compact;
        markdown_core_node_definition_compact(a, &x_compact);
        markdown_core_node_definition_compact(b, &y_compact);
        return x_compact == y_compact;
    }
    case MARKDOWN_CORE_KIND_CITATION:
        return referent_equal(a, b);
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        markdown_core_footnote_label(a, &x);
        markdown_core_footnote_label(b, &y);
        return optional_string_equal(x, y);
    case MARKDOWN_CORE_KIND_SPECIMEN: {
        markdown_core_optional_i64 x_start, y_start;
        markdown_core_specimen_properties(a, &x, &x_start);
        markdown_core_specimen_properties(b, &y, &y_start);
        return optional_string_equal(x, y) && optional_int_equal(x_start, y_start);
    }
    case MARKDOWN_CORE_KIND_METADATA:
        return metadata_equal(a, b);
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_NONE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
    case MARKDOWN_CORE_KIND_CITE:
        return true;
    }
    return true;
}
