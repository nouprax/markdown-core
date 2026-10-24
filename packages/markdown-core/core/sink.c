#include "alloc.h"
#include "sink.h"

int32_t markdown_core_context_opener_add(markdown_core_context_sink *sink, markdown_core_node_kind kind,
                                         markdown_core_place place, bool block) {
    for (size_t i = sink->opener_count; i-- > 0;) {
        const markdown_core_context_opener *listed = &sink->openers[i];
        if (listed->kind == kind && listed->place.start == place.start && listed->place.end == place.end &&
            listed->block == block) {
            return (int32_t)i;
        }
    }
    markdown_core_context_opener *openers =
        markdown_core_reserve(sink->openers, &sink->opener_capacity, sink->opener_count + 1, sizeof(*openers));
    if (!openers) {
        sink->failed = true;
        return -1;
    }
    sink->openers = openers;
    openers[sink->opener_count] = (markdown_core_context_opener){kind, place, block};
    return (int32_t)sink->opener_count++;
}

void markdown_core_context_field_add(markdown_core_context_sink *sink, const markdown_core_context_field *field) {
    for (size_t i = sink->field_count; i-- > 0;) {
        const markdown_core_context_field *listed = &sink->fields[i];
        if (listed->kind == field->kind && listed->owner == field->owner && listed->opener == field->opener &&
            listed->start == field->start && listed->declares == field->declares) {
            return;
        }
    }
    markdown_core_context_field *fields =
        markdown_core_reserve(sink->fields, &sink->field_capacity, sink->field_count + 1, sizeof(*fields));
    if (!fields) {
        sink->failed = true;
        return;
    }
    sink->fields = fields;
    fields[sink->field_count++] = *field;
}

void markdown_core_context_sink_clear(markdown_core_context_sink *sink) {
    markdown_core_free((void *)sink->blocks);
    markdown_core_free(sink->openers);
    markdown_core_free(sink->fields);
    *sink = (markdown_core_context_sink){.source = sink->source, .length = sink->length};
}
