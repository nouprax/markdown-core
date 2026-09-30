import MarkdownCoreC

extension ParseError {
    init(from error: OpaquePointer?) {
        // swift-format-ignore: NeverForceUnwrap
        let code = ParseErrorCode(rawValue: Int32(markdown_core_error_get_code(error).rawValue))!
        self.init(code: code, message: markdown_core_error_get_message(error).required)
    }
}

extension markdown_core_string {
    var required: String {
        // The bytes are decoded as they are: a payload is never checked for
        // UTF-8 validity, so no optional conversion stands in for that check.
        // swiftlint:disable:next optional_data_string_conversion
        String(decoding: UnsafeBufferPointer(start: data, count: length), as: UTF8.self)
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
    /// Indexed by the native value.
    private static let native: [Flow] = [.none, .left, .center, .right]

    init(from flow: markdown_core_flow) {
        self = Flow.native[Int(flow.rawValue)]
    }
}

extension markdown_core_optional_string {
    /// `nil` when the source did not write this, and `""` when it wrote it and
    /// it was empty. The presence flag decides; the pointer never does.
    var string: String? {
        has_value ? value.required : nil
    }
}
