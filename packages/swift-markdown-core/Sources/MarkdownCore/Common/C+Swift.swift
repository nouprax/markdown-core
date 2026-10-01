import MarkdownCoreC

extension MarkdownCoreError {
    /// The error a failed facade call reports. Each code is the C
    /// `markdown_core_status` of the same value.
    init(_ status: markdown_core_status) {
        switch status {
        case MARKDOWN_CORE_ALLOCATION_FAILED: self.init(code: .allocationFailed)
        case MARKDOWN_CORE_OUT_OF_BOUNDS: self.init(code: .outOfBounds)
        case MARKDOWN_CORE_INSIDE_SCALAR: self.init(code: .insideScalar)
        // A C enum switch is never exhaustive in Swift; the one failure left
        // is MARKDOWN_CORE_KIND_MISMATCH.
        default: self.init(code: .kindMismatch)
        }
    }
}

/// Reads the one out-parameter of a facade accessor. The builder asks each
/// accessor only of the kind it reads, and each `*_at` only below its count,
/// so the facade always answers.
func answer<Value>(
    _ unanswered: Value,
    _ read: (UnsafeMutablePointer<Value>) -> markdown_core_status
) -> Value {
    var value = unanswered
    answered(read(&value))
    return value
}

/// A node, value or view the facade answers through a pointer out-parameter,
/// `nil` when the relation or field is absent.
func answer(_ read: (UnsafeMutablePointer<OpaquePointer?>) -> markdown_core_status) -> OpaquePointer? {
    answer(nil, read)
}

/// The status of a facade call the binding makes only where it cannot fail.
func answered(_ status: markdown_core_status) {
    assert(status == MARKDOWN_CORE_OK, "the facade refused a call the binding makes only where it answers")
}

/// The literal of a `Text`, `Code`, `HTML`, `HTMLBlock` or `Comment`.
func nativeLiteral(of node: OpaquePointer) -> String {
    answer(markdown_core_string()) { markdown_core_node_literal(node, $0) }.required
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
