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
            message: markdown_core_error_get_message(error).required
        )
    }
}

extension markdown_core_string {
    var required: String {
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

extension Placement {
    init(from mode: markdown_core_placement) {
        self = mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? .embedded : .standalone
    }
}

extension TextUnit {
    var native: markdown_core_text_unit {
        switch self {
        case .utf8: MARKDOWN_CORE_TEXT_UNIT_UTF8
        case .utf16: MARKDOWN_CORE_TEXT_UNIT_UTF16
        }
    }
}

extension Flow {
    init(from flow: markdown_core_flow) {
        switch flow {
        case MARKDOWN_CORE_FLOW_LEFT: self = .left
        case MARKDOWN_CORE_FLOW_CENTER: self = .center
        case MARKDOWN_CORE_FLOW_RIGHT: self = .right
        default: self = .none
        }
    }
}

extension markdown_core_optional_string {
    /// `nil` when the source did not write this, and `""` when it wrote it and
    /// it was empty. The presence flag decides; the pointer never does.
    var string: String? {
        has_value ? value.required : nil
    }
}
