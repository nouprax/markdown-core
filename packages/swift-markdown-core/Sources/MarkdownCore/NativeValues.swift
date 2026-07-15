import MarkdownCoreC

extension ParseError {
    init(from error: OpaquePointer?) {
        guard let error else {
            self.init(code: .internal, message: "markdown parsing failed", scope: nil)
            return
        }
        let rawCode = markdown_core_error_get_code(error).rawValue
        let code = ParseErrorCode(rawValue: Int32(rawCode)) ?? .internal
        var nativeScope = markdown_core_scope()
        let parsedScope =
            markdown_core_error_get_scope(error, &nativeScope)
            ? Scope(from: nativeScope) : nil
        self.init(
            code: code,
            message: markdown_core_error_get_message(error).requiredString,
            scope: parsedScope
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

extension markdown_core_string_view {
    var requiredString: String {
        guard let data else { return "" }
        // The native facade has already validated UTF-8 and this initializer also
        // gives deterministic replacement semantics if that contract regresses.
        // swiftlint:disable:next optional_data_string_conversion
        return String(decoding: UnsafeBufferPointer(start: data, count: length), as: UTF8.self)
    }

    var optionalString: String? {
        data == nil ? nil : requiredString
    }
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
