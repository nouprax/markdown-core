import MarkdownCoreC

/// A node-independent size. Every present component is a positive 32-bit integer.
public struct Dimensions: Sendable, Hashable {
    /// Required width in 1...2147483647.
    public let width: Int
    /// Height in 1...2147483647, or nil when unspecified.
    public let height: Int?

    /// Creates a size with required width and optional height.
    public init(width: Int, height: Int? = nil) {
        precondition((1...Int(Int32.max)).contains(width))
        precondition(height.map { (1...Int(Int32.max)).contains($0) } ?? true)
        self.width = width
        self.height = height
    }
}

extension Dimensions {
    init(_ value: markdown_core_dimensions) {
        self.init(width: Int(value.width), height: value.height.has_value ? Int(value.height.value) : nil)
    }
}

/// Authored horizontal content alignment.
public enum Flow: String, Sendable {
    /// No explicit alignment was authored.
    case none
    /// Align to the left.
    case left
    /// Align to the center.
    case center
    /// Align to the right.
    case right
}
