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
        // Swift input reaches the native parser as valid UTF-8. This defensive
        // decoding also remains total if an internal payload violates that invariant.
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
