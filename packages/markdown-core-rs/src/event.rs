//! Semantic labels of things happening.

use crate::util::constant::TAB_SIZE;

/// Semantic label of a span.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Name {
    /// Attention sequence.
    ///
    /// > 👉 **Note**: this is used while parsing but compiled away.
    AttentionSequence,
    /// Whole autolink.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`AutolinkEmail`][Name::AutolinkEmail],
    ///   [`AutolinkMarker`][Name::AutolinkMarker],
    ///   [`AutolinkProtocol`][Name::AutolinkProtocol]
    /// * **Construct**:
    ///   [`autolink`][crate::construct::autolink]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <https://example.com> and <admin@example.com>
    ///     ^^^^^^^^^^^^^^^^^^^^^     ^^^^^^^^^^^^^^^^^^^
    /// ```
    Autolink,
    /// Email autolink w/o markers.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Autolink`][Name::Autolink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`autolink`][crate::construct::autolink]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <admin@example.com>
    ///      ^^^^^^^^^^^^^^^^^
    /// ```
    AutolinkEmail,
    /// Marker of an autolink.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Autolink`][Name::Autolink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`autolink`][crate::construct::autolink]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <https://example.com>
    ///     ^                   ^
    /// ```
    AutolinkMarker,
    /// Protocol autolink w/o markers.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Autolink`][Name::Autolink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`autolink`][crate::construct::autolink]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <https://example.com>
    ///      ^^^^^^^^^^^^^^^^^^^
    /// ```
    AutolinkProtocol,
    /// Line ending preceded only by whitespace or nothing at all.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`blank_line`][crate::construct::blank_line]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ␠␠␊
    ///       ^
    /// ```
    BlankLineEnding,
    /// Whole block quote.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [document content][crate::construct::document]
    /// * **Content model**:
    ///   [`BlockQuotePrefix`][Name::BlockQuotePrefix],
    ///   [flow content][crate::construct::flow]
    /// * **Construct**:
    ///   [`block_quote`][crate::construct::block_quote]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > a
    ///     ^^^
    /// > | b
    ///     ^
    /// ```
    BlockQuote,
    /// Block quote marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`BlockQuotePrefix`][Name::BlockQuotePrefix]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`block_quote`][crate::construct::block_quote]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > a
    ///     ^
    ///   | b
    /// ```
    BlockQuoteMarker,
    /// Block quote prefix.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`BlockQuote`][Name::BlockQuote]
    /// * **Content model**:
    ///   [`BlockQuoteMarker`][Name::BlockQuoteMarker],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`block_quote`][crate::construct::block_quote]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > a
    ///     ^^
    ///   | b
    /// ```
    BlockQuotePrefix,
    /// Byte order mark.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   optional first event
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`document`][crate::construct::document]
    ByteOrderMark,
    /// Callout header.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`CalloutFold`][Name::CalloutFold],
    ///   [`CalloutMarker`][Name::CalloutMarker],
    ///   [`CalloutTitle`][Name::CalloutTitle],
    ///   [`CalloutVariant`][Name::CalloutVariant],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`callout`][crate::construct::callout]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > [!note]- a
    ///       ^^^^^^^^^^
    /// ```
    Callout,
    /// Callout fold.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Callout`][Name::Callout]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`callout`][crate::construct::callout]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > [!note]- a
    ///              ^
    /// ```
    CalloutFold,
    /// Callout marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Callout`][Name::Callout]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`callout`][crate::construct::callout]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > [!note] a
    ///       ^^    ^
    /// ```
    CalloutMarker,
    /// Callout title.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Callout`][Name::Callout]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`callout`][crate::construct::callout]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > [!note] a
    ///               ^
    /// ```
    CalloutTitle,
    /// Callout variant.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Callout`][Name::Callout]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`callout`][crate::construct::callout]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | > [!note] a
    ///         ^^^^
    /// ```
    CalloutVariant,
    /// Whole cross link or cross embed.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`CrossLinkLabel`][Name::CrossLinkLabel],
    ///   [`CrossLinkMarker`][Name::CrossLinkMarker],
    ///   [`CrossLinkSeparator`][Name::CrossLinkSeparator],
    ///   [`CrossLinkDest`][Name::CrossLinkDest]
    /// * **Construct**:
    ///   [`cross_link`][crate::construct::cross_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [[b|c]] d
    ///       ^^^^^^^
    /// ```
    CrossLink,
    /// Cross link label.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CrossLink`][Name::CrossLink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`cross_link`][crate::construct::cross_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [[b|c]] d
    ///            ^
    /// ```
    CrossLinkLabel,
    /// Cross link marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CrossLink`][Name::CrossLink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`cross_link`][crate::construct::cross_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![[b]] d
    ///       ^^^ ^^
    /// ```
    CrossLinkMarker,
    /// Cross link separator.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CrossLink`][Name::CrossLink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`cross_link`][crate::construct::cross_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [[b|c]] d
    ///          ^
    /// ```
    CrossLinkSeparator,
    /// Cross link destination.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CrossLink`][Name::CrossLink]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`cross_link`][crate::construct::cross_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [[b#c]] d
    ///         ^^^
    /// ```
    CrossLinkDest,
    /// Whole character escape.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [string content][crate::construct::string] or
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`CharacterEscapeMarker`][Name::CharacterEscapeMarker],
    ///   [`CharacterEscapeValue`][Name::CharacterEscapeValue]
    /// * **Construct**:
    ///   [`character_escape`][crate::construct::character_escape]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a \- b
    ///       ^^
    /// ```
    CharacterEscape,
    /// Character escape marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterEscape`][Name::CharacterEscape]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_escape`][crate::construct::character_escape]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a \- b
    ///       ^
    /// ```
    CharacterEscapeMarker,
    /// Character escape value.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterEscape`][Name::CharacterEscape]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_escape`][crate::construct::character_escape]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a \- b
    ///        ^
    /// ```
    CharacterEscapeValue,
    /// Whole character reference.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [string content][crate::construct::string] or
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`CharacterReferenceMarker`][Name::CharacterReferenceMarker],
    ///   [`CharacterReferenceMarkerHexadecimal`][Name::CharacterReferenceMarkerHexadecimal],
    ///   [`CharacterReferenceMarkerNumeric`][Name::CharacterReferenceMarkerNumeric],
    ///   [`CharacterReferenceMarkerSemi`][Name::CharacterReferenceMarkerSemi],
    ///   [`CharacterReferenceValue`][Name::CharacterReferenceValue]
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///       ^^^^^   ^^^^^^^   ^^^^^^^^^
    /// ```
    CharacterReference,
    /// Character reference opening marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterReference`][Name::CharacterReference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///       ^       ^         ^
    /// ```
    CharacterReferenceMarker,
    /// Character reference hexadecimal numeric marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterReference`][Name::CharacterReference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///                           ^
    /// ```
    CharacterReferenceMarkerHexadecimal,
    /// Character reference numeric marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterReference`][Name::CharacterReference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///                ^         ^
    /// ```
    CharacterReferenceMarkerNumeric,
    /// Character reference closing marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterReference`][Name::CharacterReference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///           ^         ^           ^
    /// ```
    CharacterReferenceMarkerSemi,
    /// Character reference value.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CharacterReference`][Name::CharacterReference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`character_reference`][crate::construct::character_reference]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a &amp; b &#8800; c &#x1D306; d
    ///        ^^^      ^^^^       ^^^^^
    /// ```
    CharacterReferenceValue,
    /// Whole code (fenced).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`CodeFencedFence`][Name::CodeFencedFence],
    ///   [`CodeFlowChunk`][Name::CodeFlowChunk],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ````markdown
    /// > | ```js
    ///     ^^^^^
    /// > | console.log(1)
    ///     ^^^^^^^^^^^^^^
    /// > | ```
    ///     ^^^
    /// ````
    CodeFenced,
    /// A code (fenced) fence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeFenced`][Name::CodeFenced]
    /// * **Content model**:
    ///   [`CodeFencedFenceInfo`][Name::CodeFencedFenceInfo],
    ///   [`CodeFencedFenceMeta`][Name::CodeFencedFenceMeta],
    ///   [`CodeFencedFenceSequence`][Name::CodeFencedFenceSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ````markdown
    /// > | ```js
    ///     ^^^^^
    ///   | console.log(1)
    /// > | ```
    ///     ^^^
    /// ````
    CodeFencedFence,
    /// A code (fenced) fence info word.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeFencedFence`][Name::CodeFencedFence]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ````markdown
    /// > | ```js
    ///        ^^
    ///   | console.log(1)
    ///   | ```
    /// ````
    CodeFencedFenceInfo,
    /// A code (fenced) fence meta string.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeFencedFence`][Name::CodeFencedFence]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ````markdown
    /// > | ```js highlight="1"
    ///           ^^^^^^^^^^^^^
    ///   | console.log(1)
    ///   | ```
    /// ````
    CodeFencedFenceMeta,
    /// A code (fenced) fence sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeFencedFenceSequence`][Name::CodeFencedFenceSequence]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ````markdown
    /// > | ```js
    ///     ^^^
    ///   | console.log(1)
    /// > | ```
    ///     ^^^
    /// ````
    CodeFencedFenceSequence,
    /// A code (fenced, indented) chunk.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeFenced`][Name::CodeFenced],
    ///   [`CodeIndented`][Name::CodeIndented]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow],
    ///   [`code_indented`][crate::construct::code_indented]
    ///
    /// ## Example
    ///
    /// ````markdown
    ///   | ```js
    /// > | console.log(1)
    ///     ^^^^^^^^^^^^^^
    ///   | ```
    /// ````
    ///
    /// ```markdown
    /// > | ␠␠␠␠console.log(1)
    ///         ^^^^^^^^^^^^^^
    /// ```
    CodeFlowChunk,
    /// Whole code (indented).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`CodeFlowChunk`][Name::CodeFlowChunk],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// ␠␠␠␠console.log(1)
    /// ^^^^^^^^^^^^^^^^^^
    /// ```
    CodeIndented,
    /// Whole code (text).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`CodeTextData`][Name::CodeTextData],
    ///   [`CodeTextSequence`][Name::CodeTextSequence],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a `b` c
    ///       ^^^
    /// ```
    CodeText,
    /// Code (text) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeText`][Name::CodeText]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a `b` c
    ///        ^
    /// ```
    CodeTextData,
    /// Code (text) sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CodeText`][Name::CodeText]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a `b` c
    ///       ^ ^
    /// ```
    /// Whole comment.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`CommentSequence`][Name::CommentSequence],
    ///   [`CommentText`][Name::CommentText],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a %%b%% c
    ///       ^^^^^
    /// ```
    Comment,
    /// Comment (flow) chunk.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CommentFlow`][Name::CommentFlow]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | %%
    /// > | a
    ///     ^
    ///   | %%
    /// ```
    CommentChunk,
    /// Comment (flow) fence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`CommentFlow`][Name::CommentFlow]
    /// * **Content model**:
    ///   [`CommentSequence`][Name::CommentSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | %%
    ///     ^^
    ///   | a
    /// > | %%
    ///     ^^
    /// ```
    CommentFence,
    /// Whole comment (flow).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`CommentChunk`][Name::CommentChunk],
    ///   [`CommentFence`][Name::CommentFence],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | %%
    ///     ^^
    /// > | a
    ///     ^
    /// > | %%
    ///     ^^
    /// ```
    CommentFlow,
    /// Comment sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Comment`][Name::Comment]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a %%b%% c
    ///       ^^ ^^
    /// ```
    CommentSequence,
    /// Comment text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Comment`][Name::Comment]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`comment`][crate::construct::comment]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a %%b%% c
    ///         ^
    /// ```
    CommentText,
    CodeTextSequence,
    /// Content.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [content][crate::construct::content]
    /// * **Construct**:
    ///   [`content`][crate::construct::content]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b
    ///     ^^^^^^
    /// > | c.
    ///     ^^
    /// ```
    Content,
    /// Data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [string content][crate::construct::string],
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`data`][crate::construct::partial_data]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | aa *bb* cc
    ///     ^^^ ^^ ^^^
    /// ```
    Data,
    /// Whole definition.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`DefinitionMarker`][Name::DefinitionMarker],
    ///   [`DefinitionLabel`][Name::DefinitionLabel],
    ///   [`DefinitionDestination`][Name::DefinitionDestination],
    ///   [`DefinitionTitle`][Name::DefinitionTitle],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`definition`][crate::construct::definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///     ^^^^^^^^^^
    /// ```
    Definition,
    /// Whole definition destination.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Definition`][Name::Definition]
    /// * **Content model**:
    ///   [`DefinitionDestinationLiteral`][Name::DefinitionDestinationLiteral],
    ///   [`DefinitionDestinationRaw`][Name::DefinitionDestinationRaw]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///          ^
    /// > | [a]: <b> "c"
    ///          ^^^
    /// ```
    DefinitionDestination,
    /// Definition destination literal.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionDestination`][Name::DefinitionDestination]
    /// * **Content model**:
    ///   [`DefinitionDestinationLiteralMarker`][Name::DefinitionDestinationLiteralMarker],
    ///   [`DefinitionDestinationText`][Name::DefinitionDestinationText]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: <b> "c"
    ///          ^^^
    /// ```
    DefinitionDestinationLiteral,
    /// Definition destination literal marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionDestinationLiteral`][Name::DefinitionDestinationLiteral]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: <b> "c"
    ///          ^ ^
    /// ```
    DefinitionDestinationLiteralMarker,
    /// Definition destination raw.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionDestination`][Name::DefinitionDestination]
    /// * **Content model**:
    ///   [`DefinitionDestinationText`][Name::DefinitionDestinationText]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///          ^
    /// ```
    DefinitionDestinationRaw,
    /// Definition destination data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionDestinationLiteral`][Name::DefinitionDestinationLiteral],
    ///   [`DefinitionDestinationRaw`][Name::DefinitionDestinationRaw]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///          ^
    /// > | [a]: <b> "c"
    ///           ^
    /// ```
    DefinitionDestinationText,
    /// Whole definition label.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Definition`][Name::Definition]
    /// * **Content model**:
    ///   [`DefinitionLabelMarker`][Name::DefinitionLabelMarker],
    ///   [`DefinitionLabelText`][Name::DefinitionLabelText],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///     ^^^
    /// ```
    DefinitionLabel,
    /// Definition label marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionLabel`][Name::DefinitionLabel]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///     ^ ^
    /// ```
    DefinitionLabelMarker,
    /// Definition label data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionLabel`][Name::DefinitionLabel]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///      ^
    /// ```
    DefinitionLabelText,
    /// Definition marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Definition`][Name::Definition],
    ///   [`FootnoteDefinition`][Name::FootnoteDefinition]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`definition`][crate::construct::definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///        ^
    /// ```
    DefinitionMarker,
    /// Whole definition title.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Definition`][Name::Definition]
    /// * **Content model**:
    ///   [`DefinitionTitleMarker`][Name::DefinitionTitleMarker],
    ///   [`DefinitionTitleText`][Name::DefinitionTitleText],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///            ^^^
    /// ```
    DefinitionTitle,
    /// Definition title marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionTitle`][Name::DefinitionTitle]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///            ^ ^
    /// ```
    DefinitionTitleMarker,
    /// Definition title data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`DefinitionTitle`][Name::DefinitionTitle]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [a]: b "c"
    ///             ^
    /// ```
    DefinitionTitleText,
    /// Emphasis.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`EmphasisSequence`][Name::EmphasisSequence],
    ///   [`EmphasisText`][Name::EmphasisText]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | *a*
    ///     ^^^
    /// ```
    Emphasis,
    /// Emphasis sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Emphasis`][Name::Emphasis]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | *a*
    ///     ^ ^
    /// ```
    EmphasisSequence,
    /// Emphasis text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Emphasis`][Name::Emphasis]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | *a*
    ///      ^
    /// ```
    EmphasisText,
    /// Whole frontmatter.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [document content][crate::construct::document]
    /// * **Content model**:
    ///   [`FrontmatterFence`][Name::FrontmatterFence],
    ///   [`FrontmatterChunk`][Name::FrontmatterChunk],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`frontmatter`][crate::construct::frontmatter]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ---
    ///     ^^^
    /// > | title: Neptune
    ///     ^^^^^^^^^^^^^^
    /// > | ---
    ///     ^^^
    /// ```
    Frontmatter,
    /// Frontmatter chunk.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Frontmatter`][Name::Frontmatter]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`frontmatter`][crate::construct::frontmatter]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | ---
    /// > | title: Neptune
    ///     ^^^^^^^^^^^^^^
    ///   | ---
    /// ```
    FrontmatterChunk,
    /// Frontmatter fence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Frontmatter`][Name::Frontmatter]
    /// * **Content model**:
    ///   [`FrontmatterSequence`][Name::FrontmatterSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`frontmatter`][crate::construct::frontmatter]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ---
    ///     ^^^
    ///   | title: Neptune
    /// > | ---
    ///     ^^^
    /// ```
    FrontmatterFence,
    /// Frontmatter sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FrontmatterFence`][Name::FrontmatterFence]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`frontmatter`][crate::construct::frontmatter]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ---
    ///     ^^^
    ///   | title: Neptune
    /// > | ---
    ///     ^^^
    /// ```
    FrontmatterSequence,
    /// GFM extension: email autolink.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void.
    /// * **Construct**:
    ///   [`autolink_literal`][crate::construct::autolink_literal]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | context@example.com
    ///     ^^^^^^^^^^^^^^^^^^^
    /// ```
    AutolinkLiteralEmail,
    /// GFM extension: email autolink w/ explicit `mailto`.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void.
    /// * **Construct**:
    ///   [`autolink_literal`][crate::construct::autolink_literal]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | mailto:context@example.com
    ///     ^^^^^^^^^^^^^^^^^^^^^^^^^^
    /// ```
    AutolinkLiteralMailto,
    /// GFM extension: autolink w/ protocol.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void.
    /// * **Construct**:
    ///   [`autolink_literal`][crate::construct::autolink_literal]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | https://example.com
    ///     ^^^^^^^^^^^^^^^^^^^
    /// ```
    AutolinkLiteralProtocol,
    /// GFM extension: autolink w/ www.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void.
    /// * **Construct**:
    ///   [`autolink_literal`][crate::construct::autolink_literal]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | www.example.com
    ///     ^^^^^^^^^^^^^^^
    /// ```
    AutolinkLiteralWww,
    /// GFM extension: email autolink w/ explicit `xmpp`.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void.
    /// * **Construct**:
    ///   [`autolink_literal`][crate::construct::autolink_literal]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | mailto:a@b.c/d
    ///     ^^^^^^^^^^^^^^
    /// ```
    AutolinkLiteralXmpp,
    /// GFM extension: whole footnote call.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`Label`][Name::Label]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [^b] c
    ///       ^^^^
    /// ```
    FootnoteCall,
    /// GFM extension: label start (footnote).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Label`][Name::Label]
    /// * **Content model**:
    ///   [`FootnoteCallMarker`][Name::FootnoteCallMarker],
    ///   [`LabelMarker`][Name::LabelMarker]
    /// * **Construct**:
    ///   [`label_start_footnote`][crate::construct::label_start_footnote]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [^b] c
    ///       ^^
    /// ```
    FootnoteCallLabel,
    /// GFM extension: label start (footnote) marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteCallLabel`][Name::FootnoteCallLabel]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label_start_footnote`][crate::construct::label_start_footnote]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [^b] c
    ///        ^
    /// ```
    FootnoteCallMarker,
    /// GFM extension: whole footnote definition.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [document content][crate::construct::document]
    /// * **Content model**:
    ///   [`FootnoteDefinitionPrefix`][Name::FootnoteDefinitionPrefix],
    ///   [document content][crate::construct::flow]
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///     ^^^^^^^
    /// ```
    FootnoteDefinition,
    /// GFM extension: footnote definition prefix.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteDefinition`][Name::FootnoteDefinition]
    /// * **Content model**:
    ///   [`DefinitionMarker`][Name::DefinitionMarker],
    ///   [`FootnoteDefinitionLabel`][Name::FootnoteDefinitionLabel],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///     ^^^^^^
    /// ```
    FootnoteDefinitionPrefix,
    /// GFM extension: footnote definition label.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteDefinitionPrefix`][Name::FootnoteDefinitionPrefix]
    /// * **Content model**:
    ///   [`FootnoteDefinitionLabelMarker`][Name::FootnoteDefinitionLabelMarker],
    ///   [`FootnoteDefinitionLabelText`][Name::FootnoteDefinitionLabelText],
    ///   [`FootnoteDefinitionMarker`][Name::FootnoteDefinitionMarker]
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///     ^^^^
    /// ```
    FootnoteDefinitionLabel,
    /// GFM extension: footnote definition label marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteDefinitionLabel`][Name::FootnoteDefinitionLabel]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///     ^  ^
    FootnoteDefinitionLabelMarker,
    /// GFM extension: footnote definition label string.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteDefinitionLabel`][Name::FootnoteDefinitionLabel]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///       ^
    FootnoteDefinitionLabelText,
    /// GFM extension: footnote definition marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`FootnoteDefinitionLabel`][Name::FootnoteDefinitionLabel]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`footnote_definition`][crate::construct::footnote_definition]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | [^a]: b
    ///      ^
    FootnoteDefinitionMarker,
    /// GFM extension: Strikethrough.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`StrikethroughSequence`][Name::StrikethroughSequence],
    ///   [`StrikethroughText`][Name::StrikethroughText]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ~a~
    ///     ^^^
    /// ```
    Strikethrough,
    /// GFM extension: Strikethrough sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Strikethrough`][Name::Strikethrough]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ~a~
    ///     ^ ^
    /// ```
    StrikethroughSequence,
    /// GFM extension: Strikethrough text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Strikethrough`][Name::Strikethrough]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ~a~
    ///      ^
    /// ```
    StrikethroughText,
    /// Whole insertion (`++a++`).
    ///
    /// Formed by the [`attention`][crate::construct::attention] resolver
    /// from [`AttentionSequence`][Name::AttentionSequence]s.
    Insertion,
    /// Marker of a insertion.
    InsertionSequence,
    /// Content of a insertion.
    InsertionText,
    /// Whole mark (`==a==`).
    ///
    /// Formed by the [`attention`][crate::construct::attention] resolver
    /// from [`AttentionSequence`][Name::AttentionSequence]s.
    Mark,
    /// Marker of a mark.
    MarkSequence,
    /// Content of a mark.
    MarkText,
    /// Whole subscript (`~a~`).
    ///
    /// Formed by the [`attention`][crate::construct::attention] resolver
    /// from [`AttentionSequence`][Name::AttentionSequence]s.
    Subscript,
    /// Marker of a subscript.
    SubscriptSequence,
    /// Content of a subscript.
    SubscriptText,
    /// Whole superscript (`^a^`).
    ///
    /// Formed by the [`attention`][crate::construct::attention] resolver
    /// from [`AttentionSequence`][Name::AttentionSequence]s.
    Superscript,
    /// Marker of a superscript.
    SuperscriptSequence,
    /// Content of a superscript.
    SuperscriptText,
    /// GFM extension: Table.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`TableBody`][Name::TableBody],
    ///   [`TableHead`][Name::TableHead],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///     ^^^^^
    /// > | | - |
    ///     ^^^^^
    /// > | | b |
    ///     ^^^^^
    /// ```
    Table,
    /// GFM extension: Table body.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Table`][Name::Table]
    /// * **Content model**:
    ///   [`TableRow`][Name::TableRow],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a |
    ///   | | - |
    /// > | | b |
    ///     ^^^^^
    /// ```
    TableBody,
    /// GFM extension: Table cell.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableRow`][Name::TableRow]
    /// * **Content model**:
    ///   [`TableCellDivider`][Name::TableCellDivider],
    ///   [`TableCellText`][Name::TableCellText],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///     ^^^^^
    ///   | | - |
    /// > | | b |
    ///     ^^^^^
    /// ```
    TableCell,
    /// GFM extension: Table cell text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableCell`][Name::TableCell]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///       ^
    ///   | | - |
    /// > | | b |
    ///       ^
    /// ```
    TableCellText,
    /// GFM extension: Table cell divider.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableCell`][Name::TableCell]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///     ^   ^
    /// > | | - |
    ///     ^   ^
    /// > | | b |
    ///     ^   ^
    /// ```
    TableCellDivider,
    /// GFM extension: Table delimiter row.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableHead`][Name::TableHead]
    /// * **Content model**:
    ///   [`TableDelimiterCell`][Name::TableDelimiterCell]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a |
    /// > | | - |
    ///     ^^^^^
    ///   | | b |
    /// ```
    TableDelimiterRow,
    /// GFM extension: Table delimiter alignment marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableDelimiterCellValue`][Name::TableDelimiterCellValue]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a  |
    /// > | | :- |
    ///       ^
    ///   | | b  |
    /// ```
    TableDelimiterMarker,
    /// GFM extension: Table delimiter cell.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableDelimiterRow`][Name::TableDelimiterRow]
    /// * **Content model**:
    ///   [`TableCellDivider`][Name::TableCellDivider],
    ///   [`TableDelimiterCellValue`][Name::TableDelimiterCellValue],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a |
    /// > | | - |
    ///     ^^^^^
    ///   | | b |
    /// ```
    TableDelimiterCell,
    /// GFM extension: Table delimiter cell alignment.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableDelimiterCell`][Name::TableDelimiterCell]
    /// * **Content model**:
    ///   [`TableDelimiterMarker`][Name::TableDelimiterMarker],
    ///   [`TableDelimiterFiller`][Name::TableDelimiterFiller]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a |
    /// > | | - |
    ///       ^
    ///   | | b |
    /// ```
    TableDelimiterCellValue,
    /// GFM extension: Table delimiter filler.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableDelimiterCellValue`][Name::TableDelimiterCellValue]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | | a |
    /// > | | - |
    ///       ^
    ///   | | b |
    /// ```
    TableDelimiterFiller,
    /// GFM extension: Table head.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Table`][Name::Table]
    /// * **Content model**:
    ///   [`TableRow`][Name::TableRow],
    ///   [`TableDelimiterRow`][Name::TableDelimiterRow],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///     ^^^^^
    /// > | | - |
    ///     ^^^^^
    ///   | | b |
    /// ```
    TableHead,
    /// GFM extension: Table row.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TableBody`][Name::TableBody],
    ///   [`TableHead`][Name::TableHead]
    /// * **Content model**:
    ///   [`TableCell`][Name::TableCell]
    /// * **Construct**:
    ///   [`table`][crate::construct::table]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | | a |
    ///     ^^^^^
    ///   | | - |
    /// > | | b |
    ///     ^^^^^
    /// ```
    TableRow,
    /// GFM extension: task list item check.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`TaskListItemMarker`][Name::TaskListItemMarker],
    ///   [`TaskListItemValueChecked`][Name::TaskListItemValueChecked],
    ///   [`TaskListItemValueUnchecked`][Name::TaskListItemValueUnchecked]
    /// * **Construct**:
    ///   [`task_list_item_check`][crate::construct::task_list_item_check]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * [x] y.
    ///       ^^^
    /// ```
    TaskListItemCheck,
    /// GFM extension: task list item check marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TaskListItemCheck`][Name::TaskListItemCheck]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`task_list_item_check`][crate::construct::task_list_item_check]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * [x] y.
    ///       ^ ^
    /// ```
    TaskListItemMarker,
    /// GFM extension: task list item value: checked.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TaskListItemCheck`][Name::TaskListItemCheck]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`task_list_item_check`][crate::construct::task_list_item_check]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * [x] y.
    ///        ^
    /// ```
    TaskListItemValueChecked,
    /// GFM extension: task list item value: unchecked.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`TaskListItemCheck`][Name::TaskListItemCheck]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`task_list_item_check`][crate::construct::task_list_item_check]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * [ ] z.
    ///        ^
    /// ```
    TaskListItemValueUnchecked,
    /// Whole hard break (escape).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`hard_break_escape`][crate::construct::hard_break_escape]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a\␊
    ///      ^
    /// > | b
    /// ```
    HardBreakEscape,
    /// Whole hard break (trailing).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`whitespace`][crate::construct::partial_whitespace]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a␠␠␊
    ///      ^^
    /// > | b
    /// ```
    HardBreakTail,
    /// Whole heading (atx).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`HeadingAtxSequence`][Name::HeadingAtxSequence],
    ///   [`HeadingAtxText`][Name::HeadingAtxText],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`heading_atx`][crate::construct::heading_atx]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | # alpha
    ///     ^^^^^^^
    /// ```
    HeadingAtx,
    /// Heading (atx) sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HeadingAtx`][Name::HeadingAtx]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`heading_atx`][crate::construct::heading_atx]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | # alpha
    ///     ^
    /// ```
    HeadingAtxSequence,
    /// Heading (atx) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HeadingAtx`][Name::HeadingAtx]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`heading_atx`][crate::construct::heading_atx]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | # alpha
    ///       ^^^^^
    /// ```
    HeadingAtxText,
    /// Whole heading (setext).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`HeadingSetextText`][Name::HeadingSetextText],
    ///   [`HeadingSetextUnderline`][Name::HeadingSetextUnderline],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`heading_setext`][crate::construct::heading_setext]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | alpha
    ///     ^^^^^
    /// > | =====
    ///     ^^^^^
    /// ```
    HeadingSetext,
    /// Heading (setext) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HeadingSetext`][Name::HeadingSetext]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`heading_setext`][crate::construct::heading_setext]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | alpha
    ///     ^^^^^
    ///   | =====
    /// ```
    HeadingSetextText,
    /// Heading (setext) underline.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HeadingSetext`][Name::HeadingSetext]
    /// * **Content model**:
    ///   [`HeadingSetextUnderlineSequence`][Name::HeadingSetextUnderlineSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`heading_setext`][crate::construct::heading_setext]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | alpha
    /// > | =====
    ///     ^^^^^
    /// ```
    HeadingSetextUnderline,
    /// Heading (setext) underline sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HeadingSetext`][Name::HeadingSetext]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`heading_setext`][crate::construct::heading_setext]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | alpha
    /// > | =====
    ///     ^^^^^
    /// ```
    HeadingSetextUnderlineSequence,
    /// Whole html (flow).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`HtmlFlowData`][Name::HtmlFlowData],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`html_flow`][crate::construct::html_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <div>
    ///     ^^^^^
    /// ```
    HtmlFlow,
    /// HTML (flow) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HtmlFlow`][Name::HtmlFlow]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`html_flow`][crate::construct::html_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | <div>
    ///     ^^^^^
    /// ```
    HtmlFlowData,
    /// Whole html (text).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`HtmlTextData`][Name::HtmlTextData],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`html_text`][crate::construct::html_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a <b> c
    ///       ^^^
    /// ```
    HtmlText,
    /// HTML (text) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`HtmlText`][Name::HtmlText]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`html_text`][crate::construct::html_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a <b> c
    ///       ^^^
    /// ```
    HtmlTextData,
    /// Image.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`Label`][Name::Label],
    ///   [`Resource`][Name::Resource],
    ///   [`Reference`][Name::Reference]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b] c
    ///       ^^^^
    /// > | a ![b][c] d
    ///       ^^^^^^^
    /// > | a ![b](c) d
    ///       ^^^^^^^
    /// ```
    Image,
    /// Label.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Image`][Name::Image],
    ///   [`Link`][Name::Link]
    /// * **Content model**:
    ///   [`LabelImage`][Name::LabelImage],
    ///   [`LabelLink`][Name::LabelLink],
    ///   [`LabelEnd`][Name::LabelEnd],
    ///   [`LabelText`][Name::LabelText]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [b] c
    ///       ^^^
    /// > | a ![b][c] d
    ///       ^^^^
    /// > | a [b](c) d
    ///       ^^^
    /// ```
    Label,
    /// Label end.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Label`][Name::Label]
    /// * **Content model**:
    ///   [`LabelMarker`][Name::LabelMarker]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c) d
    ///          ^
    /// > | a [b](c) d
    ///         ^
    /// ```
    LabelEnd,
    /// Label start (image).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Label`][Name::Label]
    /// * **Content model**:
    ///   [`LabelImageMarker`][Name::LabelImageMarker],
    ///   [`LabelMarker`][Name::LabelMarker]
    /// * **Construct**:
    ///   [`label_start_image`][crate::construct::label_start_image]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c) d
    ///       ^^
    /// ```
    LabelImage,
    /// Label start (image) marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`LabelImage`][Name::LabelImage]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label_start_image`][crate::construct::label_start_image]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c) d
    ///       ^
    /// ```
    LabelImageMarker,
    /// Label start (link).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Label`][Name::Label]
    /// * **Content model**:
    ///   [`LabelMarker`][Name::LabelMarker]
    /// * **Construct**:
    ///   [`label_start_link`][crate::construct::label_start_link]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [b](c) d
    ///       ^
    /// ```
    LabelLink,
    /// Label marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`LabelImage`][Name::LabelImage],
    ///   [`LabelLink`][Name::LabelLink],
    ///   [`LabelEnd`][Name::LabelEnd]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label_start_image`][crate::construct::label_start_image],
    ///   [`label_start_link`][crate::construct::label_start_link],
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c) d
    ///        ^ ^
    /// > | a [b](c) d
    ///       ^ ^
    /// ```
    LabelMarker,
    /// Label text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Label`][Name::Label]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [b] c
    ///        ^
    /// > | a ![b][c] d
    ///         ^
    /// > | a [b](c) d
    ///        ^
    /// ```
    LabelText,
    /// Line ending.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   basically everywhere
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   n/a
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a␊
    ///      ^
    ///   | b
    /// ```
    LineEnding,
    /// Link.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`Label`][Name::Label],
    ///   [`Resource`][Name::Resource],
    ///   [`Reference`][Name::Reference]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a [b] c
    ///       ^^^
    /// > | a [b][c] d
    ///       ^^^^^^
    /// > | a [b](c) d
    ///       ^^^^^^
    /// ```
    Link,
    /// List item.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ListOrdered`][Name::ListOrdered],
    ///   [`ListUnordered`][Name::ListUnordered]
    /// * **Content model**:
    ///   [`ListItemPrefix`][Name::ListItemPrefix],
    ///   [flow content][crate::construct::flow]
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * a
    ///     ^^^
    /// > | 1. b
    ///     ^^^^
    /// ```
    ListItem,
    /// List item (marker).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ListItemPrefix`][Name::ListItemPrefix]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * a
    ///     ^
    /// > | 1. b
    ///      ^
    /// ```
    ListItemMarker,
    /// List item (prefix).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ListItem`][Name::ListItem]
    /// * **Content model**:
    ///   [`ListItemMarker`][Name::ListItemMarker],
    ///   [`ListItemValue`][Name::ListItemValue],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * a
    ///     ^^
    /// > |   b
    ///     ^^
    /// ```
    ListItemPrefix,
    /// List item (value).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ListItemPrefix`][Name::ListItemPrefix]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | 1. b
    ///     ^
    /// ```
    ListItemValue,
    /// List (ordered).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [document content][crate::construct::document]
    /// * **Content model**:
    ///   [`BlankLineEnding`][Name::BlankLineEnding],
    ///   [`BlockQuotePrefix`][Name::BlockQuotePrefix],
    ///   [`ListItem`][Name::ListItem],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | 1. a
    ///     ^^^^
    /// > | 2. b
    ///     ^^^^
    /// ```
    ListOrdered,
    /// List (unordered).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [document content][crate::construct::document]
    /// * **Content model**:
    ///   [`BlankLineEnding`][Name::BlankLineEnding],
    ///   [`BlockQuotePrefix`][Name::BlockQuotePrefix],
    ///   [`ListItem`][Name::ListItem],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`list item`][crate::construct::list_item]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * a
    ///     ^^^
    /// > | * b
    ///     ^^^
    /// ```
    ListUnordered,
    /// Whole math (flow).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`MathFlowFence`][Name::MathFlowFence],
    ///   [`MathFlowChunk`][Name::MathFlowChunk],
    ///   [`LineEnding`][Name::LineEnding],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | $$
    ///     ^^
    /// > | \frac{1}{2}
    ///     ^^^^^^^^^^^
    /// > | $$
    ///     ^^
    /// ```
    MathFlow,
    /// A math (flow) fence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathFlow`][Name::MathFlow]
    /// * **Content model**:
    ///   [`MathFlowFenceMeta`][Name::MathFlowFenceMeta],
    ///   [`MathFlowFenceSequence`][Name::MathFlowFenceSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | $$
    ///     ^^
    ///   | \frac{1}{2}
    /// > | $$
    ///     ^^
    /// ```
    MathFlowFence,
    /// A math (flow) fence meta string.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathFlowFence`][Name::MathFlowFence]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | $$alpha bravo
    ///       ^^^^^^^^^^^
    ///   | \frac{1}{2}
    ///   | $$
    /// ```
    MathFlowFenceMeta,
    /// A math (flow) fence sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathFlowFenceSequence`][Name::MathFlowFenceSequence]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | $$
    ///     ^^
    ///   | \frac{1}{2}
    /// > | $$
    ///     ^^
    /// ```
    MathFlowFenceSequence,
    /// A math (flow) chunk.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathFlow`][Name::MathFlow]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_flow`][crate::construct::raw_flow]
    ///
    /// ## Example
    ///
    /// ```markdown
    ///   | $$
    /// > | \frac{1}{2}
    ///     ^^^^^^^^^^^
    ///   | $$
    /// ```
    MathFlowChunk,
    /// Whole math (text).
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`MathTextData`][Name::MathTextData],
    ///   [`MathTextSequence`][Name::MathTextSequence],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a $b$ c
    ///       ^^^
    /// ```
    MathText,
    /// Math (text) data.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathText`][Name::MathText]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a `b` c
    ///        ^
    /// ```
    MathTextData,
    /// Math (text) sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`MathText`][Name::MathText]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`raw_text`][crate::construct::raw_text]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a $b$ c
    ///       ^ ^
    /// ```
    MathTextSequence,
    /// Paragraph.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [content][crate::construct::content]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`paragraph`][crate::construct::paragraph]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a b
    ///     ^^^
    /// > | c.
    ///     ^^
    /// ```
    Paragraph,
    /// Reference.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Image`][Name::Image],
    ///   [`Link`][Name::Link]
    /// * **Content model**:
    ///   [`ReferenceMarker`][Name::ReferenceMarker],
    ///   [`ReferenceText`][Name::ReferenceText]
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b][c] d
    ///           ^^^
    /// ```
    Reference,
    /// Reference marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Reference`][Name::Reference]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b][c] d
    ///           ^ ^
    /// ```
    ReferenceMarker,
    /// Reference string.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Reference`][Name::Reference]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`label`][crate::construct::partial_label]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b][c] d
    ///            ^
    /// ```
    ReferenceText,
    /// Resource.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Image`][Name::Image],
    ///   [`Link`][Name::Link]
    /// * **Content model**:
    ///   [`ResourceMarker`][Name::ResourceMarker],
    ///   [`ResourceDestination`][Name::ResourceDestination],
    ///   [`ResourceTitle`][Name::ResourceTitle],
    ///   [`SpaceOrTab`][Name::SpaceOrTab],
    ///   [`LineEnding`][Name::LineEnding]
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c "d") e
    ///           ^^^^^^^
    /// > | a [b](c) d
    ///          ^^^
    /// ```
    Resource,
    /// Resource destination.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Resource`][Name::Resource]
    /// * **Content model**:
    ///   [`ResourceDestinationLiteral`][Name::ResourceDestinationLiteral],
    ///   [`ResourceDestinationRaw`][Name::ResourceDestinationRaw]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c "d") e
    ///            ^
    /// ```
    ResourceDestination,
    /// Resource destination literal.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceDestination`][Name::ResourceDestination]
    /// * **Content model**:
    ///   [`ResourceDestinationLiteralMarker`][Name::ResourceDestinationLiteralMarker],
    ///   [`ResourceDestinationText`][Name::ResourceDestinationText]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///            ^^^
    /// ```
    ResourceDestinationLiteral,
    /// Resource destination literal marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceDestinationLiteral`][Name::ResourceDestinationLiteral]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///            ^ ^
    /// ```
    ResourceDestinationLiteralMarker,
    /// Resource destination raw.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceDestination`][Name::ResourceDestination]
    /// * **Content model**:
    ///   [`ResourceDestinationText`][Name::ResourceDestinationText]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c "d") e
    ///            ^
    /// ```
    ResourceDestinationRaw,
    /// Resource destination raw.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceDestinationLiteral`][Name::ResourceDestinationLiteral],
    ///   [`ResourceDestinationRaw`][Name::ResourceDestinationRaw]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`destination`][crate::construct::partial_destination]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///             ^
    /// > | a ![b](c "d") e
    ///            ^
    /// ```
    ResourceDestinationText,
    /// Resource marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Resource`][Name::Resource]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`label_end`][crate::construct::label_end]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](c "d") e
    ///           ^     ^
    /// ```
    ResourceMarker,
    /// Resource title.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Resource`][Name::Resource]
    /// * **Content model**:
    ///   [`ResourceTitleMarker`][Name::ResourceTitleMarker],
    ///   [`ResourceTitleText`][Name::ResourceTitleText]
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///                ^^^
    /// ```
    ResourceTitle,
    /// Resource title marker.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceTitle`][Name::ResourceTitle]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///                ^ ^
    /// ```
    ResourceTitleMarker,
    /// Resource title string.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ResourceTitle`][Name::ResourceTitle]
    /// * **Content model**:
    ///   [string content][crate::construct::string]
    /// * **Construct**:
    ///   [`title`][crate::construct::partial_title]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | a ![b](<c> "d") e
    ///                 ^
    /// ```
    ResourceTitleText,
    /// Space or tab.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   basically everywhere
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   n/a
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | ␠* * *␠
    ///     ^ ^ ^ ^
    /// ```
    SpaceOrTab,
    /// The indentation a list item's continuation line consumes
    /// (micromark `listItemIndent`).
    ListItemIndent,
    /// The indentation a footnote definition's continuation line consumes
    /// (micromark `FootnoteDefinitionIndent`).
    FootnoteDefinitionIndent,
    /// Strong.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [text content][crate::construct::text]
    /// * **Content model**:
    ///   [`StrongSequence`][Name::StrongSequence],
    ///   [`StrongText`][Name::StrongText]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | **a**
    ///     ^^^^^
    /// ```
    Strong,
    /// Strong sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Strong`][Name::Strong]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | **a**
    ///     ^^ ^^
    /// ```
    StrongSequence,
    /// Strong text.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`Strong`][Name::Strong]
    /// * **Content model**:
    ///   [text content][crate::construct::text]
    /// * **Construct**:
    ///   [`attention`][crate::construct::attention]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | **a**
    ///       ^
    /// ```
    StrongText,
    /// Whole thematic break.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [flow content][crate::construct::flow]
    /// * **Content model**:
    ///   [`ThematicBreakSequence`][Name::ThematicBreakSequence],
    ///   [`SpaceOrTab`][Name::SpaceOrTab]
    /// * **Construct**:
    ///   [`thematic_break`][crate::construct::thematic_break]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * * *
    ///     ^^^^^
    /// ```
    ThematicBreak,
    /// Thematic break sequence.
    ///
    /// ## Info
    ///
    /// * **Context**:
    ///   [`ThematicBreak`][Name::ThematicBreak]
    /// * **Content model**:
    ///   void
    /// * **Construct**:
    ///   [`thematic_break`][crate::construct::thematic_break]
    ///
    /// ## Example
    ///
    /// ```markdown
    /// > | * * *
    ///     ^ ^ ^
    /// ```
    ThematicBreakSequence,
}

/// What an event is to the AST compiler.
///
/// Every [`Name`][] has exactly one role, so a new name has to be placed
/// here, and a new block or inline has to be compiled.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Role {
    /// A block node.
    Block(Block),
    /// An inline node, or text in one.
    Inline(Inline),
    /// Whitespace and line endings between or inside nodes.
    Space(Space),
    /// Read by the node it is in, or not compiled.
    Part,
}

/// Blocks.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Block {
    Paragraph,
    HeadingAtx,
    HeadingSetext,
    ThematicBreak,
    CodeIndented,
    CodeFenced,
    HtmlFlow,
    Quote,
    List,
    Definition,
    Table,
    Footnote,
    MathFlow,
    Frontmatter,
    Comment,
}

/// Inlines.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Inline {
    Data,
    CharacterEscape,
    CharacterReference,
    HardBreakTail,
    HardBreakEscape,
    Emphasis,
    Strong,
    Strikethrough,
    Insertion,
    Mark,
    Subscript,
    Superscript,
    CodeText,
    MathText,
    HtmlText,
    Comment,
    CrossLink,
    Autolink,
    Literal,
    Media,
    FootnoteCall,
}

/// Whitespace and line endings.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Space {
    Whitespace,
    LineEnding,
    BlankLineEnding,
}

impl Name {
    /// The role of this name.
    #[allow(clippy::too_many_lines)]
    pub fn role(self) -> Role {
        match self {
            Name::Paragraph => Role::Block(Block::Paragraph),
            Name::HeadingAtx => Role::Block(Block::HeadingAtx),
            Name::HeadingSetext => Role::Block(Block::HeadingSetext),
            Name::ThematicBreak => Role::Block(Block::ThematicBreak),
            Name::CodeIndented => Role::Block(Block::CodeIndented),
            Name::CodeFenced => Role::Block(Block::CodeFenced),
            Name::HtmlFlow => Role::Block(Block::HtmlFlow),
            Name::BlockQuote => Role::Block(Block::Quote),
            Name::ListOrdered | Name::ListUnordered => Role::Block(Block::List),
            Name::Definition => Role::Block(Block::Definition),
            Name::Table => Role::Block(Block::Table),
            Name::FootnoteDefinition => Role::Block(Block::Footnote),
            Name::MathFlow => Role::Block(Block::MathFlow),
            Name::Frontmatter => Role::Block(Block::Frontmatter),
            Name::Data => Role::Inline(Inline::Data),
            Name::CharacterEscape => Role::Inline(Inline::CharacterEscape),
            Name::CharacterReference => Role::Inline(Inline::CharacterReference),
            Name::HardBreakTail => Role::Inline(Inline::HardBreakTail),
            Name::HardBreakEscape => Role::Inline(Inline::HardBreakEscape),
            Name::Emphasis => Role::Inline(Inline::Emphasis),
            Name::Strong => Role::Inline(Inline::Strong),
            Name::Strikethrough => Role::Inline(Inline::Strikethrough),
            Name::Insertion => Role::Inline(Inline::Insertion),
            Name::Mark => Role::Inline(Inline::Mark),
            Name::Subscript => Role::Inline(Inline::Subscript),
            Name::Superscript => Role::Inline(Inline::Superscript),
            Name::CodeText => Role::Inline(Inline::CodeText),
            Name::MathText => Role::Inline(Inline::MathText),
            Name::HtmlText => Role::Inline(Inline::HtmlText),
            Name::Comment => Role::Inline(Inline::Comment),
            Name::CrossLink => Role::Inline(Inline::CrossLink),
            Name::CommentFlow => Role::Block(Block::Comment),
            Name::Autolink => Role::Inline(Inline::Autolink),
            Name::AutolinkLiteralProtocol
            | Name::AutolinkLiteralWww
            | Name::AutolinkLiteralEmail
            | Name::AutolinkLiteralMailto
            | Name::AutolinkLiteralXmpp => Role::Inline(Inline::Literal),
            Name::Link | Name::Image => Role::Inline(Inline::Media),
            Name::FootnoteCall => Role::Inline(Inline::FootnoteCall),
            Name::SpaceOrTab => Role::Space(Space::Whitespace),
            Name::LineEnding => Role::Space(Space::LineEnding),
            Name::BlankLineEnding => Role::Space(Space::BlankLineEnding),
            Name::AttentionSequence
            | Name::AutolinkEmail
            | Name::AutolinkMarker
            | Name::AutolinkProtocol
            | Name::BlockQuoteMarker
            | Name::BlockQuotePrefix
            | Name::ByteOrderMark
            | Name::CharacterEscapeMarker
            | Name::CharacterEscapeValue
            | Name::CharacterReferenceMarker
            | Name::CharacterReferenceMarkerHexadecimal
            | Name::CharacterReferenceMarkerNumeric
            | Name::CharacterReferenceMarkerSemi
            | Name::CharacterReferenceValue
            | Name::CodeFencedFence
            | Name::CodeFencedFenceInfo
            | Name::CodeFencedFenceMeta
            | Name::CodeFencedFenceSequence
            | Name::CodeFlowChunk
            | Name::CodeTextData
            | Name::CodeTextSequence
            | Name::Callout
            | Name::CalloutFold
            | Name::CalloutMarker
            | Name::CalloutTitle
            | Name::CalloutVariant
            | Name::CommentChunk
            | Name::CrossLinkLabel
            | Name::CrossLinkMarker
            | Name::CrossLinkSeparator
            | Name::CrossLinkDest
            | Name::CommentFence
            | Name::CommentSequence
            | Name::CommentText
            | Name::Content
            | Name::DefinitionDestination
            | Name::DefinitionDestinationLiteral
            | Name::DefinitionDestinationLiteralMarker
            | Name::DefinitionDestinationRaw
            | Name::DefinitionDestinationText
            | Name::DefinitionLabel
            | Name::DefinitionLabelMarker
            | Name::DefinitionLabelText
            | Name::DefinitionMarker
            | Name::DefinitionTitle
            | Name::DefinitionTitleMarker
            | Name::DefinitionTitleText
            | Name::EmphasisSequence
            | Name::EmphasisText
            | Name::FrontmatterChunk
            | Name::FrontmatterFence
            | Name::FrontmatterSequence
            | Name::FootnoteCallLabel
            | Name::FootnoteCallMarker
            | Name::FootnoteDefinitionPrefix
            | Name::FootnoteDefinitionLabel
            | Name::FootnoteDefinitionLabelMarker
            | Name::FootnoteDefinitionLabelText
            | Name::FootnoteDefinitionMarker
            | Name::StrikethroughSequence
            | Name::StrikethroughText
            | Name::InsertionSequence
            | Name::InsertionText
            | Name::MarkSequence
            | Name::MarkText
            | Name::SubscriptSequence
            | Name::SubscriptText
            | Name::SuperscriptSequence
            | Name::SuperscriptText
            | Name::TableBody
            | Name::TableCell
            | Name::TableCellText
            | Name::TableCellDivider
            | Name::TableDelimiterRow
            | Name::TableDelimiterMarker
            | Name::TableDelimiterCell
            | Name::TableDelimiterCellValue
            | Name::TableDelimiterFiller
            | Name::TableHead
            | Name::TableRow
            | Name::TaskListItemCheck
            | Name::TaskListItemMarker
            | Name::TaskListItemValueChecked
            | Name::TaskListItemValueUnchecked
            | Name::HeadingAtxSequence
            | Name::HeadingAtxText
            | Name::HeadingSetextText
            | Name::HeadingSetextUnderline
            | Name::HeadingSetextUnderlineSequence
            | Name::HtmlFlowData
            | Name::HtmlTextData
            | Name::Label
            | Name::LabelEnd
            | Name::LabelImage
            | Name::LabelImageMarker
            | Name::LabelLink
            | Name::LabelMarker
            | Name::LabelText
            | Name::ListItem
            | Name::ListItemMarker
            | Name::ListItemPrefix
            | Name::ListItemValue
            | Name::MathFlowFence
            | Name::MathFlowFenceMeta
            | Name::MathFlowFenceSequence
            | Name::MathFlowChunk
            | Name::MathTextData
            | Name::MathTextSequence
            | Name::Reference
            | Name::ReferenceMarker
            | Name::ReferenceText
            | Name::Resource
            | Name::ResourceDestination
            | Name::ResourceDestinationLiteral
            | Name::ResourceDestinationLiteralMarker
            | Name::ResourceDestinationRaw
            | Name::ResourceDestinationText
            | Name::ResourceMarker
            | Name::ResourceTitle
            | Name::ResourceTitleMarker
            | Name::ResourceTitleText
            | Name::ListItemIndent
            | Name::FootnoteDefinitionIndent
            | Name::StrongSequence
            | Name::StrongText
            | Name::ThematicBreakSequence => Role::Part,
        }
    }
}

/// List of void events, used to make sure everything is working well.
pub const VOID_EVENTS: [Name; 76] = [
    Name::AttentionSequence,
    Name::AutolinkEmail,
    Name::AutolinkMarker,
    Name::AutolinkProtocol,
    Name::BlankLineEnding,
    Name::BlockQuoteMarker,
    Name::ByteOrderMark,
    Name::CharacterEscapeMarker,
    Name::CharacterEscapeValue,
    Name::CharacterReferenceMarker,
    Name::CharacterReferenceMarkerHexadecimal,
    Name::CharacterReferenceMarkerNumeric,
    Name::CharacterReferenceMarkerSemi,
    Name::CharacterReferenceValue,
    Name::CodeFencedFenceSequence,
    Name::CodeFlowChunk,
    Name::CodeTextData,
    Name::CodeTextSequence,
    Name::Data,
    Name::DefinitionDestinationLiteralMarker,
    Name::DefinitionLabelMarker,
    Name::DefinitionMarker,
    Name::DefinitionTitleMarker,
    Name::EmphasisSequence,
    Name::FrontmatterChunk,
    Name::AutolinkLiteralEmail,
    Name::AutolinkLiteralProtocol,
    Name::AutolinkLiteralWww,
    Name::FootnoteCallMarker,
    Name::FootnoteDefinitionLabelMarker,
    Name::FootnoteDefinitionMarker,
    Name::StrikethroughSequence,
    Name::InsertionSequence,
    Name::MarkSequence,
    Name::SubscriptSequence,
    Name::SuperscriptSequence,
    Name::TableCellDivider,
    Name::TableDelimiterMarker,
    Name::TableDelimiterFiller,
    Name::TaskListItemMarker,
    Name::TaskListItemValueChecked,
    Name::TaskListItemValueUnchecked,
    Name::FrontmatterSequence,
    Name::HardBreakEscape,
    Name::HardBreakTail,
    Name::HeadingAtxSequence,
    Name::HeadingSetextUnderlineSequence,
    Name::HtmlFlowData,
    Name::HtmlTextData,
    Name::LabelImageMarker,
    Name::LabelMarker,
    Name::LineEnding,
    Name::ListItemMarker,
    Name::ListItemValue,
    Name::MathFlowFenceSequence,
    Name::MathFlowChunk,
    Name::MathTextData,
    Name::MathTextSequence,
    Name::CalloutFold,
    Name::CalloutMarker,
    Name::CalloutVariant,
    Name::CommentChunk,
    Name::CrossLinkLabel,
    Name::CrossLinkMarker,
    Name::CrossLinkSeparator,
    Name::CrossLinkDest,
    Name::CommentSequence,
    Name::CommentText,
    Name::ReferenceMarker,
    Name::ResourceMarker,
    Name::ResourceTitleMarker,
    Name::SpaceOrTab,
    Name::ListItemIndent,
    Name::FootnoteDefinitionIndent,
    Name::StrongSequence,
    Name::ThematicBreakSequence,
];

/// Embedded content type.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Content {
    /// Represents [flow content][crate::construct::flow].
    Flow,
    /// Represents [content][crate::construct::content].
    #[allow(clippy::enum_variant_names)]
    Content,
    /// Represents [string content][crate::construct::string].
    String,
    /// Represents [text content][crate::construct::text].
    Text,
}

/// Link to another event.
#[derive(Clone, Debug)]
pub struct Link {
    /// Previous event.
    pub previous: Option<usize>,
    /// Next event.
    pub next: Option<usize>,
    /// Content type.
    pub content: Content,
}

/// Place in the document.
///
/// The interface for the location in the document comes from unist
/// [`Point`](https://github.com/syntax-tree/unist#point).
#[derive(Clone, Debug)]
pub struct Point {
    /// 1-indexed line number.
    pub line: usize,
    /// 1-indexed column number.
    ///
    /// This is increased up to a tab stop for tabs.
    /// Some editors count tabs as 1 character, so this position is not the
    /// same as editors.
    pub column: usize,
    /// 0-indexed position in the document.
    ///
    /// Also an `index` into `bytes`.
    pub index: usize,
    /// Virtual step on the same `index`.
    pub vs: usize,
}

impl Point {
    /// Create a new point, that is shifted from the close earlier current
    /// point, to `index`.
    pub fn shift_to(&self, bytes: &[u8], index: usize) -> Point {
        let mut next = self.clone();
        debug_assert!(index > next.index, "expected to shift forward");

        while next.index < index {
            match bytes[next.index] {
                b'\n' | b'\r' => unreachable!("cannot move past line endings"),
                b'\t' => {
                    let remainder = next.column % TAB_SIZE;
                    let vs = if remainder == 0 {
                        0
                    } else {
                        TAB_SIZE - remainder
                    };
                    next.index += 1;
                    next.column += 1 + vs;
                }
                _ => {
                    next.index += 1;
                    next.column += 1;
                }
            }
        }

        next
    }
}

/// Event kinds.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Kind {
    /// The start of something.
    Enter,
    /// The end of something.
    Exit,
}

/// Something semantic happening somewhere.
#[derive(Clone, Debug)]
pub struct Event {
    /// Kind of event.
    pub kind: Kind,
    /// Name of event.
    pub name: Name,
    /// Place where this happens.
    pub point: Point,
    /// Link to another event.
    pub link: Option<Link>,
}
