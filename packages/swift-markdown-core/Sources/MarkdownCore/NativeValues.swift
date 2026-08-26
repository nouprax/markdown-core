import MarkdownCoreC

extension ParseError {
    init(from error: OpaquePointer?) {
        guard let error else {
            self.init(code: .internal, message: "markdown parsing failed")
            return
        }
        let rawCode = markdown_core_error_get_code(error).rawValue
        let code = ParseErrorCode(rawValue: Int32(rawCode)) ?? .internal
        self.init(
            code: code,
            message: markdown_core_error_get_message(error).requiredString
        )
    }

    /// Reads the native error into a value and FREES it. Every C entry point
    /// that fails hands its error object over exactly once, so the handover
    /// ends here — the one place a throw is built from a native failure.
    static func take(_ error: OpaquePointer?) -> ParseError {
        defer { markdown_core_error_free(error) }
        return ParseError(from: error)
    }
}

extension ParseOptions {
    /// The C spelling of this option set, field for field. One mapping, read
    /// by the one-shot parse and the session alike, so the two entries cannot
    /// disagree about what an option means.
    var native: markdown_core_parse_options {
        markdown_core_parse_options(
            smart_punctuation: smartPunctuation,
            footnotes: footnotes,
            strip_html_comments: stripHTMLComments,
            tables: tables,
            strikethrough: strikethrough,
            autolinks: autolinks,
            task_lists: taskLists,
            formulas: formulas,
            directives: directives
        )
    }
}

extension Scope {
    init(from scope: markdown_core_scope) {
        self.init(
            start: Position(line: scope.start.line, column: scope.start.column),
            end: Position(line: scope.end.line, column: scope.end.column)
        )
    }
}

extension markdown_core_string {
    var requiredString: String {
        guard let data else { return "" }
        // The native facade has already validated UTF-8 and this initializer also
        // gives deterministic replacement semantics if that contract regresses.
        // swiftlint:disable:next optional_data_string_conversion
        return String(decoding: UnsafeBufferPointer(start: data, count: length), as: UTF8.self)
    }

    // `optionalString` USED TO LIVE HERE and read absence off the pointer.
    // Requirement 14 moved that question to the value itself: see
    // `markdown_core_optional_string.string`.
}

extension PlacementMode {
    init(from mode: markdown_core_placement_mode) {
        self = mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? .embedded : .standalone
    }
}

extension TableAlignment {
    init(from alignment: markdown_core_table_alignment) {
        switch alignment {
        case MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT: self = .left
        case MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER: self = .center
        case MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT: self = .right
        default: self = .none
        }
    }
}

extension markdown_core_optional_string {
    /// `nil` when the source did not write this, and `""` when it wrote it and
    /// it was empty. The presence flag decides; the pointer never does.
    var string: String? {
        has_value ? value.requiredString : nil
    }
}
