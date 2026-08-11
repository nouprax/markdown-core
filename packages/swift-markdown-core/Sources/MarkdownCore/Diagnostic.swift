import MarkdownCoreC

/// Why a diagnostic was raised.
///
/// One case, and it is not a placeholder. Markdown has no parse errors:
/// every byte sequence is a valid document, and the constructs an author
/// most often gets "wrong" — an unclosed fence, a link reference with no
/// definition, a short table row — are all defined outcomes of the standard
/// semantics rather than failures. Reporting those would be reporting
/// Markdown itself.
public enum DiagnosticCode: Int32, Sendable, Hashable {
    /// A directive's `{...}` attribute block did not parse.
    ///
    /// The exception, because a directive's attribute grammar is this
    /// library's own rather than Markdown's: braces that do not parse are a
    /// mistake with no defined fallback meaning, and the parse degrades them
    /// to literal text. That degradation is invisible in the tree — the node
    /// simply has no attributes — so without this an editor cannot tell "no
    /// attributes were written" from "the attributes were rejected".
    case directiveAttributes = 1
}

/// One thing to underline: what, and where.
///
/// No message. A message is a string shown to a human, which means it is
/// localized, and localizing it in the library would put one English
/// sentence in it and four copies of the decision to ignore that sentence in
/// the bindings. The code says what happened; the wording belongs to
/// whoever is speaking to the user.
public struct Diagnostic: Sendable, Hashable {
    /// What happened.
    public let code: DiagnosticCode
    /// Where to underline.
    public let scope: Scope
}
