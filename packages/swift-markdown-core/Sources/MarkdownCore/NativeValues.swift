import MarkdownCoreC

extension ParseError {
    init(from error: OpaquePointer?) {
        guard let error else {
            self.init(code: .internal, message: "markdown parsing failed")
            return
        }
        let rawCode = markdown_core_error_get_code(error).rawValue
        let code = ParseErrorCode(rawValue: Int32(rawCode)) ?? .internal
        self.init(code: code, message: markdown_core_error_get_message(error).string)
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
    var string: String {
        guard let data else { return "" }
        // Well-formed by construction, not by validation: every entry point
        // into this package takes a Swift `String`, so the bytes the facade
        // hands back are the ones it was given. The facade itself neither
        // validates nor replaces (incremental-canonical-ast.md 7.1), so this
        // initializer's replacement semantics are a decoder default that
        // nothing is expected to reach, not a backstop for a facade contract.
        // swiftlint:disable:next optional_data_string_conversion
        return String(decoding: UnsafeBufferPointer(start: data, count: length), as: UTF8.self)
    }

    var optional: String? {
        data == nil ? nil : string
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
